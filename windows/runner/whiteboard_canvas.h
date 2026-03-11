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
    cv::Mat stroke_canvas;   // CV_8UC3, white-initialized, exactly 512x512
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

    // Spatial hash of 512x512 chunks
    std::unordered_map<uint64_t, std::unique_ptr<Chunk>> chunks;

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
    static const int       kMotionForceInterval = 1;         // Process every N-th frame even with no motion. 0=disabled. Rec: 3-8.

    // Alignment improvements (each can be toggled independently)
    static constexpr bool  kEnableJumpRejection      = true;  // Reject matches that jump too far from previous position.
    static constexpr bool  kEnableNeighborBinMerge     = true;  // Merge winning vote bin with 8 neighbors for robustness.
    static constexpr bool  kEnablePhaseCorrelation     = true;  // Sub-pixel refinement via cv::phaseCorrelate after coarse match.

    static constexpr float kMaxJumpPx              = 40.0f;  // Max allowed position jump (px). Rec: 30-60.
    static const int       kVoteBinSize            = 5;       // Vote bin quantisation (px). Smaller = finer. Rec: 5-15.

    // Sub-canvas creation
    static const int       kMinStrokePixelsForNewSC = 500;   // Min whiteboard pixels to spawn a new canvas. Low: noisy starts, High: missed content. Rec: 300-1000.
    static const int       kMotionLongEdge = 256;            // Downscale size for motion detection. Low: faster but blind to fine motion, High: slow. Rec: 128-512.
    static constexpr float kMinMotionFraction = 0.01f;       // Min changed pixels to process frame. Low: processes noise, High: ignores slow movement. Rec: 0.005-0.03.
    static constexpr float kMaxMotionFraction = 0.15f;       // Max changed pixels to process frame. Low: ignores fast pans, High: allows blurry frames. Rec: 0.10-0.25.
    static const int       kStillFramePatience = 8;          // Wait N frames of stillness before allowing matching. Low: hasty/unstable, High: sluggish. Rec: 5-15.

    // Worker queue
    static const int       kQueueDepth         = 1;          // Buffer for incoming frames. Higher increases lag but prevents dropped frames. Rec: 1-2.

    // Anti-ghosting layers (each can be toggled independently)
    static constexpr bool  kEnableProximitySuppression = true;  // Dilate existing ink to suppress nearby new strokes.
    static constexpr bool  kEnableGridReplace          = true;  // Replace cell content when IoU is low (content changed).
    static constexpr bool  kEnableGhostBlock           = true;  // Block painting cells where overlap is high (duplicate ghost).
    static constexpr bool  kEnableAbsenceErasure       = true;  // Erase canvas cells when strokes disappear for N frames.

    static const int       kProximityRadius     = 30;        // Pixel radius to suppress nearby matches. Low: duplicates, High: ignores dense content. Rec: 10-30.
    static const int       kGridCellSize        = 100;       // Grid size for content density checks. Low: precise/slow, High: coarse. Rec: 100-400.
    static const int       kMinCellStrokePixels = 50;         // Min pixels in cell to consider it "full". Low: over-sensitive, High: lets ghosts stay. Rec: 30-100.
    static constexpr float kCellReplaceIoU      = 0.40f;       // Overlap threshold to replace old data. Low: messy layers, High: stubborn old data. Rec: 0.1-0.4.
    static constexpr float kCellGhostOverlap    = 0.35f;       // Overlap threshold to flag a "ghost" stroke. Low: aggressive erasures, High: trails visible. Rec: 0.15-0.35.
    static const int       kAbsenceEraseFrames  = 5;          // Count of frames where stroke is missing to erase. Low: flickering, High: slow erase. Rec: 3-10.
    static const int       kAbsenceEraseThr     = 10;         // Intensity threshold for "missing" detection. Low: sensitive to shadows, High: ignores erasures. Rec: 5-20.

    // Stroke locality mask (restrict painting to regions near actual strokes)
    static constexpr bool  kEnableStrokeLocality    = true;   // Only paint within R px of detected strokes. Eliminates edge artifacts.
    static const int       kStrokeLocalityRadius    = 30;     // Dilation radius around strokes (px). Low: tight/clips, High: wider/less filtering. Rec: 40-80.
    static const int       kLocalityNoiseErode      = 3;      // Morph-open kernel to remove tiny noise before dilation. 0=disabled. Rec: 3-5.

    // Straight-line suppression (removes long h/v lines from frame edges picked up by threshold)
    static constexpr bool  kEnableLineSuppression       = true;   // Detect and remove long straight horizontal/vertical lines from binary.
    static constexpr float kLineSuppressionLengthFrac   = 0.25f;  // Min line length as fraction of frame dim. Low: aggressive, High: only very long lines. Rec: 0.15-0.35.

    // Frame edge margins (left/right binary suppression, complements existing top/bottom crop)
    static constexpr float kFrameEdgeMarginLeftPct  = 0.03f;  // Fraction of width to crop from left. Rec: 0.02-0.05.
    static constexpr float kFrameEdgeMarginRightPct = 0.03f;  // Fraction of width to crop from right. Rec: 0.02-0.05.

    // Raw canvas quality (each can be toggled independently)
    static constexpr bool  kEnableBlurRejection   = true;   // Skip painting raw frame when it's blurry (camera in motion).
    static constexpr bool  kEnableRawEdgeFeather   = false;  // Fade out frame edges to blend seams in raw canvas.
    static constexpr float kBlurThreshold          = 30.0f;  // Laplacian variance below this = blurry. Low: strict, High: permissive. Rec: 30-80.
    static const int       kRawEdgeMargin          = 30;     // Pixels to crop from each edge of raw frame. Rec: 15-50.
    static const int       kRawFeatherWidth        = 40;     // Width of the fade gradient at edges (px). Rec: 20-60.

    // Contour matching
    static constexpr double kMaxShapeDist    = 0.5; // matchShapes threshold. Low: strict/fewer matches, High: loose/false positives. Rec: 0.3-0.7.
    static const int        kMinContourArea  = 30;   // px² — filter noise. Low: keeps dust/artifacts, High: ignores small dots/letters. Rec: 10-100.
    static const int        kMinShapeVotes   = 5;    // min matched pairs to accept shift. Low: unstable/random jumps, High: hard to lock on. Rec: 2-5.

    // -----------------------------------------------------------------------
    // Chunk Grid constants
    // -----------------------------------------------------------------------
    static const int kChunkSize = 512;               // Size of internal memory tiles (px). Do not change without re-tuning. Rec: 512.
    static const int kDefaultCanvasWidth = 1920;      // Initial view width if no content exists. Rec: 1280-1920.
    static const int kDefaultCanvasHeight = 1080;     // Initial view height if no content exists. Rec: 720-1080.

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
    int     frames_since_warp_ = 0;                  // Frames elapsed since the last successful spatial lock/alignment.
    int     matched_frame_counter_ = 0;              // Total count of frames that successfully matched to the canvas.
    int     motion_frame_counter_ = 0;               // Counter for force-processing every N frames.

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

    // Phase correlation: extract a gray region from canvas chunks at global position.
    cv::Mat GetCanvasGrayRegion(WhiteboardGroup& group, int global_x, int global_y,
                                int width, int height);

    
    // Chunk Grid management
    uint64_t GetChunkHash(int grid_x, int grid_y) const;
    void EnsureChunkAllocated(WhiteboardGroup& group, int grid_x, int grid_y);
    void PaintStrokesToChunks(WhiteboardGroup& group, const cv::Mat& binary,
                              const cv::Mat& enhanced_bgr, cv::Point2f camera_pos,
                              const cv::Mat& no_update_mask = cv::Mat());
    void PaintRawFrameToChunks(WhiteboardGroup& group, const cv::Mat& frame_bgr,
                               cv::Point2f camera_pos,
                               const cv::Mat& no_update_mask = cv::Mat(),
                               const cv::Mat& binary = cv::Mat());
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
