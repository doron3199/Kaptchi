#pragma once
// ============================================================================
// whiteboard_canvas.h -- YOLO + Contour Matching Canvas Stitcher
//
// Captures whiteboard content from a moving camera using:
//   1. ML object detection (YOLO) to isolate drawn text/symbol regions
//   2. Hu Moment contour matching (cv::matchShapes) for drift-free placement
//   3. Two-pass pixel stitching that paints only genuinely new strokes
//   4. Dynamic canvas growth as the camera pans
//
// Threading model:
//   Camera thread  -> ProcessFrame()  (queues work, non-blocking)
//   Worker thread  -> pipeline stages (ProcessFrameInternal)
//   UI thread      -> GetViewport()   (try_lock, never stalls camera)
// ============================================================================

#include <opencv2/opencv.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <future>

class WhiteboardCanvasHelperClient;

enum class CanvasRenderMode : int {
    kStroke = 0,
    kRaw = 1,
};

// ---------------------------------------------------------------------------
// Chunk -- one fixed-size tile of the virtual infinite whiteboard
// ---------------------------------------------------------------------------
struct Chunk {
    cv::Mat stroke_canvas;   // CV_8UC3, white-initialized, kChunkWidth x chunk_height_
    cv::Mat absence_counter; // CV_8UC1, zero-initialized, tracking erase frames
    cv::Mat raw_canvas;      // CV_8UC3, white-initialized raw mosaic tile
    int grid_x;
    int grid_y;
};

// ---------------------------------------------------------------------------
// WhiteboardGroup -- A single continuous lecture session (until wiped)
// ---------------------------------------------------------------------------
struct WhiteboardGroup {
    int debug_id = -1;

    // Spatial hash of kChunkWidth x chunk_height_ chunks
    std::unordered_map<uint64_t, std::unique_ptr<Chunk>> chunks;

    // Stroke view bounds (in pixels)
    int stroke_min_px_x = 0;
    int stroke_min_px_y = 0;
    int stroke_max_px_x = 0;
    int stroke_max_px_y = 0;

    // Raw view bounds (in pixels)
    int raw_min_px_x = 0;
    int raw_min_px_y = 0;
    int raw_max_px_x = 0;
    int raw_max_px_y = 0;

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
    cv::Mat person_mask;  // YOLO Person Mask
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

    // --- Sub-canvas navigation ---
    int  GetSubCanvasCount() const;
    int  GetActiveSubCanvasIndex() const;
    void SetActiveSubCanvas(int idx);
    int  GetSortedSubCanvasIndex(int pos) const;
    int  GetSortedPosition(int idx) const;

private:
    // -----------------------------------------------------------------------
    // Tuning constants
    // -----------------------------------------------------------------------

    static constexpr bool kEnableMotionGate = true;

    // Alignment improvements (each can be toggled independently)
    static constexpr bool  kEnableJumpRejection      = false;  // Reject matches that jump too far from previous position.
    static constexpr bool  kEnableNeighborBinMerge     = false;  // Merge winning vote bin with 8 neighbors for robustness.

    // Sub-pixel refinement methods (at most one should be enabled at a time)
    static constexpr bool  kEnableECCRefinement        = false;  // ECC optimization: spatial-domain iterative search. Handles intensity changes well. Slow (~5-15ms).
    static constexpr bool  kEnableTemplateRefinement    = true;  // Template matching: brute-force correlation of ink-rich patches. Sub-pixel via parabola fit.
    static constexpr bool  kEnableLKRefinement          = false;   // Lucas-Kanade optical flow: tracks contour centroids between frames. Very fast (<1ms).

    // ECC parameters
    static const int       kECCMaxIterations           = 50;     // Max iterations for ECC optimization. Rec: 30-100.
    static constexpr double kECCTerminationEps         = 1e-4;   // Convergence threshold for ECC. Lower = more precise but slower. Rec: 1e-5 to 1e-3.
    static constexpr float kECCMaxCorrection           = 10.0f;  // Max pixel correction from ECC. Reject larger shifts.

    // Template matching parameters
    static const int       kTemplateMatchPatchSize     = 64;     // Size of the ink-rich patch to match (px at reference res). Rec: 32-128.
    static const int       kTemplateMatchSearchRadius  = 15;     // Search radius around expected position (px at reference res). Rec: 5-20.
    static constexpr float kTemplateMaxCorrection      = 10.0f;  // Max pixel correction from template matching.

    // Lucas-Kanade parameters
    static const int       kLKMinTrackedPoints         = 3;      // Min contour centroids to track. Rec: 3-10.
    static const int       kLKMaxTrackedPoints         = 20;     // Max contour centroids to track. Rec: 10-30.
    static constexpr float kLKMaxCorrection            = 10.0f;  // Max pixel correction from LK flow.

    static constexpr float kMaxJumpPx              = 40.0f;  // Max allowed position jump (px at reference res). Rec: 30-60.

    // Sub-canvas creation
    static const int       kMinStrokePixelsForNewSC = 500;   // Min whiteboard pixels to spawn a new canvas (at reference res). Rec: 300-1000.
    static const int       kMotionLongEdge = 256;            // Downscale size for motion detection. Low: faster but blind to fine motion, High: slow. Rec: 128-512.
    static constexpr float kMinMotionFraction = 0.001f;       // Min changed pixels to process frame. Low: processes noise, High: ignores slow movement. Rec: 0.005-0.03.
    static constexpr float kMaxMotionFraction = 0.08f;       // Max changed pixels to process frame. Low: ignores fast pans, High: allows blurry frames. Rec: 0.10-0.25.
    static const int       kStillFramePatience = 8;          // Wait N frames of stillness before allowing matching. Low: hasty/unstable, High: sluggish. Rec: 5-15.
    static const int       kFailedMatchPatience = 10;        // Spawns new sub-canvas after N consecutive match failures.

    // Worker queue
    static const int       kQueueDepth         = 1;          // Buffer for incoming frames. Higher increases lag but prevents dropped frames. Rec: 1-2.

    // Anti-ghosting layers (each can be toggled independently)
    static constexpr bool  kEnableProximitySuppression = true;  // Dilate existing ink to suppress nearby new strokes.
    static constexpr bool  kEnableGridReplace          = true;  // Replace cell content when IoU is low (content changed).
    static constexpr bool  kEnableGhostBlock           = true;  // Block painting cells where overlap is high (duplicate ghost).
    static constexpr bool  kEnableAbsenceErasure       = true;  // Erase canvas cells when strokes disappear for N frames.

    static const int       kProximityRadius     = 30;        // Pixel radius to suppress nearby matches (at reference res). Rec: 10-30.
    static const int       kGridCellSize        = 100;       // Grid size for content density checks (at reference res). Rec: 100-400.
    static const int       kMinCellStrokePixels = 50;         // Min pixels in cell to consider it "full" (at reference res). Rec: 30-100.
    static constexpr float kCellReplaceIoU      = 0.40f;       // Overlap threshold to replace old data. Low: messy layers, High: stubborn old data. Rec: 0.1-0.4.
    static constexpr float kCellGhostOverlap    = 0.35f;       // Overlap threshold to flag a "ghost" stroke. Low: aggressive erasures, High: trails visible. Rec: 0.15-0.35.
    static const int       kAbsenceEraseFrames  = 5;          // Count of frames where stroke is missing to erase. Low: flickering, High: slow erase. Rec: 3-10.
    static const int       kAbsenceEraseThr     = 10;         // Intensity threshold for "missing" detection. Low: sensitive to shadows, High: ignores erasures. Rec: 5-20.

    // Raw canvas quality (each can be toggled independently)
    static constexpr bool  kEnableBlurRejection   = true;   // Skip painting raw frame when it's blurry (camera in motion).
    static constexpr bool  kEnableRawEdgeFeather   = false;  // Fade out frame edges to blend seams in raw canvas.
    static constexpr float kBlurThreshold          = 30.0f;  // Laplacian variance below this = blurry. Low: strict, High: permissive. Rec: 30-80.
    static const int       kRawEdgeMargin          = 30;     // Pixels to crop from each edge of raw frame (at reference res). Rec: 15-50.
    static const int       kRawFeatherWidth        = 40;     // Width of the fade gradient at edges (at reference res). Rec: 20-60.

    // Contour matching
    static constexpr float  kRectangleThreshold = 3.0f; // Aspect ratio threshold (max(w/h, h/w)). Strokes exceeding this are filtered out as "rectangular" artifacts (like board edges).
    static constexpr double kMaxShapeDist    = 0.7; // matchShapes threshold. Low: strict/fewer matches, High: loose/false positives. Rec: 0.3-0.7.
    static const int        kMinContourArea  = 30;   // px² — filter noise (at reference res). Rec: 10-100.
    static const int        kMinShapeVotes   = 5;    // min matched pairs to accept shift. Low: unstable/random jumps, High: hard to lock on. Rec: 2-5.

    // -----------------------------------------------------------------------
    // Chunk Grid constants
    // -----------------------------------------------------------------------
    static const int kChunkWidth = 512;              // Width of internal memory tiles (px). Do not change without re-tuning. Rec: 512.
    int chunk_height_ = 0;                           // Height of chunks = frame_h_, set on first frame.

    // Resolution-relative helpers: scale a reference-res value to actual frame_h_
    static const int kReferenceHeight = 1080;
    int ScalePx(int ref_val) const { return std::max(1, (int)std::round((float)ref_val * frame_h_ / kReferenceHeight)); }
    int ScaleArea(int ref_val) const { return std::max(1, (int)std::round((float)ref_val * frame_h_ / kReferenceHeight * frame_h_ / kReferenceHeight)); }

    // -----------------------------------------------------------------------
    // Sub-canvas collection (protected by state_mutex_)
    // -----------------------------------------------------------------------
    std::vector<std::unique_ptr<WhiteboardGroup>> groups_; // List of all captured whiteboard segments (sub-canvases).
    int active_group_idx_ = -1;                      // Index of the canvas currently being painted into by the camera.
    int view_group_idx_   = -1;                      // Index of the canvas currently shown on the UI screen.
    
    cv::Point2f global_camera_pos_{0, 0};            // Top-left pixel coordinate of current frame relative to canvas origin.
    int frame_w_ = 0;    // Original camera frame width (cached from first frame).
    int frame_h_ = 0;    // Original camera frame height (cached from first frame).

    // -----------------------------------------------------------------------
    // Worker thread and queue
    // -----------------------------------------------------------------------
    std::thread              worker_thread_;         // Dedicated thread for CV heavy lifting (YOLO results -> Canvas).
    std::mutex               queue_mutex_;           // Mutex protecting the work_queue_ from concurrent access.
    std::condition_variable  queue_cv_;              // Signal for the worker thread when new frames arrive.
    std::queue<CanvasWorkItem> work_queue_;          // Queue of frames and masks waiting for stitching.
    std::atomic<bool>        stop_worker_{false};    // Lifecycle flag to shut down the worker thread safely.
    std::unique_ptr<WhiteboardCanvasHelperClient> helper_client_; // Client for IPC/Remote processing offloading.
    bool                     remote_process_ = false; // Whether matching is happening via helper_client vs local.

    // -----------------------------------------------------------------------
    // State mutex
    // -----------------------------------------------------------------------
    mutable std::mutex state_mutex_;                 // Protects all canvas data (groups, chunks, pos) for UI rendering.

    // -----------------------------------------------------------------------
    // Motion gate
    // -----------------------------------------------------------------------
    cv::Mat prev_gray_;                              // Store for previous frame to compute frame-to-frame delta.
    cv::Mat prev_frame_gray_;                        // Full-res gray of previous successfully matched frame (for LK flow).
    std::vector<cv::Point2f> prev_tracked_points_;   // Contour centroids from the previous matched frame (for LK flow).
    int     matched_frame_counter_ = 0;              // Total count of frames that successfully matched to the canvas.
    int     consecutive_failed_matches_ = 0;         // Counter for consecutive match failures.
    float   last_match_accuracy_ = 0.0f;             // Voting consensus (votes) of the last successful match.

    // -----------------------------------------------------------------------
    // Canvas contour cache (rebuilt when new strokes are painted)
    // -----------------------------------------------------------------------
    struct ContourShape {
        std::vector<cv::Point> contour;              // Raw list of hull/outline points for the stroke.
        cv::Point2f            centroid;             // Geometric center in canvas space.
        double                 hu[7];                // Scale/Rotation invariant Hu Moments (raw).
        double                 area = 0.0;           // Area of the contour in pixels.
    };
    std::vector<ContourShape> canvas_contours_;      // Cached shapes of everything currently on the active canvas.
    bool canvas_contours_dirty_ = true;              // Flag to trigger cache rebuild after new painting.

    // -----------------------------------------------------------------------
    // Atomic flags
    // -----------------------------------------------------------------------
    std::atomic<bool> has_content_{false};           // True if any strokes have been painted across all canvases.
    std::atomic<bool> canvas_view_mode_{false};      // UI state: true when viewing the full panorama instead of live feed.
    std::atomic<int>  render_mode_{static_cast<int>(CanvasRenderMode::kRaw)}; // Mode for canvas rendering (Strokes/Raw).

    // -----------------------------------------------------------------------
    // Debug
    // -----------------------------------------------------------------------
    struct ScDebugInfo {
        std::string window_name;
        int         debug_id  = -1;
        bool        closed    = false;
    };
    std::vector<ScDebugInfo> sc_debug_infos_;        // Metadata for OpenCV debug windows.
    int next_debug_id_ = 0;                          // Counter for unique debug window naming.

    struct PipelineDebugState {
        cv::Mat frame;                               // Raw frame for the debug grid.
        cv::Mat gray_clean;                          // Grayscale input.
        cv::Mat enhanced_bgr;                        // High-contrast normalized frame.
        cv::Mat binary;                              // Thresholded stroke mask.
        cv::Mat person_mask;                         // Mask to exclude people from matching.
        float   motion_fraction    = 0.0f;           // Calculated frame motion % for current frame.
        int     stroke_pixel_count = 0;              // Count of candidate stroke pixels found.
        cv::Point2f shift;                           // Calculated alignment shift (dx, dy).
        std::string match_status;                    // Text description of matching result (OK, Fail, etc).
        bool    created_new_sc     = false;          // True if this frame triggered a sub-canvas split.
    };

    struct PerformanceStats {
        double stage1_motion_ms = 0.0;               // Time spent on motion detection (ms).
        double stage3_enhance_ms = 0.0;              // Time spent on image normalization/enhancement (ms).
        double stage4_matching_ms = 0.0;             // Time spent on contour matching and alignment (ms).
        double stage5_painting_ms = 0.0;             // Time spent writing pixels to memory chunks (ms).
        double total_frame_contours = 0.0;           // Number of contours extracted from current frame.
        double total_canvas_contours = 0.0;          // Number of contours searched on the canvas.
        double total_candidate_pairs = 0.0;          // Count of shape pairs that passed initial filters.
        double total_accepted_pairs = 0.0;           // Count of shape pairs that survived voting.
        double total_best_votes = 0.0;               // Consensus count for the chosen alignment shift.
        int frame_count = 0;                         // Total frame count processed.
        int matched_frames = 0;                      // Total frames aligned successfully.
        std::chrono::time_point<std::chrono::steady_clock> last_print_time; // Throttle for console logging.
    } perf_stats_;

    // -----------------------------------------------------------------------
    // Internal methods (all run on worker_thread_ unless noted)
    // -----------------------------------------------------------------------
    void WorkerLoop();
    void ProcessFrameInternal(const cv::Mat& frame, const cv::Mat& person_mask);

    // Contour helpers
    std::vector<ContourShape> ExtractContourShapes(const cv::Mat& binary,
                                                   cv::Point2f roi_offset) const;
    void RebuildCanvasContours(WhiteboardGroup& group);

    // Extract a gray region from canvas chunks at global position.
    cv::Mat GetCanvasGrayRegion(WhiteboardGroup& group, int global_x, int global_y,
                                int width, int height);

    // Sub-pixel refinement methods (called after coarse contour match in stage 4)
    bool RefineWithECC(WhiteboardGroup& group, const cv::Mat& gray,
                       cv::Point2f& pos, int bin_size);
    bool RefineWithTemplate(WhiteboardGroup& group, const cv::Mat& binary, const cv::Mat& gray,
                            cv::Point2f& pos);
    bool RefineWithLK(const cv::Mat& gray,
                      const std::vector<ContourShape>& frame_contours,
                      cv::Point2f& pos);

    // Chunk Grid management
    uint64_t GetChunkHash(int grid_x, int grid_y) const;
    void EnsureChunkAllocated(WhiteboardGroup& group, int grid_x, int grid_y);

    // Matching helpers
    bool MatchContours(const std::vector<ContourShape>& frame_contours,
                       const std::vector<ContourShape>& canvas_contours,
                       cv::Point2f& out_pos, int& out_votes, int binSize);

    void PaintStrokesToChunks(WhiteboardGroup& group, const cv::Mat& binary,
                              const cv::Mat& enhanced_bgr, cv::Point2f camera_pos,
                              const cv::Mat& no_update_mask = cv::Mat());
    void PaintRawFrameToChunks(WhiteboardGroup& group, const cv::Mat& frame_bgr,
                               cv::Point2f camera_pos,
                               const cv::Mat& no_update_mask = cv::Mat());
    void RebuildStrokeRenderCache(WhiteboardGroup& group);
    void RebuildRawRenderCache(WhiteboardGroup& group);

    // Create a new group (canvas) seeded with the current frame.
    void CreateSubCanvas(const cv::Mat& frame_bgr, const cv::Mat& binary,
                         const cv::Mat& enhanced_bgr,
                         const std::vector<ContourShape>& seed_contours,
                         const cv::Mat& stroke_no_update_mask = cv::Mat(),
                         const cv::Mat& raw_no_update_mask = cv::Mat());
    
    // Debug: 3x2 tile grid showing pipeline stages
    void RenderDebugGrid(const PipelineDebugState& state);
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
    __declspec(dllexport) bool    GetCanvasViewportRgba(uint8_t* buffer, int width, int height, float panX, float panY, float zoom);

    __declspec(dllexport) int     GetSubCanvasCount();
    __declspec(dllexport) int     GetActiveSubCanvasIndex();
    __declspec(dllexport) void    SetActiveSubCanvas(int idx);
    __declspec(dllexport) int     GetSortedSubCanvasIndex(int pos);
    __declspec(dllexport) int     GetSortedPosition(int idx);

    __declspec(dllexport) void    SetWhiteboardDebug(bool enabled);
    __declspec(dllexport) void    SetCanvasEnhanceThreshold(float threshold);
}
