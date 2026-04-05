#pragma once
// ============================================================================
// whiteboard_canvas.h -- Graph-Based Canvas Stitcher (simplified)
//
// Captures whiteboard content from a moving camera using:
//   1. Connected-component blob extraction per frame
//   2. Spatial + total-shape matching to find camera offset
//   3. Battle logic for overlap resolution (refresh/coexist)
//   4. Absence tracking for natural erasure
//
// Threading model:
//   Camera thread  -> ProcessFrame()  (queues work, non-blocking)
//   Worker thread  -> ProcessFrameInternal
//   UI thread      -> GetViewport()   (mutex, never stalls camera)
// ============================================================================

#include <opencv2/opencv.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>

class WhiteboardCanvasHelperClient;

enum class CanvasRenderMode : int {
    kStroke = 0,
    kRaw = 1,
};

enum class AlignmentScoreMode : int {
    kIoU = 0,
    kChamfer = 1,
    kLargestBlob = 2,
};

enum class NodeReplacementMode : int {
    kAlwaysReplace   = 0,  // Always replace matched canvas node with frame blob
    kIouThreshold    = 1,  // Replace only if mask IoU < 50%
    kPeriodicReplace = 2,  // Replace every N-th time a node is matched
    kLocationAverage = 3,  // Don't replace content, average centroid positions
};

// ---------------------------------------------------------------------------
// DrawingNode -- A single drawing entity on the canvas
// ---------------------------------------------------------------------------
static constexpr float kAbsenceScoreInitial = 0.0f;
static constexpr float kAbsenceScoreMax     = 20.0f;
static constexpr float kAbsenceScoreSeenThreshold = 1.0f;

struct DrawingNode {
    int id = -1;
    cv::Mat binary_mask;               // CV_8UC1, tight to bbox
    cv::Mat color_pixels;              // CV_8UC3, tight to bbox (raw mode)
    cv::Rect bbox_canvas;
    cv::Point2f centroid_canvas;
    std::vector<cv::Point> contour;    // relative to bbox origin
    double hu[7] = {};
    double area = 0.0;
    float  absence_score = kAbsenceScoreInitial;
    bool   has_crossed_absence_seen_threshold = false;
    int    last_seen_frame = 0;
    int    created_frame   = 0;
    bool   user_locked     = false;
    int    match_count     = 0;
    bool   duplicate_debug_marked = false;
    int    duplicate_debug_partner_id = -1;
    float  duplicate_debug_positional_overlap = 0.0f;
    float  duplicate_debug_centroid_iou = 0.0f;
    float  duplicate_debug_bbox_iou = 0.0f;
    float  duplicate_debug_shape_difference = 1.0f;
    int    duplicate_debug_reason_mask = 0;
};

// ---------------------------------------------------------------------------
// FrameBlob -- Transient per-frame extraction result
// ---------------------------------------------------------------------------
struct FrameBlob {
    cv::Rect     bbox;
    cv::Point2f  centroid;
    cv::Mat      binary_mask;    // CV_8UC1
    cv::Mat      color_pixels;   // CV_8UC3
    std::vector<cv::Point> contour;
    double       hu[7] = {};
    double       area  = 0.0;
    int          matched_node_id = -1;
    cv::Point2f  matched_offset{0, 0};
};

// ---------------------------------------------------------------------------
// SpatialIndex -- Grid-based spatial hash for fast proximity queries
// ---------------------------------------------------------------------------
class SpatialIndex {
public:
    explicit SpatialIndex(int cell_size = 200) : cell_size_(cell_size) {}

    void Insert(int id, cv::Point2f centroid) {
        int cx = (int)std::floor(centroid.x / cell_size_);
        int cy = (int)std::floor(centroid.y / cell_size_);
        cells_[CellKey(cx, cy)].push_back(id);
        positions_[id] = centroid;
    }

    void Remove(int id, cv::Point2f centroid) {
        int cx = (int)std::floor(centroid.x / cell_size_);
        int cy = (int)std::floor(centroid.y / cell_size_);
        auto it = cells_.find(CellKey(cx, cy));
        if (it != cells_.end()) {
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
            if (v.empty()) cells_.erase(it);
        }
        positions_.erase(id);
    }

    void Clear() { cells_.clear(); positions_.clear(); }

    std::vector<int> QueryRadius(cv::Point2f point, float radius) const {
        std::vector<int> result;
        float r2 = radius * radius;
        int min_cx = (int)std::floor((point.x - radius) / cell_size_);
        int max_cx = (int)std::floor((point.x + radius) / cell_size_);
        int min_cy = (int)std::floor((point.y - radius) / cell_size_);
        int max_cy = (int)std::floor((point.y + radius) / cell_size_);
        for (int cy = min_cy; cy <= max_cy; cy++) {
            for (int cx = min_cx; cx <= max_cx; cx++) {
                auto it = cells_.find(CellKey(cx, cy));
                if (it == cells_.end()) continue;
                for (int id : it->second) {
                    auto pit = positions_.find(id);
                    if (pit == positions_.end()) continue;
                    cv::Point2f d = pit->second - point;
                    if (d.x*d.x + d.y*d.y <= r2) result.push_back(id);
                }
            }
        }
        return result;
    }

private:
    int cell_size_;
    std::unordered_map<uint64_t, std::vector<int>> cells_;
    std::unordered_map<int, cv::Point2f> positions_;

    static uint64_t CellKey(int cx, int cy) {
        return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy;
    }
};

// ---------------------------------------------------------------------------
// WhiteboardGroup -- One continuous lecture session
// ---------------------------------------------------------------------------
struct WhiteboardGroup {
std::unordered_map<int, std::unique_ptr<DrawingNode>> nodes;
    int next_node_id = 0;
    SpatialIndex spatial_index{200};

    int stroke_min_px_x = 0, stroke_min_px_y = 0;
    int stroke_max_px_x = 512, stroke_max_px_y = 512;
    int raw_min_px_x = 0, raw_min_px_y = 0;
    int raw_max_px_x = 512, raw_max_px_y = 512;
    int fixed_render_height = 0;

    cv::Mat stroke_render_cache;
    bool    stroke_cache_dirty = true;
    cv::Mat raw_render_cache;
    bool    raw_cache_dirty    = true;

    std::unordered_set<int> user_deleted_ids;

    // Hard edges: adjacency list connecting nodes that were created in the same
    // frame when their centroids are nearby. When one node moves, all
    // transitively connected nodes move by the same delta.
    std::unordered_map<int, std::unordered_set<int>> hard_edges;
};

// ---------------------------------------------------------------------------
// Work item queued from camera thread to worker thread
// ---------------------------------------------------------------------------
struct CanvasWorkItem {
    cv::Mat frame;
    cv::Mat person_mask;
};

// ---------------------------------------------------------------------------
// WhiteboardCanvas
// ---------------------------------------------------------------------------
class WhiteboardCanvas {
public:
    WhiteboardCanvas();
    ~WhiteboardCanvas();

    // --- Frame scheduling ---
    void ProcessFrame(const cv::Mat& frame, const cv::Mat& person_mask);

    // --- Viewport rendering ---
    bool GetViewport(float panX, float panY, float zoom,
                     cv::Size viewSize, cv::Mat& out_frame);
    bool GetOverview(cv::Size viewSize, cv::Mat& out_frame);
    bool GetOverviewBlocking(cv::Size viewSize, cv::Mat& out_frame);

    // --- State control ---
    void Reset();
    bool HasContent() const;
    bool IsCanvasViewMode() const;
    void SetCanvasViewMode(bool mode);
    void SetRenderMode(CanvasRenderMode mode);
    CanvasRenderMode GetRenderMode() const;
    void SetDuplicateDebugMode(bool enabled);
    bool IsDuplicateDebugMode() const;
    bool IsRemoteProcess() const;
    cv::Size GetCanvasSize() const;
    uint64_t GetCanvasVersion() const {
        return canvas_version_.load(std::memory_order_relaxed);
    }
    void SyncRuntimeSettings();
    void InvalidateRenderCaches();
    void RecordRgbaCopyProfile(double /*duration_ms*/) {}  // no-op

    // --- Sub-canvas navigation ---
    int  GetSubCanvasCount() const;
    int  GetActiveSubCanvasIndex() const;
    void SetActiveSubCanvas(int idx);
    int  GetSortedSubCanvasIndex(int pos) const;
    int  GetSortedPosition(int idx) const;

    // --- Graph node access (for edit screen) ---
    int  GetGraphNodeCount() const;
    int  GetGraphNodes(float* buffer, int max_nodes) const;
    int  GetGraphHardEdges(int* buffer, int max_edges) const;
    int  GetGraphNodeNeighbors(int node_id, int* neighbors, int max_neighbors) const;
    bool CompareGraphNodes(int id_a, int id_b, float* result) const;
    bool CompareGraphNodesAtOffset(int id_a, int id_b, float dx, float dy,
                                   float* result) const;
    int  GetGraphNodeMasks(uint8_t* buffer, int max_bytes) const;
    bool MoveGraphNode(int node_id, float new_cx, float new_cy);
    bool DeleteGraphNode(int node_id);
    bool ApplyUserEdits(const int* delete_ids, int delete_count,
                        const float* moves, int move_count);
    int  LockAllGraphNodes();
    bool GetGraphCanvasBounds(int* bounds) const;
    int  GetGraphNodeContours(float* buffer, int max_floats) const;

    // --- Debug snapshots (stubs — kept for FFI compatibility) ---
    bool CaptureGraphDebugSnapshot(int /*slot*/, const cv::Mat& /*frame*/,
                                   const cv::Mat& /*person_mask*/) { return false; }
    int  GetGraphSnapshotNodeCount(int /*slot*/) const { return 0; }
    int  GetGraphSnapshotNodes(int /*slot*/, float* /*buf*/, int /*max*/) const { return 0; }
    bool GetGraphSnapshotCanvasBounds(int /*slot*/, int* /*bounds*/) const { return false; }
    int  GetGraphSnapshotNodeContours(int /*slot*/, float* /*buf*/, int /*max*/) const { return 0; }
    bool CompareGraphSnapshotNodes(int, int, int, int, float*) const { return false; }
    bool CombineGraphDebugSnapshots(int, int, int, int) { return false; }
    bool CopyGraphDebugSnapshot(int, int) { return false; }

private:
    // -----------------------------------------------------------------------
    // Tuning constants
    // -----------------------------------------------------------------------

    // --- Motion gate ---
    // Disable to process every frame regardless of camera shake (useful for debugging).
    static constexpr bool  kEnableMotionGate            = true;
    // Fraction of changed pixels above which a frame is considered too shaky and skipped.
    // Lower = stricter (fewer frames accepted, less ghosting). Higher = more frames processed including shaky ones.
    static constexpr float kMaxMotionFraction            = 0.05f;
    // Once a low-motion frame is accepted, the gate enters a lock state and stays there
    // until a later frame exceeds this motion fraction. The unlocking frame is also skipped.
    static constexpr float kOpenMotionGateLockFraction   = 0.09f;
    // Frame is downscaled to this long-edge size before computing motion. Smaller = faster.
    static const int       kMotionLongEdge               = 256;
    // Pixel absolute-difference value above which a pixel is counted as "changed" for motion.
    // Lower = more sensitive to small brightness shifts. Higher = only large changes count.
    static const int       kMotionPixelThreshold         = 10;

    // --- Bootstrapping ---
    // Minimum foreground pixel count in a frame before a new sub-canvas is created.
    // Raise to avoid creating a canvas on noise/reflections. Lower to start capturing sooner.
    static const int       kMinStrokePixelsForNewSC      = 500;
    // Graph must have at least this many nodes before matching is attempted.
    // Too low = matching starts before enough reference strokes exist, producing bad offsets.
    static const int       kStableGraphNodeThreshold     = 5;


    // --- Binarization ---
    // Adaptive Gaussian threshold block size (must be odd). Larger = smoother, more tolerant of
    // uneven lighting. Smaller = sharper boundary but more noise.
    static const int       kBinarizeBlockSize            = 25;
    // Constant subtracted from the adaptive threshold mean. Higher = only high-contrast marks
    // are binarized. Lower = thicker strokes captured (more sensitive).
    static const int       kBinarizeOffset               = 4;
    // Connected components smaller than this area (px²) at half-resolution are removed before
    // upscaling. Eliminates isolated noise specks from the binary mask.
    static const int       kBinarizeMinBlobArea          = 3;
    // Morphological dilation kernel size (NxN). Larger = wider strokes and more connected blobs.
    // Raise to join nearby strokes into one blob. Lower to keep strokes thin and separate.
    static const int       kDilationKernelSize           = 11;

    // --- Blob extraction ---
    // Connected components smaller than this (px²) are discarded as noise.
    // Lower = more small marks captured. Higher = cleaner graph but may miss fine writing.
    static const int       kMinContourArea               = 10;
    // Blobs whose long-edge / short-edge ratio exceeds this are rejected (e.g. board edges).
    // Raise if long horizontal strokes are being incorrectly filtered out.
    static constexpr float kMaxAllowedRectangle          = 115.0f;
    // Blobs whose width OR height exceeds this fraction of the frame height are rejected
    // (filters out whiteboard edges / frame borders captured as strokes).
    static constexpr float kMaxBlobDimensionFraction      = 0.650f;
    // When true, rejects blobs whose pixels overlap:
    //   (a) Left/right frame border strips of width max(1, frame_w/100).
    //       Note: top and bottom borders are NOT masked.
    //   (b) The lecturer bounding box expanded by max(1, frame_dim/100).
    // Disable to capture content that touches the side edges or the lecturer region.
    static constexpr bool  kEnableFrameStrokeRejectFilter = true;
    // Blobs wider than this (px) bypass the frame-stroke reject filter.
    // Large blobs (e.g. full diagram) should not be discarded just because they touch the border.
    static constexpr int   kFrameStrokeRejectMinWidth     = 300;

    // --- Matching (3-step pipeline) ---
    // Radius (px) for shape matching (step 2) after rough offset is applied.
    static constexpr float kShapeMatchSearchRadius       = 70.0f;
    // Max shape differentness for the global rough-matching pass.
    // This now prefers contour matching, with Shape Context taking precedence
    // when the local OpenCV build includes the contrib shape module.
    static constexpr float kGlobalShapeMaxDifference     = 0.50f;
    // Area ratio gate for coarse candidate filtering before shape comparison.
    static constexpr float kAreaRatioMin                 = 0.9f;
    // Minimum combined similarity to accept a shape match in step 2.
    // Rejects weak matches even if they're the "best" nearby candidate.
    // Live matching uses the same centroid-aligned shape geometry as duplicate detection.
    static constexpr float kShapeMatchMinScore           = 0.30f;
    // Max distance (px) of a match vector from the global mean before it is rejected.
    static constexpr float kOutlierVectorThreshold       = 5.0f;
    // Max distance (px) of a match vector from its width-third mean before it is rejected.
    static constexpr float kPartitionOutlierVectorThreshold = 1.0f;
    // Number of horizontal frame partitions used for independent motion estimation.
    static const int       kHorizontalMatchPartitions    = 3;
    // Radius (px) for the final shape-matching refinement pass (step 3).
    // This reuses the step-2 matcher with a tighter search window after offset refinement.
    static constexpr float kFinalShapeMatchSearchRadius  = 30.0f;
    // Minimum combined similarity to accept a shape match in step 3.
    // Keep this separate from step 2 so the final refinement gate can be tuned independently.
    static constexpr float kFinalShapeMatchMinScore      = 0.70f;
    // Minimum number of inlier matches required before new strokes are added to the graph.
    static const int       kMinMatchesForNewNode         = 5;

    // --- Replacement mode ---
    static constexpr NodeReplacementMode kReplacementMode = NodeReplacementMode::kAlwaysReplace;
    // IoU below which replacement happens (for kIouThreshold mode).
    static constexpr float kIouReplaceThreshold          = 0.50f;
    // Replace every N-th match (for kPeriodicReplace mode).
    static const int       kPeriodicReplaceInterval      = 5;
    // Blend factor for location averaging (for kLocationAverage mode). 0=keep old, 1=use new.
    static constexpr float kLocationAverageAlpha         = 0.5f;

    // --- Merge (duplicate insertion check) ---
    // Enable duplicate suppression when inserting new blobs into the graph.
    // Disable to always insert unmatched blobs as fresh nodes, even if they overlap existing ones.
    static constexpr bool  kEnableInsertMergeDeduplication = true;
    // Radius (px, canvas coords) to search for duplicate candidates around each new node's centroid.
    // Raise if near-duplicate nodes from nearby positions are not being caught.
    static constexpr float kMergeSearchRadiusPx          = 60.0f;
    // Positional overlap ratio (overlap / min_area) above which an overlapping duplicate
    // may replace the existing node without centroid alignment.
    static constexpr float kDuplicatePosOverlapThreshold = 0.70f;
    // Centroid-aligned mask IoU threshold above which a new blob is treated as a duplicate after
    // aligning centroids. Lower = more aggressive deduplication.
    static constexpr float kDuplicateCentroidIouThreshold = 0.80f;
    // Original-position bbox IoU threshold above which two shapes are treated as duplicates.
    static constexpr float kDuplicateBboxIouThreshold  = 0.70f;
    // Total-shape differentness threshold below which two shapes are treated as duplicates, as
    // long as they were not created in the same frame.
    static constexpr float kDuplicateMaxShapeDifference = 0.2f;
    // Sweep-only merge gate: current black-mask positional overlap over min-area must exceed this
    // before the expensive sliding-window IoU search runs.
    static constexpr float kSweepMergePosOverlapThreshold = 0.10f;
    // Sweep-only merge gate: best sliding-window IoU over thresholded black masks must exceed this
    // before two nodes are replaced by a fresh merged node. Lower-or-equal matches fall back to
    // the existing duplicate-delete behavior when positional overlap is high enough.
    static constexpr float kSweepMergeSlidingIouThreshold = 0.59f;
    // Wide-node sweep filter: if either node is at least this wide, use overlap over the smaller
    // node's black-pixel count instead of IoU for the sliding-window merge/delete decision.
    static const int       kSweepMergeWideNodeWidthThreshold = 300;
    // Maximum absolute translation (px per axis) scanned by the expensive sweep-only merge search.
    static const int       kSweepMergeMaxSlidePx        = 120;
    // Run a whole-graph duplicate sweep every N processed frames to collapse pre-existing duplicates
    // that were admitted earlier or drifted together after later updates.
    static const int       kGraphDedupeIntervalFrames   = 1;

    // --- Absence (natural erasure) ---
    // Score subtracted per frame when a node is in the visible area but not matched.
    // Higher = nodes erase faster when not seen.
    static constexpr float kAbsenceDecrement             = 0.5f;
    // Score added per frame when a node is successfully matched.
    // Higher = nodes recover faster after being re-seen.
    static constexpr float kAbsenceIncrement             = 0.5f;
    // A node is only absence-penalised if a matched node exists within this radius (canvas px).
    // This acts as a "visibility proxy": we only penalise what we can actually see.
    // Raise if nodes far from current strokes are decaying too fast.
    static constexpr float kAbsenceNearbyRadius          = 100.0f;
    // Minimum fraction of a projected node bbox that must overlap the observable region
    // (the inset frame) before the node is considered plausibly visible for absence decay.
    static constexpr float kAbsenceVisibleFractionMin    = 1.0f;
    // Inward margin shrunk from the cropped frame edges to define the "observable region."
    // When -1, uses max(1, frame_dim/50) (~2%), which exceeds the reject-mask edge strips (~1%).
    // Nodes outside this region are protected from absence decay.
    static constexpr int   kAbsenceFrameInsetPx          = -1;
    // Expansion added to the lecturer rect for absence-protection.
    // When -1, uses max(1, frame_dim/50) (~2%), strictly > reject-mask lecturer expansion (~1%).
    static constexpr int   kAbsenceLecturerExpansionPx   = -1;
    // If this fraction of a node bbox overlaps the lecturer region, protect it from decay.
    static constexpr float kAbsenceLecturerOverlapMin    = 0.01f;

    // --- Hard edges ---
    // Maximum centroid distance for creating a hard edge between nodes from the same frame.
    static constexpr float kHardEdgeMaxCentroidDist      = 100.0f;

    // --- Canvas defaults ---
    static const int kDefaultCanvasWidth  = 1920;
    static const int kDefaultCanvasHeight = 1080;

    // -----------------------------------------------------------------------
    // Sub-canvas collection (protected by state_mutex_)
    // -----------------------------------------------------------------------
    std::vector<std::unique_ptr<WhiteboardGroup>> groups_;
    int active_group_idx_ = -1;
    int view_group_idx_   = -1;

    int         frame_w_ = 0;
    int         frame_h_ = 0;

    // -----------------------------------------------------------------------
    // Worker thread and queue
    // -----------------------------------------------------------------------
    std::thread             worker_thread_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::optional<CanvasWorkItem> pending_item_;
    std::atomic<bool>       stop_worker_{false};
    std::unique_ptr<WhiteboardCanvasHelperClient> helper_client_;
    bool                    remote_process_ = false;
    bool                    duplicate_debug_mode_ = false;

    mutable std::mutex state_mutex_;

    // -----------------------------------------------------------------------
    // Motion gate
    // -----------------------------------------------------------------------
    cv::Mat prev_gray_;
    bool motion_gate_locked_ = false;

    // -----------------------------------------------------------------------
    // Atomic flags
    // -----------------------------------------------------------------------
    std::atomic<bool>     has_content_{false};
    std::atomic<bool>     canvas_view_mode_{false};
    std::atomic<int>      render_mode_{static_cast<int>(CanvasRenderMode::kRaw)};
    std::atomic<uint64_t> canvas_version_{0};

    void BumpCanvasVersion() {
        canvas_version_.fetch_add(1, std::memory_order_relaxed);
    }

    int processed_frame_id_ = 0;

    // -----------------------------------------------------------------------
    // Internal methods (run on worker_thread_)
    // -----------------------------------------------------------------------
    void WorkerLoop();
    bool EnsureRenderCacheReady(WhiteboardGroup& group, CanvasRenderMode render_mode);
    void ProcessFrameInternal(const cv::Mat& uncut_frame, const cv::Mat& person_mask);
    bool ApplyMotionGate(const cv::Mat& gray, float& motion_fraction, bool& motion_too_high);

    static cv::Mat BuildBinaryMask(const cv::Mat& gray, const cv::Mat& no_update_mask,
                                    int& stroke_pixel_count);
    std::vector<FrameBlob> ExtractFrameBlobs(const cv::Mat& binary,
                                              const cv::Mat& frame_bgr) const;
    cv::Point2f GlobalShapePass(WhiteboardGroup& group,
                                const std::vector<FrameBlob>& blobs);
    cv::Point2f MatchBlobsToGraph(WhiteboardGroup& group,
                                   std::vector<FrameBlob>& blobs);
    bool UpdateGraph(WhiteboardGroup& group, std::vector<FrameBlob>& blobs,
                     int current_frame, cv::Point2f frame_offset,
                     const cv::Rect& lecturer_canvas_rect = cv::Rect());

    static void RemoveNodeFromGraph(WhiteboardGroup& group, int node_id);
    void RebuildStrokeRenderCache(WhiteboardGroup& group);
    void RebuildRawRenderCache(WhiteboardGroup& group);
    void CreateSubCanvas(const cv::Mat& frame_bgr, const cv::Mat& binary,
                         std::vector<FrameBlob>& blobs, int current_frame);
    void SeedGroupFromFrameBlobs(WhiteboardGroup& group,
                                 const std::vector<FrameBlob>& blobs,
                                 int current_frame);
    void UpdateGroupBounds(WhiteboardGroup& group);
};

// ---------------------------------------------------------------------------
// Globals (defined in whiteboard_canvas.cpp)
// ---------------------------------------------------------------------------
extern WhiteboardCanvas*  g_whiteboard_canvas;
extern std::atomic<bool>  g_whiteboard_enabled;
extern std::atomic<float> g_canvas_pan_x;
extern std::atomic<float> g_canvas_pan_y;
extern std::atomic<float> g_canvas_zoom;
extern std::atomic<bool>  g_whiteboard_debug;
extern std::atomic<bool>  g_duplicate_debug_mode;
extern std::atomic<float> g_yolo_fps;
extern std::atomic<float> g_canvas_enhance_threshold;

// ---------------------------------------------------------------------------
// FFI exports
// ---------------------------------------------------------------------------
extern "C" {
    __declspec(dllexport) void    SetPanoramaEnabled(bool enabled);
    __declspec(dllexport) void    ResetPanorama();
    __declspec(dllexport) void    SetPanoramaViewport(float panX, float panY, float zoom);
    __declspec(dllexport) void    GetPanoramaCanvasSize(int* width, int* height);
    __declspec(dllexport) bool    IsPanoramaEnabled();

    __declspec(dllexport) void    SetCanvasViewMode(bool mode);
    __declspec(dllexport) bool    IsCanvasViewMode();
    __declspec(dllexport) void    SetCanvasRenderMode(int mode);
    __declspec(dllexport) int64_t GetCanvasTextureId();
    __declspec(dllexport) bool    GetCanvasOverviewRgba(uint8_t* buffer, int width, int height);
    __declspec(dllexport) bool    GetCanvasViewportRgba(uint8_t* buffer, int width, int height,
                                                         float panX, float panY, float zoom);

    __declspec(dllexport) int     GetSubCanvasCount();
    __declspec(dllexport) int     GetActiveSubCanvasIndex();
    __declspec(dllexport) void    SetActiveSubCanvas(int idx);
    __declspec(dllexport) int     GetSortedSubCanvasIndex(int pos);
    __declspec(dllexport) int     GetSortedPosition(int idx);

    __declspec(dllexport) void    SetWhiteboardDebug(bool enabled);
    __declspec(dllexport) void    SetDuplicateDebugMode(bool enabled);
    __declspec(dllexport) bool    GetDuplicateDebugMode();
    __declspec(dllexport) void    SetCanvasEnhanceThreshold(float threshold);

    // Graph node access
    __declspec(dllexport) int     GetGraphNodeCount();
    __declspec(dllexport) int     GetGraphNodes(float* buffer, int max_nodes);
    __declspec(dllexport) int     GetGraphHardEdges(int* buffer, int max_edges);
    __declspec(dllexport) int     GetGraphNodeNeighbors(int node_id, int* neighbors,
                                                         int max_neighbors);
    __declspec(dllexport) bool    CompareGraphNodes(int id_a, int id_b, float* result);
    __declspec(dllexport) bool    CompareGraphNodesAtOffset(int id_a, int id_b,
                                                             float dx, float dy, float* result);
    __declspec(dllexport) int     GetGraphNodeMasks(uint8_t* buffer, int max_bytes);
    __declspec(dllexport) bool    MoveGraphNode(int node_id, float new_cx, float new_cy);
    __declspec(dllexport) bool    DeleteGraphNode(int node_id);
    __declspec(dllexport) bool    ApplyUserEdits(const int* delete_ids, int delete_count,
                                                  const float* moves, int move_count);
    __declspec(dllexport) int     LockAllGraphNodes();
    __declspec(dllexport) bool    GetGraphCanvasBounds(int* bounds);
    __declspec(dllexport) int     GetGraphNodeContours(float* buffer, int max_floats);

    // Debug snapshots (stubs)
    __declspec(dllexport) bool    CaptureGraphDebugSnapshot(int slot);
    __declspec(dllexport) int     GetGraphSnapshotNodeCount(int slot);
    __declspec(dllexport) int     GetGraphSnapshotNodes(int slot, float* buffer, int max_nodes);
    __declspec(dllexport) bool    GetGraphSnapshotCanvasBounds(int slot, int* bounds);
    __declspec(dllexport) int     GetGraphSnapshotNodeContours(int slot, float* buffer,
                                                                int max_floats);
    __declspec(dllexport) bool    CompareGraphSnapshotNodes(int slot_a, int id_a,
                                                             int slot_b, int id_b,
                                                             float* result);
    __declspec(dllexport) bool    CombineGraphDebugSnapshots(int slot_a, int anchor_id_a,
                                                              int slot_b, int anchor_id_b);
    __declspec(dllexport) bool    CopyGraphDebugSnapshot(int source_slot, int target_slot);

    // Canvas version + full-res export
    __declspec(dllexport) uint64_t GetCanvasVersion();
    __declspec(dllexport) bool     GetCanvasFullResRgba(uint8_t* buffer, int max_w, int max_h,
                                                         int* out_w, int* out_h);
}
