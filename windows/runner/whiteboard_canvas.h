#pragma once
// ============================================================================
// whiteboard_canvas.h -- Graph-Based Canvas Stitcher (simplified)
//
// Captures whiteboard content from a moving camera using:
//   1. Connected-component blob extraction per frame
//   2. Spatial + Hu-distance matching to find camera offset
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

// ---------------------------------------------------------------------------
// DrawingNode -- A single drawing entity on the canvas
// ---------------------------------------------------------------------------
static constexpr float kAbsenceScoreInitial = 3.5f;
static constexpr float kAbsenceScoreMax     = 10.0f;

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
    int    last_seen_frame = 0;
    int    created_frame   = 0;
    bool   user_locked     = false;
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
    static const int       kBinarizeOffset               = 5;
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
    // Connected components within this pixel radius are merged into a single blob.
    // Raise to join fragmented strokes into one node. Lower to keep individual marks separate.
    static constexpr float kStrokeClusterRadius          = 70.0f;
    // When true, rejects blobs whose pixels overlap:
    //   (a) Left/right frame border strips of width max(ceil(kStrokeClusterRadius), frame_w/100).
    //       Note: top and bottom borders are NOT masked.
    //   (b) The lecturer bounding box expanded by max(ceil(kStrokeClusterRadius), frame_dim/100).
    // Disable to capture content that touches the side edges or the lecturer region.
    static constexpr bool  kEnableFrameStrokeRejectFilter = true;

    // --- Matching ---
    // Maximum Hu-moment L2 distance (in log10 space) to accept a blob↔node match.
    // Lower = stricter shape matching, fewer but more reliable matches.
    // Higher = more matches but more false positives between similar shapes.
    static constexpr double kHuDistanceThreshold         = 0.7;
    // A matched blob must be within this many pixels of the median consensus offset
    // to be counted as an inlier. Lower = tighter consensus required.
    static constexpr float kRansacTolerancePx            = 5.0f;
    // Blob area must fall in [kAreaRatioMin × node_area,  node_area / kAreaRatioMin].
    // e.g. 0.4 → blob can be at most 2.5× larger or 2.5× smaller than the node.
    //      0.5 → tighter: at most 2×.   0.1 → nearly useless: up to 10×.
    // Must be in (0, 1). Lower = stricter size gate.
    static constexpr float kAreaRatioMin                 = 0.1f;
    // Lowe's ratio test. A match is rejected when BOTH:
    //   (a) a second candidate also falls within kHuDistanceThreshold, AND
    //   (b) best_hu_dist / second_best_hu_dist > kHuUniquenessRatio  (best is not clearly better).
    // If only one candidate exists within threshold, the test is skipped (match always accepted).
    // 1.0 = test is off (always accept). 0.5 = very strict (best must be <50% of second-best dist).
    static constexpr float kHuUniquenessRatio            = 1.00f;
    // Minimum number of inlier blob↔node matches required before new strokes are added to the graph.
    // This gates how trustworthy frame_offset must be before committing new nodes.
    // Raise if lecturer motion causes spurious new strokes. Lower if new strokes are missed.
    static const int       kMinMatchesForNewNode         = 2;

    // --- Merge (duplicate insertion check) ---
    // Radius (px, canvas coords) to search for duplicate candidates around each new node's centroid.
    // Raise if near-duplicate nodes from nearby positions are not being caught.
    static constexpr float kMergeSearchRadiusPx          = 60.0f;
    // Overlap ratio (overlap / min_area) above which a new blob is treated as a duplicate of an
    // existing node and suppressed (or used to refresh it). Lower = more aggressive deduplication.
    static constexpr float kDuplicateOverlapThreshold    = 0.90f;

    // --- Node merge (post-pass dedup between existing nodes) ---
    // Scoring method for sliding-window alignment before merging two nodes.
    static constexpr AlignmentScoreMode kAlignmentMode   = AlignmentScoreMode::kIoU;
    // Sliding window search radius (px). Best offset searched in [-r, +r] x [-r, +r].
    static constexpr int   kAlignSearchRadius            = 10;
    // Mask containment (overlap / smaller_node_px) above which the smaller node is
    // removed as a fragment of the larger. Only between different-frame nodes.
    static constexpr float kContainmentRemoveThreshold   = 0.80f;
    // Mask overlap (overlap / min_area) above which two different-frame nodes are
    // combined into a single merged node via sliding-window alignment.
    static constexpr float kOverlapMergeThreshold        = 0.50f;

    // --- Battle (overlap resolution between existing nodes) ---
    // Overlap fraction above which a matched blob refreshes (overwrites) its node.
    // Higher = harder to refresh a node; only large overlaps trigger an update.
    // Lower = nodes update more eagerly on partial overlap.
    static constexpr float kBattleRefreshOverlap         = 0.70f;
    // Centroid shift (canvas px) above which a refreshing blob always replaces the node even
    // if the incoming blob is not larger. Allows node to follow a moving stroke.
    static constexpr float kBattleRefreshShiftPx         = 3.0f;

    // --- Absence (natural erasure) ---
    // Score subtracted per frame when a node is in the visible area but not matched.
    // Higher = nodes erase faster when not seen.
    static constexpr float kAbsenceDecrement             = 0.3f;
    // Score added per frame when a node is successfully matched.
    // Higher = nodes recover faster after being re-seen.
    static constexpr float kAbsenceIncrement             = 0.7f;
    // A node is only absence-penalised if a matched node exists within this radius (canvas px).
    // This acts as a "visibility proxy": we only penalise what we can actually see.
    // Raise if nodes far from current strokes are decaying too fast.
    static constexpr float kAbsenceNearbyRadius          = 100.0f;

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

    mutable std::mutex state_mutex_;

    // -----------------------------------------------------------------------
    // Motion gate
    // -----------------------------------------------------------------------
    cv::Mat prev_gray_;

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
    __declspec(dllexport) void    SetCanvasEnhanceThreshold(float threshold);

    // Graph node access
    __declspec(dllexport) int     GetGraphNodeCount();
    __declspec(dllexport) int     GetGraphNodes(float* buffer, int max_nodes);
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
