#pragma once
// ============================================================================
// whiteboard_canvas.h -- Graph-Based Canvas Stitcher
//
// Captures whiteboard content from a moving camera using:
//   1. Contour extraction into first-class DrawingNode entities
//   2. Graph matching via long/short-edge similarity + edge-distance checks
//   3. Battle logic for overlap resolution (coexist/refresh/replace)
//   4. Duplicate-aware node merging for a stable stitched graph
//
// Threading model:
//   Camera thread  -> ProcessFrame()  (queues work, non-blocking)
//   Worker thread  -> pipeline stages (ProcessFrameInternal)
//   UI thread      -> GetViewport()   (try_lock, never stalls camera)
// ============================================================================

#include <opencv2/opencv.hpp>
#include <opencv2/flann.hpp>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <future>

class WhiteboardCanvasHelperClient;

enum class CanvasRenderMode : int {
    kStroke = 0,
    kRaw = 1,
};

// ---------------------------------------------------------------------------
// CandidateNode -- Tentative drawing; must be confirmed k frames before promotion
// ---------------------------------------------------------------------------
struct CandidateNode {
    int id = -1;                        // Unique candidate ID
    cv::Mat binary_mask;                // CV_8UC1, tight to bbox
    cv::Mat color_pixels;               // CV_8UC3, tight to bbox (for raw mode)
    cv::Rect bbox_canvas;               // Bounding box in canvas coordinates
    cv::Point2f centroid_canvas;        // Centroid in canvas coordinates
    std::vector<cv::Point> contour;     // Contour points (relative to bbox origin)
    double hu[7] = {};                  // Hu Moments for matching
    double area = 0.0;                  // Pixel area
    int seen_count = 1;                 // Frames this candidate was confirmed
    int absence_count = 0;              // Consecutive processed frames where the candidate was not re-seen
    int last_seen_frame = 0;            // Last frame where this candidate was seen
    int created_frame = 0;              // Frame this candidate was first seen
    std::vector<int> anchor_node_ids;   // Neighbor graph nodes used for placement
};

// ---------------------------------------------------------------------------
// DrawingNode -- A single drawing entity on the canvas (replaces Chunk pixels)
// ---------------------------------------------------------------------------
struct DrawingNode {
    int id = -1;                        // Unique ID within group
    cv::Mat binary_mask;                // CV_8UC1, tight to bbox
    cv::Mat color_pixels;               // CV_8UC3, tight to bbox (for raw mode)
    cv::Rect bbox_canvas;               // Bounding box in canvas coordinates
    cv::Point2f centroid_canvas;        // Centroid in canvas coordinates
    std::vector<cv::Point> contour;     // Contour points (relative to bbox origin)
    double hu[7] = {};                  // Hu Moments for matching
    double area = 0.0;                  // Pixel area
    double max_area_seen = 0.0;         // Largest confirmed area across refreshes
    int absence_count = 0;              // Frames visible-but-not-seen
    int last_seen_frame = 0;
    int created_frame = 0;
    int seen_count = 0;                 // Number of frames this node was confirmed
    int in_view_count = 0;              // Frames this node was observable (in cropped view, not occluded)
    int match_distance = -1;            // BFS hop distance from a directly-matched blob (0=matched, 1=neighbor, etc.; -1=not seen this frame)
    std::vector<int> neighbor_ids;      // K nearest neighbor edges
};

// ---------------------------------------------------------------------------
// FrameBlob -- Transient, per-frame extraction result
// ---------------------------------------------------------------------------
struct FrameBlob {
    cv::Rect bbox;                      // In frame coordinates
    cv::Point2f centroid;               // In frame coordinates
    cv::Mat binary_mask;                // Tight mask (CV_8UC1)
    cv::Mat color_pixels;               // BGR pixels (CV_8UC3)
    std::vector<cv::Point> contour;     // Contour in frame coordinates
    std::vector<int> neighbor_blob_indices; // K-nearest frame-graph neighbors
    double hu[7] = {};
    double area = 0.0;
    int matched_node_id = -1;           // Set during matching
    double matched_shape_dist = 1e9;    // Shape distance for the best matched canvas node
    cv::Point2f matched_offset{0, 0};   // Estimated canvas translation from the best match
    double matched_long_ratio = 0.0;    // Long-edge similarity for the best graph match
    double matched_short_ratio = 0.0;   // Short-edge similarity for the best graph match
    double matched_edge_error = 1e9;    // Graph edge-distance error to supporting matches
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
        uint64_t key = CellKey(cx, cy);
        cells_[key].push_back(id);
        positions_[id] = centroid;
    }

    void Remove(int id, cv::Point2f centroid) {
        int cx = (int)std::floor(centroid.x / cell_size_);
        int cy = (int)std::floor(centroid.y / cell_size_);
        uint64_t key = CellKey(cx, cy);
        auto it = cells_.find(key);
        if (it != cells_.end()) {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
            if (vec.empty()) cells_.erase(it);
        }
        positions_.erase(id);
    }

    void Clear() {
        cells_.clear();
        positions_.clear();
    }

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
                    cv::Point2f diff = pit->second - point;
                    if (diff.x * diff.x + diff.y * diff.y <= r2) {
                        result.push_back(id);
                    }
                }
            }
        }
        return result;
    }

    std::vector<int> QueryKNearest(cv::Point2f point, int k) const {
        // Search expanding rings until we have enough candidates
        std::vector<std::pair<float, int>> candidates;
        for (const auto& pair : positions_) {
            cv::Point2f diff = pair.second - point;
            float d2 = diff.x * diff.x + diff.y * diff.y;
            candidates.push_back({d2, pair.first});
        }
        std::sort(candidates.begin(), candidates.end());
        std::vector<int> result;
        for (int i = 0; i < std::min(k, (int)candidates.size()); i++) {
            result.push_back(candidates[i].second);
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
// WhiteboardGroup -- A single continuous lecture session (until wiped)
// ---------------------------------------------------------------------------
struct WhiteboardGroup {
    int debug_id = -1;

    // Graph of drawing nodes
    std::unordered_map<int, std::unique_ptr<DrawingNode>> nodes;
    int next_node_id = 0;
    SpatialIndex spatial_index{200};

    // Candidate pool — blobs awaiting confirmation across multiple frames
    std::unordered_map<int, std::unique_ptr<CandidateNode>> candidates;
    int next_candidate_id = 0;

    // Stroke view bounds (in pixels)
    int stroke_min_px_x = 0;
    int stroke_min_px_y = 0;
    int stroke_max_px_x = 512;
    int stroke_max_px_y = 512;

    // Raw view bounds (in pixels)
    int raw_min_px_x = 0;
    int raw_min_px_y = 0;
    int raw_max_px_x = 512;
    int raw_max_px_y = 512;

    // Fixed render height (locked to first frame height, only grows)
    int fixed_render_height = 0;

    // Last detected lecturer area in canvas coordinates (for overlay)
    cv::Rect last_lecturer_rect;

    // Rendered stitched outputs for UI viewport
    cv::Mat stroke_render_cache;
    bool stroke_cache_dirty = true;
    cv::Mat raw_render_cache;
    bool raw_cache_dirty = true;
};

// ---------------------------------------------------------------------------
// Work item queued from the camera thread to the canvas worker thread
// ---------------------------------------------------------------------------
struct CanvasWorkItem {
    cv::Mat frame;        // BGR
    cv::Mat person_mask;  // Incoming person mask
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
    void SyncRuntimeSettings();
    void InvalidateRenderCaches();
    void RecordRgbaCopyProfile(double duration_ms);

    // --- Sub-canvas navigation ---
    int  GetSubCanvasCount() const;
    int  GetActiveSubCanvasIndex() const;
    void SetActiveSubCanvas(int idx);
    int  GetSortedSubCanvasIndex(int pos) const;
    int  GetSortedPosition(int idx) const;

    // --- Graph debug ---
    int  GetGraphNodeCount() const;
    int  GetGraphNodes(float* buffer, int max_nodes) const;
    int  GetGraphNodeNeighbors(int node_id, int* neighbors, int max_neighbors) const;
    bool CompareGraphNodes(int id_a, int id_b, float* result) const;
    bool MoveGraphNode(int node_id, float new_cx, float new_cy);
    bool GetGraphCanvasBounds(int* bounds) const;
    int  GetGraphNodeContours(float* buffer, int max_floats) const;
    bool CaptureGraphDebugSnapshot(int slot,
                                   const cv::Mat& frame,
                                   const cv::Mat& person_mask);
    int  GetGraphSnapshotNodeCount(int slot) const;
    int  GetGraphSnapshotNodes(int slot, float* buffer, int max_nodes) const;
    bool GetGraphSnapshotCanvasBounds(int slot, int* bounds) const;
    int  GetGraphSnapshotNodeContours(int slot, float* buffer, int max_floats) const;
    bool CompareGraphSnapshotNodes(int slot_a,
                                   int id_a,
                                   int slot_b,
                                   int id_b,
                                   float* result) const;
    bool CombineGraphDebugSnapshots(int slot_a,
                                    int anchor_id_a,
                                    int slot_b,
                                    int anchor_id_b);
    bool CopyGraphDebugSnapshot(int source_slot, int target_slot);

private:
    // -----------------------------------------------------------------------
    // Tuning constants
    // -----------------------------------------------------------------------

    static constexpr bool kEnableMotionGate = true;
    static constexpr bool kEnableHighMotionGate = true; // Skip frame updates when motion exceeds kMaxMotionFraction.
    static const int       kMotionForceInterval = 1;

    // Alignment improvements
    // Merge duplicate threshold: max centroid distance to consider nodes as potentially same
    static constexpr bool  kEnableJumpRejection      = false; // Reject matches with implausibly large jumps (pre-RANSAC)
    static constexpr bool  kEnableNeighborBinMerge    = false; // Reject matches with implausibly large jumps (pre-RANSAC)
    static constexpr bool  kEnablePhaseCorrelation    = false; // Reject matches with implausibly large jumps (pre-RANSAC)

    static constexpr float kMaxJumpPx              = 200.0f;
    static const int       kVoteBinSize            = 5;

    // Sub-canvas creation
    static const int       kMinStrokePixelsForNewSC = 500;
    static const int       kMotionLongEdge = 256;
    static constexpr float kMinMotionFraction = 0.01f;
    static constexpr float kMaxMotionFraction = 0.05f;
    static const int       kStillFramePatience = 1;

    // Worker queue
    static const int       kQueueDepth         = 1;

    // Raw canvas quality
    // (blur rejection removed — not used in current pipeline)

    // Contour matching
    static constexpr double kMaxShapeDist    = 0.5;
    static const int        kMinContourArea  = 30;
    static const int        kMinShapeVotes   = 3;
    static const int        kStableGraphNodeThreshold = 5; // Number of confirmations before a node is considered stable enough to seed new sub-canvas creation.
    static constexpr float  kMinNodeLongEdgeSimilarity = 0.80f;
    static constexpr float  kMinNodeShortEdgeSimilarity = 0.80f;
    static constexpr float  kGraphEdgeDistanceTolerancePx = 1.0f;
    static const int        kGraphSeedCandidateLimit = 24;

    // Graph matching
    static constexpr float  kStrokeClusterRadius = 75.0f; // Max centroid distance to cluster strokes together.
    static constexpr float  kSquareSelectionRadiusThreshold = 15.0f; // Min radius to prefer squarest stroke
    static const int        kMatchSearchRadius = 120; // Maximum centroid distance (in pixels) for a blob to be considered a potential match to a graph node.
    static const int        kKNeighbors = 5; // Number of nearest graph nodes to consider when matching a blob, and number of neighbors to store for each node.
    static constexpr double kFrameGraphAnchorShapeDist = 0.20;       // Maximum shape distance for a blob to be trusted as a frame-graph anchor.
    static const int        kFrameGraphAnchorNeighbors = 4;          // Number of nearby matched blobs used to estimate a new blob's canvas position.

    // Duplicate-avoidance toggles
    static constexpr bool   kEnableMergeNearIdenticalBbox = false;           // Merge existing nodes when their bbox size and IoU are almost identical.
    static constexpr bool   kEnableMergeOverlapShapeContainment = false;     // Merge existing nodes when overlap, shape distance, and containment all indicate a duplicate.
    static constexpr bool   kEnableMergeShiftedDuplicate = false;            // Merge translated duplicates using centroid-aligned mask overlap.
    static constexpr bool   kEnableCanonicalDuplicateRetention = false;      // Keep the strongest canonical node during merges instead of simply keeping the newest one.
    static constexpr bool   kEnableMergeCenterAlignedDuplicate = true;     // Merge nodes when center-aligned bitwise AND exceeds threshold (catches shifted duplicates).
    static constexpr bool   kEnableMergeSideAlignedDuplicate = true;       // Merge nodes when side-attached alignment bitwise AND exceeds threshold.
    static constexpr bool   kEnableFrameStrokeRejectFilter = true;          // Reject whole frame blobs that touch the side margins or padded lecturer area before graph admission.
    static constexpr float kToFarToBeSame = 40.0f;

    // Battle thresholds
    static constexpr float kBattleCoexistOverlap = 0.15f; // Minimum IoU for a new blob to coexist with an existing node (no refresh or replacement).
    static constexpr float kBattleRefreshOverlap = 0.6f; // Minimum IoU for an existing node to be refreshed by a new blob with better shape similarity.
    static constexpr float kBattleReplaceOverlap = 0.5f; // Minimum IoU for an existing node to be replaced by a new blob with much better shape similarity.

    // KD-Tree + RANSAC matching
    static constexpr double kKdTreeHuDistanceThreshold = 3.0; // Maximum Hu Moments distance for a blob-node pair to be considered a potential match (pre-RANSAC).
    static constexpr float  kKdTreeMinBboxSimilarity   = 0.70f; // Minimum bounding box similarity for a blob-node pair to be considered a potential match (pre-RANSAC).
    static constexpr float  kRansacInlierTolerancePx   = 5.0f; // Maximum allowed pixel error for a blob-node pair to be considered an inlier in RANSAC.
    static constexpr int    kRansacMaxIterations        = 300; // Maximum RANSAC iterations per blob-node pair
    static constexpr int    kMinRansacInliers           = 3; // Minimum inliers required for a blob-node match to be accepted
    static constexpr int    kKdTreeKnnNeighbors         = 5; // Number of nearest neighbors to retrieve from KD-Tree for each blob during matching

    // Frame-to-canvas merge depth
    static constexpr int    kMaxNewBlobHopDepth         = 3; // Max BFS hops from a matched blob to admit unmatched blobs as new nodes

    // Camera tracking — vote confidence
    static const int       kMinVotesForCameraUpdate = 3;  // Minimum matched pairs to trust position

    // Camera tracking — velocity smoothing
    static constexpr float kVelocitySmoothingAlpha  = 0.3f;  // EMA alpha for velocity
    static constexpr float kMaxPredictedJumpPx      = 400.0f; // Absolute cap on velocity-predicted jump

    // Candidate confirmation (new-stroke patience)
    static constexpr bool   kEnableCandidateStaging = false;      // Temporary bypass: add anchored new blobs directly to the graph.
    static const int       kCandidateConfirmFrames = 5;   // Frames a blob must appear before becoming a node
    static const int       kCandidateExpireFrames  = 3;   // Consecutive processed frames without confirmation before a candidate is discarded
    static constexpr float kCandidateMatchRadiusPx = 30.0f; // Max centroid distance to match blob to candidate
    static constexpr double kCandidateMatchShapeDist = 0.35; // Max shape distance to match blob to candidate

    static const int       kGraphDebugCompareSnapshotCount = 2;
    static const int       kGraphDebugResultSnapshotSlot = 2;
    static const int       kGraphDebugSnapshotCount = 3;

    // Canvas defaults
    static const int kDefaultCanvasWidth = 1920;
    static const int kDefaultCanvasHeight = 1080;

    // -----------------------------------------------------------------------
    // Sub-canvas collection (protected by state_mutex_)
    // -----------------------------------------------------------------------
    std::vector<std::unique_ptr<WhiteboardGroup>> groups_;
    int active_group_idx_ = -1;
    int view_group_idx_   = -1;

    cv::Point2f global_camera_pos_{0, 0};
    cv::Point2f camera_velocity_{0, 0};          // Smoothed velocity (px/frame)
    int         last_vote_count_ = 0;             // Votes from last successful match
    bool global_frame_bootstrap_consumed_ = false;
    int frame_w_ = 0;
    int frame_h_ = 0;

    // -----------------------------------------------------------------------
    // Worker thread and queue
    // -----------------------------------------------------------------------
    std::thread              worker_thread_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::queue<CanvasWorkItem> work_queue_;
    std::atomic<bool>        stop_worker_{false};
    std::unique_ptr<WhiteboardCanvasHelperClient> helper_client_;
    bool                     remote_process_ = false;

    // -----------------------------------------------------------------------
    // State mutex
    // -----------------------------------------------------------------------
    mutable std::mutex state_mutex_;
    mutable std::mutex perf_stats_mutex_;
    mutable std::mutex graph_snapshot_mutex_;

    // -----------------------------------------------------------------------
    // Motion gate
    // -----------------------------------------------------------------------
    cv::Mat prev_gray_;
    int     frames_since_warp_ = 0;
    int     matched_frame_counter_ = 0;
    int     motion_frame_counter_ = 0;

    // -----------------------------------------------------------------------
    // Atomic flags
    // -----------------------------------------------------------------------
    std::atomic<bool> has_content_{false};
    std::atomic<bool> canvas_view_mode_{false};
    std::atomic<int>  render_mode_{static_cast<int>(CanvasRenderMode::kRaw)};
    std::array<std::unique_ptr<WhiteboardGroup>, kGraphDebugSnapshotCount>
        graph_debug_snapshots_;
    int graph_debug_snapshot_frame_id_ = 0;

    // -----------------------------------------------------------------------
    // Debug
    // -----------------------------------------------------------------------
    struct ScDebugInfo {
        std::string window_name;
        int         debug_id  = -1;
        bool        closed    = false;
    };
    std::vector<ScDebugInfo> sc_debug_infos_;
    int next_debug_id_ = 0;

    struct PipelineDebugState {
        cv::Mat frame;
        cv::Mat gray_clean;
        cv::Mat enhanced_bgr;
        cv::Mat binary;
        cv::Mat no_update_mask;
        float   motion_fraction    = 0.0f;
        int     stroke_pixel_count = 0;
        cv::Point2f shift;
        std::string match_status;
        bool    created_new_sc     = false;
    };

    enum class ProfileStep : size_t {
        kQueueSubmit = 0,
        kFrameTotal,
        kNoUpdateMask,
        kMotionGate,
        kBinaryMask,
        kBlobExtract,
        kGraphMatch,
        kCameraUpdate,
        kGraphUpdate,
        kMatchedRefresh,
        kCandidateProcess,
        kMergeNodes,
        kAbsenceTracking,
        kNeighborRebuild,
        kBoundsUpdate,
        kSeedGroup,
        kCreateSubCanvas,
        kEnsureRenderCache,
        kRenderStrokeCache,
        kRenderRawCache,
        kViewport,
        kOverview,
        kRgbaCopy,
        kCount
    };

    struct ProfileAccumulator {
        uint64_t count = 0;
        double total_ms = 0.0;
        double total_sq_ms = 0.0;

        void Add(double duration_ms) {
            count++;
            total_ms += duration_ms;
            total_sq_ms += duration_ms * duration_ms;
        }
    };

    struct PerformanceStats {
        std::array<ProfileAccumulator, static_cast<size_t>(ProfileStep::kCount)> steps{};
        double total_frame_contours = 0.0;
        double total_canvas_contours = 0.0;
        double total_best_votes = 0.0;
        uint64_t total_candidate_pairs = 0;
        uint64_t total_accepted_pairs = 0;
        int frame_count = 0;
        int gated_frames = 0;
        int matched_frames = 0;
        double last_fps = 0.0;
        int last_votes = 0;
        std::chrono::time_point<std::chrono::steady_clock> window_start_time;
    } perf_stats_;

    // -----------------------------------------------------------------------
    // Internal methods (all run on worker_thread_ unless noted)
    // -----------------------------------------------------------------------
    void WorkerLoop();
    bool EnsureRenderCacheReady(WhiteboardGroup& group,
                                CanvasRenderMode render_mode);
    void ProcessFrameInternal(const cv::Mat& uncut_frame, const cv::Mat& person_mask);
    void RecordProfileSample(ProfileStep step, double duration_ms);
    void RecordFrameProfile(bool motion_gated,
                            int best_votes,
                            int blob_count,
                            int graph_node_count);
    void MaybeLogProfileSummary(bool force = false);
    static const char* ProfileStepName(ProfileStep step);
    bool ApplyMotionGate(const cv::Mat& gray,
                         float& motion_fraction,
                         bool& motion_too_low,
                         bool& motion_too_high);
    void RenderMotionGateDebug(const cv::Mat& frame,
                               const cv::Mat& gray,
                               const cv::Mat& no_update_mask,
                               float motion_fraction,
                               bool motion_too_low);
    void RenderFrameDebug(const cv::Mat& frame,
                          const cv::Mat& gray,
                          const cv::Mat& binary,
                          const cv::Mat& no_update_mask,
                          float motion_fraction,
                          int stroke_pixel_count,
                          const std::string& match_status,
                          bool created_new_sc);

    // Graph-based blob extraction
    std::vector<FrameBlob> ExtractFrameBlobs(const cv::Mat& binary,
                                              const cv::Mat& frame_bgr) const;

    // Graph-based matching: returns camera offset and vote count
    cv::Point2f MatchBlobsToGraph(WhiteboardGroup& group,
                                   std::vector<FrameBlob>& blobs,
                                   int& out_best_votes);
    cv::Point2f MatchBlobsToGraphWithSeed(const WhiteboardGroup& group,
                                          std::vector<FrameBlob>& blobs,
                                          int& out_best_votes,
                                          const cv::Point2f& prior_offset);

    // Graph update: classify blobs, battle, and add nodes
    bool UpdateGraph(WhiteboardGroup& group,
                     std::vector<FrameBlob>& blobs,
                     int current_frame);

    // Sub-steps of UpdateGraph (extracted for readability)
    void ProcessCandidateBlobs(WhiteboardGroup& group,
                               std::vector<FrameBlob>& blobs,
                               const std::unordered_set<int>& reachable_new_blob_indices,
                               std::unordered_map<int, int>& blob_to_graph_node_id,
                               const cv::Rect& cropped_frame,
                               int current_frame,
                               bool& graph_changed);
    void MergeOverlappingNodes(WhiteboardGroup& group,
                               int current_frame,
                               bool& graph_changed);
    static void RemoveNodeFromGraph(WhiteboardGroup& group, int node_id);

    // Phase correlation using nearby node masks
    cv::Mat GetCanvasGrayRegion(WhiteboardGroup& group, int global_x, int global_y,
                                int width, int height);

    // Rendering from graph nodes
    void RebuildStrokeRenderCache(WhiteboardGroup& group);
    void RebuildRawRenderCache(WhiteboardGroup& group);

    // Create a new group seeded with first frame's blobs
    void CreateSubCanvas(const cv::Mat& frame_bgr, const cv::Mat& binary,
                         std::vector<FrameBlob>& blobs,
                         int current_frame);
    void SeedGroupFromFrameBlobs(WhiteboardGroup& group,
                                 const std::vector<FrameBlob>& blobs,
                                 int current_frame);

    // Update bounds from all nodes
    void UpdateGroupBounds(WhiteboardGroup& group);

    // Debug: 3x2 tile grid showing pipeline stages
    void RenderDebugGrid(const PipelineDebugState& state);

    int processed_frame_id_ = 0;
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
extern std::atomic<bool>  g_canvas_show_graph;

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
    __declspec(dllexport) bool    GetCanvasViewportRgba(uint8_t* buffer, int width, int height, float panX, float panY, float zoom);

    __declspec(dllexport) int     GetSubCanvasCount();
    __declspec(dllexport) int     GetActiveSubCanvasIndex();
    __declspec(dllexport) void    SetActiveSubCanvas(int idx);
    __declspec(dllexport) int     GetSortedSubCanvasIndex(int pos);
    __declspec(dllexport) int     GetSortedPosition(int idx);

    __declspec(dllexport) void    SetWhiteboardDebug(bool enabled);
    __declspec(dllexport) void    SetCanvasEnhanceThreshold(float threshold);
    __declspec(dllexport) void    SetCanvasShowGraph(bool enabled);
    __declspec(dllexport) bool    IsCanvasShowGraph();

    // Graph debug FFI
    __declspec(dllexport) int     GetGraphNodeCount();
    __declspec(dllexport) int     GetGraphNodes(float* buffer, int max_nodes);
    __declspec(dllexport) int     GetGraphNodeNeighbors(int node_id, int* neighbors, int max_neighbors);
    __declspec(dllexport) bool    CompareGraphNodes(int id_a, int id_b, float* result);
    __declspec(dllexport) bool    MoveGraphNode(int node_id, float new_cx, float new_cy);
    __declspec(dllexport) bool    GetGraphCanvasBounds(int* bounds);
    __declspec(dllexport) int     GetGraphNodeContours(float* buffer, int max_floats);
    __declspec(dllexport) bool    CaptureGraphDebugSnapshot(int slot);
    __declspec(dllexport) int     GetGraphSnapshotNodeCount(int slot);
    __declspec(dllexport) int     GetGraphSnapshotNodes(int slot, float* buffer, int max_nodes);
    __declspec(dllexport) bool    GetGraphSnapshotCanvasBounds(int slot, int* bounds);
    __declspec(dllexport) int     GetGraphSnapshotNodeContours(int slot, float* buffer, int max_floats);
    __declspec(dllexport) bool    CompareGraphSnapshotNodes(int slot_a, int id_a, int slot_b, int id_b, float* result);
    __declspec(dllexport) bool    CombineGraphDebugSnapshots(int slot_a, int anchor_id_a, int slot_b, int anchor_id_b);
    __declspec(dllexport) bool    CopyGraphDebugSnapshot(int source_slot, int target_slot);
}
