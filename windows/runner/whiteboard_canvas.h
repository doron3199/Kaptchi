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
#include <thread>
#include <vector>
#include <unordered_map>
#include <chrono>

// ---------------------------------------------------------------------------
// Chunk -- one fixed-size tile of the virtual infinite whiteboard
// ---------------------------------------------------------------------------
struct Chunk {
    cv::Mat canvas;          // CV_8UC3, white-initialized, exactly 512x512
    cv::Mat absence_counter; // CV_8UC1, zero-initialized, tracking erase frames
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

    // The bounds of all allocated chunks (in pixels)
    int min_px_x = 0;
    int min_px_y = 0;
    int max_px_x = 512;
    int max_px_y = 512;

    // Rendered full canvas for UI viewport
    cv::Mat render_cache;
    bool cache_dirty = true;
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

    // --- State control ---
    void Reset();
    bool HasContent() const;
    bool IsCanvasViewMode() const;
    void SetCanvasViewMode(bool mode);
    cv::Size GetCanvasSize() const;

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

   
    // Sub-canvas creation
    static const int       kMinStrokePixelsForNewSC = 500;

    // Worker queue
    static const int       kQueueDepth         = 2;

    // Anti-ghosting layers
    static const int       kProximityRadius     = 15;
    static const int       kGridCellSize        = 100;
    static const int       kMinCellStrokePixels = 50;
    static constexpr float kCellReplaceIoU      = 0.20f;
    static constexpr float kCellGhostOverlap    = 0.25f;
    static const int       kAbsenceEraseFrames  = 5;
    static const int       kAbsenceEraseThr     = 10;

    // Contour matching
    static constexpr double kMaxShapeDist    = 0.35; // matchShapes threshold
    static const int        kMinContourArea  = 80;   // px² — filter noise
    static const int        kMinShapeVotes   = 3;    // min matched pairs to accept shift

    // -----------------------------------------------------------------------
    // Chunk Grid constants
    // -----------------------------------------------------------------------
    static const int kChunkSize = 512;

    // -----------------------------------------------------------------------
    // Sub-canvas collection (protected by state_mutex_)
    // -----------------------------------------------------------------------
    std::vector<std::unique_ptr<WhiteboardGroup>> groups_;
    int active_group_idx_ = -1;
    int view_group_idx_   = -1;
    
    cv::Point2f global_camera_pos_{0, 0}; // Top-left pixel of current frame in global space
    
    int frame_w_ = 0;    // Frame dimensions (set from first input frame)
    int frame_h_ = 0;

    // -----------------------------------------------------------------------
    // Worker thread and queue
    // -----------------------------------------------------------------------
    std::thread              worker_thread_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::queue<CanvasWorkItem> work_queue_;
    std::atomic<bool>        stop_worker_{false};

    // -----------------------------------------------------------------------
    // State mutex
    // -----------------------------------------------------------------------
    mutable std::mutex state_mutex_;

    // -----------------------------------------------------------------------
    // Motion gate
    // -----------------------------------------------------------------------
    cv::Mat prev_gray_;
    int     frames_since_warp_ = 0;

    // -----------------------------------------------------------------------
    // Canvas contour cache (rebuilt when new strokes are painted)
    // -----------------------------------------------------------------------
    struct ContourShape {
        std::vector<cv::Point> contour;
        cv::Point2f            centroid;  // in canvas pixel coords
        double                 hu[7];
    };
    std::vector<ContourShape> canvas_contours_;
    bool canvas_contours_dirty_ = true;

    // -----------------------------------------------------------------------
    // Atomic flags
    // -----------------------------------------------------------------------
    std::atomic<bool> has_content_{false};
    std::atomic<bool> canvas_view_mode_{false};

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
        cv::Mat person_mask;
        float   motion_fraction    = 0.0f;
        int     stroke_pixel_count = 0;
        cv::Point2f shift;
        std::string match_status;
        bool    created_new_sc     = false;
    };

    struct PerformanceStats {
        double stage1_motion_ms = 0.0;
        double stage3_enhance_ms = 0.0;
        double stage4_matching_ms = 0.0;
        double stage5_painting_ms = 0.0;
        int frame_count = 0;
        std::chrono::time_point<std::chrono::steady_clock> last_print_time;
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

    
    // Chunk Grid management
    uint64_t GetChunkHash(int grid_x, int grid_y) const;
    void EnsureChunkAllocated(WhiteboardGroup& group, int grid_x, int grid_y);
    void PaintStrokesToChunks(WhiteboardGroup& group, const cv::Mat& binary, const cv::Mat& enhanced_bgr, cv::Point2f camera_pos);
    void RebuildRenderCache(WhiteboardGroup& group);

    // Create a new group (canvas) seeded with the current frame.
    void CreateSubCanvas(const cv::Mat& binary, const cv::Mat& enhanced_bgr,
                         const std::vector<ContourShape>& seed_contours);
    
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
    __declspec(dllexport) int64_t GetCanvasTextureId();

    __declspec(dllexport) int     GetSubCanvasCount();
    __declspec(dllexport) int     GetActiveSubCanvasIndex();
    __declspec(dllexport) void    SetActiveSubCanvas(int idx);
    __declspec(dllexport) int     GetSortedSubCanvasIndex(int pos);
    __declspec(dllexport) int     GetSortedPosition(int idx);

    __declspec(dllexport) void    SetWhiteboardDebug(bool enabled);
    __declspec(dllexport) void    SetCanvasEnhanceThreshold(float threshold);
}
