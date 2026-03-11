// ============================================================================
// whiteboard_canvas.cpp -- ML Box-Anchored Canvas Stitcher
//
// PIPELINE OVERVIEW (runs on worker thread, once per accepted frame):
//
//   +---------------+
//   | [1] Motion    |  Frame-diff vs previous. Skip if motion is too small
//   |     Gate      |  or too large; still frames force processing after 8 skips.
//   +-------+-------+
//           |
//   +-------v-------+
//   | [2] Person    |  Gaussian-blended whitewash of detected people to white.
//   |   Whitewash   |  Dilated mask excludes person zone from binary.
//   +-------+-------+
//           |
//   +-------v-------+
//   | [3] Enhance   |  WhiteboardEnhance -> adaptiveThreshold -> binary mask.
//   |   + Binarize  |  YOLO run on frame; YOLO run on canvas (if cache dirty).
//   +-------+-------+
//           |
//   +-------v-------+
//   | [4] Match     |  Contour-shape voting via `cv::matchShapes`.
//   | via Contours  |  Matching contour pairs vote for a dx,dy camera shift.
//   |               |  Best-voted bin -> camera position.
//   +-------+-------+
//           |
//   +-------v-------+  If matched: two-pass stitch (footprint mask once, scatter
//   | [5] Paint or  |  new pixels per chunk). Canvas grows as needed.
//   |   Create SC   |  If no canvas yet: first frame seeds a new group.
//   +-------+-------+
//           |
//   +-------v-------+
//   | [6] Debug     |  Optional: 3x2 tile grid
//   |    Display    |
//   +---------------+
//
// KEY INSIGHT: Duplicates are prevented at the pixel level -- we compare
// the frame's binary strokes against the canvas's existing strokes at the
// exact mapped position, and only paint pixels that are genuinely new.
//
// ============================================================================

// KEY INSIGHT: Duplicates are prevented at the pixel level -- we compare
// the frame's binary strokes against the canvas's existing strokes at the
// exact mapped position, and only paint pixels that are genuinely new.
//
// ============================================================================

#include "whiteboard_canvas.h"
#include "whiteboard_canvas_process.h"
#include "native_camera.h"
#include "whiteboard_enhance.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

// ============================================================================
//  SECTION 1: Global state
// ============================================================================

WhiteboardCanvas*  g_whiteboard_canvas  = nullptr;
std::atomic<bool>  g_whiteboard_enabled{false};
std::atomic<float> g_canvas_pan_x{0.5f};
std::atomic<float> g_canvas_pan_y{0.5f};
std::atomic<float> g_canvas_zoom{1.0f};
std::atomic<bool>  g_whiteboard_debug{false};
std::atomic<float> g_yolo_fps{2.0f};
std::atomic<float> g_canvas_enhance_threshold{5.0f};

// ============================================================================
//  SECTION 2: Static helpers
// ============================================================================

namespace {

static std::mutex& WhiteboardLogMutex() {
    static std::mutex mutex;
    return mutex;
}

static const std::string& GetWhiteboardLogPath() {
    static const std::string path = []() {
        char module_path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return std::string("whiteboard_canvas.log");
        }

        std::string full_path(module_path, length);
        const size_t sep = full_path.find_last_of("\\/");
        if (sep == std::string::npos) {
            return std::string("whiteboard_canvas.log");
        }

        return full_path.substr(0, sep + 1) + "whiteboard_canvas.log";
    }();

    return path;
}

static std::string FormatWhiteboardLogLine(const std::string& message) {
    SYSTEMTIME local_time;
    GetLocalTime(&local_time);

    std::ostringstream stream;
    stream << '['
           << std::setfill('0') << std::setw(2) << local_time.wHour << ':'
           << std::setw(2) << local_time.wMinute << ':'
           << std::setw(2) << local_time.wSecond << '.'
           << std::setw(3) << local_time.wMilliseconds << "] "
           << message;
    return stream.str();
}

static void WhiteboardLog(const std::string& message, bool is_error = false) {
    const std::string line = FormatWhiteboardLogLine(message);

    std::lock_guard<std::mutex> lock(WhiteboardLogMutex());
    if (is_error) {
        std::cerr << line << std::endl;
    } else {
        std::cout << line << std::endl;
    }

    std::ofstream file(GetWhiteboardLogPath(), std::ios::app);
    if (file.is_open()) {
        file << line << std::endl;
    }

    std::string debug_line = line + "\n";
    OutputDebugStringA(debug_line.c_str());
}

static float ComputeScaleForLongEdge(const cv::Size& size, int max_long_edge) {
    if (size.width <= 0 || size.height <= 0 || max_long_edge <= 0) {
        return 1.0f;
    }

    const int long_edge = std::max(size.width, size.height);
    if (long_edge <= max_long_edge) {
        return 1.0f;
    }

    return static_cast<float>(max_long_edge) / static_cast<float>(long_edge);
}

static void DebugText(cv::Mat& img, const std::string& text,
                      cv::Point pos, double scale = 0.6,
                      cv::Scalar color = cv::Scalar(0, 255, 0)) {
    int thick = std::max(1, (int)(scale * 1.5));
    cv::putText(img, text, pos + cv::Point(1, 1),
                cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), thick + 1);
    cv::putText(img, text, pos,
                cv::FONT_HERSHEY_SIMPLEX, scale, color, thick);
}

static const cv::Mat& GetRenderCacheForMode(const WhiteboardGroup& group,
                                            CanvasRenderMode mode) {
    return mode == CanvasRenderMode::kRaw
        ? group.raw_render_cache
        : group.stroke_render_cache;
}

static bool GetRenderBoundsForMode(const WhiteboardGroup& group,
                                   CanvasRenderMode mode,
                                   int& min_px_x,
                                   int& min_px_y,
                                   int& max_px_x,
                                   int& max_px_y) {
    if (mode == CanvasRenderMode::kRaw) {
        min_px_x = group.raw_min_px_x;
        min_px_y = group.raw_min_px_y;
        max_px_x = group.raw_max_px_x;
        max_px_y = group.raw_max_px_y;
    } else {
        min_px_x = group.stroke_min_px_x;
        min_px_y = group.stroke_min_px_y;
        max_px_x = group.stroke_max_px_x;
        max_px_y = group.stroke_max_px_y;
    }

    return max_px_x > min_px_x && max_px_y > min_px_y;
}

static const char* RenderModeName(CanvasRenderMode mode) {
    return mode == CanvasRenderMode::kRaw ? "RAW" : "STROKE";
}

} // namespace

// ============================================================================
//  SECTION 3: Constructor / Destructor
// ============================================================================

WhiteboardCanvas::WhiteboardCanvas() {
    stop_worker_ = false;
    perf_stats_.last_print_time = std::chrono::steady_clock::now();

    WhiteboardLog("[WhiteboardCanvas] Logging to " + GetWhiteboardLogPath());

    if (!IsWhiteboardCanvasHelperProcess()) {
        auto helper_client = std::make_unique<WhiteboardCanvasHelperClient>();
        if (helper_client && helper_client->Start()) {
            helper_client_ = std::move(helper_client);
            remote_process_ = true;
            helper_client_->SetCanvasViewMode(canvas_view_mode_.load());
            helper_client_->SetRenderMode(GetRenderMode());
            SyncRuntimeSettings();
            WhiteboardLog("[WhiteboardCanvas] Initialized (helper-process client)");
            return;
        }

        WhiteboardLog("[WhiteboardCanvas] Helper process unavailable, using in-process worker");
    }

    worker_thread_ = std::thread(&WhiteboardCanvas::WorkerLoop, this);
    WhiteboardLog("[WhiteboardCanvas] Initialized (phase-correlation)");
}

WhiteboardCanvas::~WhiteboardCanvas() {
    if (remote_process_) {
        if (helper_client_) {
            helper_client_->Stop();
            helper_client_.reset();
        }
        return;
    }

    stop_worker_ = true;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void WhiteboardCanvas::SyncRuntimeSettings() {
    if (remote_process_ && helper_client_) {
        helper_client_->SyncSettings(g_whiteboard_debug.load(),
                                     g_canvas_enhance_threshold.load(),
                                     g_yolo_fps.load());
    }
}

// ============================================================================
//  SECTION 4: Public API
// ============================================================================

void WhiteboardCanvas::ProcessFrame(const cv::Mat& frame, const cv::Mat& person_mask) {
    if (frame.empty()) return;

    if (remote_process_ && helper_client_) {
        SyncRuntimeSettings();
        helper_client_->ProcessFrame(frame, person_mask);
        return;
    }

    CanvasWorkItem item;
    frame.copyTo(item.frame);
    if (!person_mask.empty()) {
        person_mask.copyTo(item.person_mask);
    }

    std::unique_lock<std::mutex> lock(queue_mutex_);
    while ((int)work_queue_.size() >= kQueueDepth) {
        work_queue_.pop();
    }
    work_queue_.push(std::move(item));
    lock.unlock();
    queue_cv_.notify_one();
}

bool WhiteboardCanvas::GetViewport(float panX, float panY, float zoom,
                                    cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetViewport(panX, panY, zoom, viewSize, out_frame);
    }

    std::unique_lock<std::mutex> lock(state_mutex_);

    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];

    const CanvasRenderMode render_mode = GetRenderMode();
    if (render_mode == CanvasRenderMode::kRaw) {
        if (group.raw_cache_dirty) {
            RebuildRawRenderCache(group);
            group.raw_cache_dirty = false;
        }
    } else {
        if (group.stroke_cache_dirty) {
            RebuildStrokeRenderCache(group);
            group.stroke_cache_dirty = false;
        }
    }

    const cv::Mat& render_cache = GetRenderCacheForMode(group, render_mode);
    if (render_cache.empty()) return false;

    zoom = std::max(1.0f, zoom);
    int cw = render_cache.cols, ch = render_cache.rows;

    float view_aspect = (float)viewSize.width / (float)viewSize.height;
    float roi_h = (float)ch / zoom;
    float roi_w = roi_h * view_aspect;
    if (roi_w > (float)cw) { roi_w = (float)cw; roi_h = roi_w / view_aspect; }
    if (roi_h > (float)ch) { roi_h = (float)ch; roi_w = roi_h * view_aspect; }

    float max_cx = (float)cw - roi_w, max_cy = (float)ch - roi_h;
    float cx = std::max(0.f, std::min(panX * max_cx, max_cx));
    float cy = std::max(0.f, std::min(panY * max_cy, max_cy));

    cv::Rect roi((int)cx, (int)cy, (int)roi_w, (int)roi_h);
    if (roi.x + roi.width  > cw) roi.width  = cw - roi.x;
    if (roi.y + roi.height > ch) roi.height = ch - roi.y;
    cv::resize(render_cache(roi), out_frame, viewSize, 0, 0, cv::INTER_LINEAR);
    return true;
}

bool WhiteboardCanvas::GetOverview(cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetOverview(viewSize, out_frame);
    }

    std::unique_lock<std::mutex> lock(state_mutex_);

    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];

    const CanvasRenderMode render_mode = GetRenderMode();
    if (render_mode == CanvasRenderMode::kRaw) {
        if (group.raw_cache_dirty) {
            RebuildRawRenderCache(group);
            group.raw_cache_dirty = false;
        }
    } else {
        if (group.stroke_cache_dirty) {
            RebuildStrokeRenderCache(group);
            group.stroke_cache_dirty = false;
        }
    }

    const cv::Mat& render_cache = GetRenderCacheForMode(group, render_mode);
    if (render_cache.empty() || viewSize.width <= 0 || viewSize.height <= 0) {
        return false;
    }

    out_frame = cv::Mat(viewSize.height, viewSize.width, CV_8UC3,
                        cv::Scalar(255, 255, 255));

    const float src_aspect = (float)render_cache.cols /
                             (float)std::max(1, render_cache.rows);
    const float dst_aspect = (float)viewSize.width /
                             (float)std::max(1, viewSize.height);

    int draw_w = viewSize.width;
    int draw_h = viewSize.height;
    if (src_aspect > dst_aspect) {
        draw_h = std::max(1, (int)std::round(draw_w / src_aspect));
    } else {
        draw_w = std::max(1, (int)std::round(draw_h * src_aspect));
    }

    cv::Mat scaled;
    cv::resize(render_cache, scaled, cv::Size(draw_w, draw_h),
               0, 0, cv::INTER_AREA);

    const int offset_x = (viewSize.width - draw_w) / 2;
    const int offset_y = (viewSize.height - draw_h) / 2;
    scaled.copyTo(out_frame(cv::Rect(offset_x, offset_y, draw_w, draw_h)));
    return true;
}

bool WhiteboardCanvas::GetOverviewBlocking(cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetOverview(viewSize, out_frame);
    }

    if (viewSize.width <= 0 || viewSize.height <= 0) return false;

    std::lock_guard<std::mutex> lock(state_mutex_);

    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];

    const CanvasRenderMode render_mode = GetRenderMode();
    if (render_mode == CanvasRenderMode::kRaw) {
        if (group.raw_cache_dirty) {
            RebuildRawRenderCache(group);
            group.raw_cache_dirty = false;
        }
    } else {
        if (group.stroke_cache_dirty) {
            RebuildStrokeRenderCache(group);
            group.stroke_cache_dirty = false;
        }
    }

    const cv::Mat& render_cache = GetRenderCacheForMode(group, render_mode);
    if (render_cache.empty()) {
        return false;
    }

    out_frame = cv::Mat(viewSize.height, viewSize.width, CV_8UC3,
                        cv::Scalar(255, 255, 255));

    const float src_aspect = (float)render_cache.cols /
                             (float)std::max(1, render_cache.rows);
    const float dst_aspect = (float)viewSize.width /
                             (float)std::max(1, viewSize.height);

    int draw_w = viewSize.width;
    int draw_h = viewSize.height;
    if (src_aspect > dst_aspect) {
        draw_h = std::max(1, (int)std::round(draw_w / src_aspect));
    } else {
        draw_w = std::max(1, (int)std::round(draw_h * src_aspect));
    }

    cv::Mat scaled;
    cv::resize(render_cache, scaled, cv::Size(draw_w, draw_h),
               0, 0, cv::INTER_AREA);

    const int offset_x = (viewSize.width - draw_w) / 2;
    const int offset_y = (viewSize.height - draw_h) / 2;
    scaled.copyTo(out_frame(cv::Rect(offset_x, offset_y, draw_w, draw_h)));
    return true;
}

void WhiteboardCanvas::Reset() {
    if (remote_process_ && helper_client_) {
        helper_client_->Reset();
        has_content_ = false;
    
        return;
    }

    {
        std::lock_guard<std::mutex> q_lock(queue_mutex_);
        std::queue<CanvasWorkItem> empty;
        std::swap(work_queue_, empty);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    groups_.clear();
    active_group_idx_ = -1;
    view_group_idx_   = -1;
    global_camera_pos_ = cv::Point2f(0, 0);

    prev_gray_ = cv::Mat();
    canvas_contours_.clear();
    canvas_contours_dirty_ = true;
    has_content_ = false;
    if (g_whiteboard_debug.load()) {
        for (const auto& info : sc_debug_infos_)
            cv::destroyWindow(info.window_name);
        cv::destroyWindow("WB Full Canvas");
    }
    sc_debug_infos_.clear();
    next_debug_id_ = 0;
    frame_w_ = 0;
    frame_h_ = 0;
    frames_since_warp_ = 0;
    matched_frame_counter_ = 0;
    motion_frame_counter_ = 0;

    perf_stats_ = PerformanceStats();
    perf_stats_.last_print_time = std::chrono::steady_clock::now();
    WhiteboardLog("[WhiteboardCanvas] Reset");
}

bool WhiteboardCanvas::HasContent() const {
    if (remote_process_ && helper_client_) {
        return helper_client_->HasContent();
    }
    return has_content_.load();
}

bool WhiteboardCanvas::IsCanvasViewMode() const {
    if (remote_process_ && helper_client_) {
        return helper_client_->IsCanvasViewMode();
    }
    return canvas_view_mode_.load();
}

void WhiteboardCanvas::SetCanvasViewMode(bool m) {
    const bool was_canvas_view_mode = canvas_view_mode_.load();
    canvas_view_mode_ = m;

    if (remote_process_ && helper_client_) {
        helper_client_->SetCanvasViewMode(m);
        return;
    }

    if (m && !was_canvas_view_mode) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        view_group_idx_ = active_group_idx_;
        // Set inside lock so the worker's CreateSubCanvas sees the correct
        // value when checking canvas_view_mode_ to update view_group_idx_.
        canvas_view_mode_.store(m);
    } else {
        canvas_view_mode_.store(m);
    }
}

void WhiteboardCanvas::SetRenderMode(CanvasRenderMode mode) {
    render_mode_.store(static_cast<int>(mode), std::memory_order_relaxed);
    if (remote_process_ && helper_client_) {
        helper_client_->SetRenderMode(mode);
    }
}

CanvasRenderMode WhiteboardCanvas::GetRenderMode() const {
    return render_mode_.load(std::memory_order_relaxed) ==
            static_cast<int>(CanvasRenderMode::kRaw)
        ? CanvasRenderMode::kRaw
        : CanvasRenderMode::kStroke;
}

bool WhiteboardCanvas::IsRemoteProcess() const {
    return remote_process_ && helper_client_ && helper_client_->IsReady();
}

cv::Size WhiteboardCanvas::GetCanvasSize() const {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetCanvasSize();
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx >= 0 && idx < (int)groups_.size()) {
        const auto& group = *groups_[idx];
        const CanvasRenderMode render_mode = GetRenderMode();
        const cv::Mat& render_cache = GetRenderCacheForMode(group, render_mode);
        if (!render_cache.empty()) {
            return cv::Size(render_cache.cols, render_cache.rows);
        }
        int min_px_x = 0;
        int min_px_y = 0;
        int max_px_x = frame_w_ > 0 ? frame_w_ : kDefaultCanvasWidth;
        int max_px_y = frame_h_ > 0 ? frame_h_ : kDefaultCanvasHeight;
        GetRenderBoundsForMode(group, render_mode, min_px_x, min_px_y, max_px_x, max_px_y);
        return cv::Size(std::max(1, max_px_x - min_px_x),
                        std::max(1, max_px_y - min_px_y));
    }
    return cv::Size(frame_w_ > 0 ? frame_w_ : kDefaultCanvasWidth,
                    frame_h_ > 0 ? frame_h_ : kDefaultCanvasHeight);
}

int WhiteboardCanvas::GetSubCanvasCount() const {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetSubCanvasCount();
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    return (int)groups_.size();
}

int WhiteboardCanvas::GetActiveSubCanvasIndex() const {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetActiveSubCanvasIndex();
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    return canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
}

void WhiteboardCanvas::SetActiveSubCanvas(int idx) {
    if (remote_process_ && helper_client_) {
        helper_client_->SetActiveSubCanvas(idx);
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (idx >= 0 && idx < (int)groups_.size()) {
        if (canvas_view_mode_.load()) {
            view_group_idx_ = idx;
        } else {
            active_group_idx_ = idx;
        }
    }
}

// Ignore sorting logic as chunks now dictate position natively. Just returning straight indices for now.
int WhiteboardCanvas::GetSortedSubCanvasIndex(int pos) const {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetSortedSubCanvasIndex(pos);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pos < 0 || pos >= (int)groups_.size()) return -1;
    return pos;
}

int WhiteboardCanvas::GetSortedPosition(int idx) const {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetSortedPosition(idx);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (idx < 0 || idx >= (int)groups_.size()) return -1;
    return idx;
}

// ============================================================================
//  SECTION 5: Worker thread
// ============================================================================

void WhiteboardCanvas::WorkerLoop() {
    while (true) {
        CanvasWorkItem item;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stop_worker_.load() || !work_queue_.empty();
            });
            if (stop_worker_.load() && work_queue_.empty()) break;
            item = std::move(work_queue_.front());
            work_queue_.pop();
        }
        try {
            ProcessFrameInternal(item.frame, item.person_mask);
        } catch (const cv::Exception& e) {
            WhiteboardLog(std::string("[WhiteboardCanvas] CV exception: ") + e.what(), true);
        } catch (...) {
            WhiteboardLog("[WhiteboardCanvas] Unknown exception in worker", true);
        }
    }
}

// ============================================================================
//  SECTION 6: Main pipeline -- ProcessFrameInternal
// ============================================================================

void WhiteboardCanvas::ProcessFrameInternal(const cv::Mat& frame, const cv::Mat& person_mask) {
    const bool dbg = g_whiteboard_debug.load();

    auto t_start = std::chrono::steady_clock::now();

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    // -------------------------------------------------------------------
    // STAGE 1: Motion gate
    // -------------------------------------------------------------------
    float motion_fraction = 0.0f;

    if (kEnableMotionGate) {
        cv::Mat motion_gray;
        const bool has_content = has_content_.load();
        bool has_motion_reference = false;
        bool motion_too_low = false;
        bool motion_too_high = false;
        const float motion_scale = ComputeScaleForLongEdge(gray.size(), kMotionLongEdge);
        if (motion_scale < 0.999f) {
            cv::resize(gray, motion_gray, cv::Size(), motion_scale, motion_scale,
                       cv::INTER_AREA);
        } else {
            motion_gray = gray;
        }

        if (!prev_gray_.empty() && prev_gray_.size() == motion_gray.size()) {
            has_motion_reference = true;
            cv::Mat diff;
            cv::absdiff(motion_gray, prev_gray_, diff);
            cv::threshold(diff, diff, 10, 255, cv::THRESH_BINARY);
            motion_fraction = static_cast<float>(cv::countNonZero(diff)) /
                              static_cast<float>(std::max<size_t>(1, diff.total()));
        }

        motion_gray.copyTo(prev_gray_);

        if (has_content && has_motion_reference) {
            motion_too_low = motion_fraction < kMinMotionFraction &&
                             frames_since_warp_ < kStillFramePatience;
            motion_too_high = motion_fraction > kMaxMotionFraction;
        }

        if (motion_too_low) {
            frames_since_warp_++;
        } else {
            frames_since_warp_ = 0;
        }

        // Force-process every N frames regardless of motion
        motion_frame_counter_++;
        bool force_process = (kMotionForceInterval > 0)
                           && (motion_frame_counter_ >= kMotionForceInterval);
        if (force_process) {
            motion_frame_counter_ = 0;
        }

        {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(4)
                   << "[WhiteboardMotion] motion=" << motion_fraction
                   << " pct=" << (motion_fraction * 100.0f)
                   << " min=" << kMinMotionFraction
                   << " max=" << kMaxMotionFraction
                   << " hasPrev=" << (has_motion_reference ? 1 : 0)
                   << " hasContent=" << (has_content ? 1 : 0)
                   << " stillFrames=" << frames_since_warp_
                   << " forceIn=" << (kMotionForceInterval - motion_frame_counter_)
                   << " gate="
                   << (force_process ? "forced"
                                     : motion_too_low ? "below-min"
                                      : motion_too_high ? "above-max"
                                                        : has_motion_reference ? "pass"
                                                                               : "warmup");
            WhiteboardLog(stream.str());
        }

        if (!force_process && (motion_too_low || motion_too_high)) {

            auto t_motion = std::chrono::steady_clock::now();
            perf_stats_.stage1_motion_ms +=
                std::chrono::duration<double, std::milli>(t_motion - t_start).count();

            if (dbg) {
                PipelineDebugState ds;
                ds.frame = frame;
                ds.gray_clean = gray;
                ds.binary = cv::Mat(gray.size(), CV_8U, cv::Scalar(0));
                ds.person_mask = person_mask;
                ds.motion_fraction = motion_fraction;
                ds.match_status = motion_too_low
                    ? "motion-gated-low"
                    : "motion-gated-high";
                RenderDebugGrid(ds);
            }
            return;
        }
    }

    auto t_motion = std::chrono::steady_clock::now();
    perf_stats_.stage1_motion_ms +=
        std::chrono::duration<double, std::milli>(t_motion - t_start).count();

    // -------------------------------------------------------------------
    // STAGE 2: Person occlusion mask (used as a no-update zone)
    // -------------------------------------------------------------------
    bool has_person_mask = !person_mask.empty() && person_mask.size() == gray.size();
    cv::Mat person_mask_dilated;

    if (has_person_mask) {
        cv::Mat small_mask;
        cv::resize(person_mask, small_mask, cv::Size(), 0.25, 0.25,
                   cv::INTER_NEAREST);

        int pad = std::max(1, (int)(std::max(small_mask.cols, small_mask.rows) * 0.10f));
        pad |= 1;
        cv::Mat k_dilate = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(pad, pad));
        cv::dilate(small_mask, small_mask, k_dilate);

        cv::resize(small_mask, person_mask_dilated, frame.size(), 0, 0,
                   cv::INTER_NEAREST);
    }

    // -------------------------------------------------------------------
    // STAGE 3: Enhance + Binarize (Optimized)
    // -------------------------------------------------------------------
    // float enh_threshold = g_canvas_enhance_threshold.load();
    // cv::Mat enhanced = WhiteboardEnhance(process_frame, enh_threshold);

    // cv::Mat enhanced_gray;
    // cv::cvtColor(enhanced, enhanced_gray, cv::COLOR_BGR2GRAY);

    cv::Mat binary;
    cv::adaptiveThreshold(gray, binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, 51, 5);
    if (has_person_mask && !person_mask_dilated.empty()) {
        binary.setTo(0, person_mask_dilated);
    }

    // Crop top 15% and bottom 10%
    const int top_cut    = (int)(gray.rows * 0.15);
    const int bottom_cut = (int)(gray.rows * 0.90);
    if (top_cut > 0)
        binary(cv::Rect(0, 0, binary.cols, top_cut)).setTo(0);
    if (bottom_cut < binary.rows)
        binary(cv::Rect(0, bottom_cut, binary.cols, binary.rows - bottom_cut)).setTo(0);

    int stroke_pixel_count = cv::countNonZero(binary);

    cv::Mat stroke_paint_bgr(frame.size(), CV_8UC3, cv::Scalar(0, 0, 0));

    auto t_enhance = std::chrono::steady_clock::now();
    perf_stats_.stage3_enhance_ms += std::chrono::duration<double, std::milli>(t_enhance - t_motion).count();

    // -------------------------------------------------------------------
    // STAGE 4: Contour-shape matching
    // -------------------------------------------------------------------

    std::vector<ContourShape> frame_contours =
        ExtractContourShapes(binary, cv::Point2f(0, 0));

    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (frame_w_ == 0) { frame_w_ = frame.cols; frame_h_ = frame.rows; }

    int         best_group_idx = -1;
    int         best_votes = 0;
    cv::Point2f matched_pos(0, 0);
    std::string match_status;
    bool created_new_sc = false;
    int candidate_pair_count = 0;
    int accepted_pair_count = 0;

    if (canvas_contours_dirty_ && active_group_idx_ >= 0
        && active_group_idx_ < (int)groups_.size()) {
        RebuildCanvasContours(*groups_[active_group_idx_]);
    }

    if (active_group_idx_ >= 0 && active_group_idx_ < (int)groups_.size()
        && !canvas_contours_.empty() && !frame_contours.empty()) {

        candidate_pair_count = static_cast<int>(canvas_contours_.size() * frame_contours.size());

        std::unordered_map<int64_t, std::vector<cv::Point2f>> votes;

        for (const auto& cc : canvas_contours_) {
            for (const auto& fc : frame_contours) {
                double dist = cv::matchShapes(cc.contour, fc.contour,
                                              cv::CONTOURS_MATCH_I2, 0);
                if (dist > kMaxShapeDist) continue;
                accepted_pair_count++;

                float dx = cc.centroid.x - fc.centroid.x;
                float dy = cc.centroid.y - fc.centroid.y;

                int bin_x = (int)std::round(dx / (float)kVoteBinSize) * kVoteBinSize;
                int bin_y = (int)std::round(dy / (float)kVoteBinSize) * kVoteBinSize;
                int64_t key = (static_cast<int64_t>(static_cast<uint32_t>(bin_x)) << 32)
                            | static_cast<uint32_t>(bin_y);
                votes[key].push_back(cv::Point2f(dx, dy));
            }
        }

        // Find the bin with most votes
        int64_t best_key = 0;
        for (const auto& vote : votes) {
            const int vote_count = (int)vote.second.size();
            if (vote_count > best_votes) {
                best_votes = vote_count;
                best_key = vote.first;
            }
        }

        if (best_votes > 0) {
            // Collect votes: optionally merge with 8 neighbor bins
            std::vector<cv::Point2f> merged;
            if (kEnableNeighborBinMerge) {
                int center_bx = (int)(int32_t)(uint32_t)(best_key >> 32);
                int center_by = (int)(int32_t)(uint32_t)(best_key & 0xFFFFFFFF);
                for (int nx = -1; nx <= 1; nx++) {
                    for (int ny = -1; ny <= 1; ny++) {
                        int nbx = center_bx + nx * kVoteBinSize;
                        int nby = center_by + ny * kVoteBinSize;
                        int64_t nkey = (static_cast<int64_t>(static_cast<uint32_t>(nbx)) << 32)
                                     | static_cast<uint32_t>(nby);
                        auto it = votes.find(nkey);
                        if (it != votes.end()) {
                            merged.insert(merged.end(), it->second.begin(), it->second.end());
                        }
                    }
                }
                best_votes = (int)merged.size();
            } else {
                merged = votes[best_key];
            }

            // Median of merged pool (robust to outlier pairs)
            std::vector<float> dxs, dys;
            dxs.reserve(merged.size());
            dys.reserve(merged.size());
            for (const auto& pt : merged) {
                dxs.push_back(pt.x);
                dys.push_back(pt.y);
            }
            std::sort(dxs.begin(), dxs.end());
            std::sort(dys.begin(), dys.end());
            int mid = (int)merged.size() / 2;
            matched_pos = cv::Point2f(dxs[mid], dys[mid]);
        }

        if (best_votes >= kMinShapeVotes) {
            best_group_idx = active_group_idx_;
        }
    }

    auto t_match = std::chrono::steady_clock::now();
    double current_matching_ms = std::chrono::duration<double, std::milli>(t_match - t_enhance).count();
    perf_stats_.stage4_matching_ms += current_matching_ms;

    {
        std::ostringstream stream;
        stream << "[Performance] Contours found: " << frame_contours.size()
               << ", Canvas contours compared: " << canvas_contours_.size()
               << ", Candidate pairs: " << candidate_pair_count
               << ", Accepted pairs: " << accepted_pair_count;
        WhiteboardLog(stream.str());
    }
    {
        std::ostringstream stream;
        stream << "[Performance] Mapping (Contour Matching) took: "
               << current_matching_ms << " ms";
        WhiteboardLog(stream.str());
    }

    // -------------------------------------------------------------------
    // STAGE 5: Paint to chunks or create a new group
    // -------------------------------------------------------------------
    if (best_group_idx >= 0) {
        auto& group = *groups_[best_group_idx];

        // --- Phase correlation: sub-pixel refinement ---
        if (kEnablePhaseCorrelation) {
            cv::Mat canvas_gray_roi = GetCanvasGrayRegion(
                group, (int)std::round(matched_pos.x), (int)std::round(matched_pos.y),
                frame.cols, frame.rows);
            if (!canvas_gray_roi.empty() && canvas_gray_roi.size() == gray.size()) {
                cv::Mat frame_f, canvas_f;
                gray.convertTo(frame_f, CV_32F);
                canvas_gray_roi.convertTo(canvas_f, CV_32F);
                cv::Mat hann;
                cv::createHanningWindow(hann, frame_f.size(), CV_32F);
                cv::Point2d subpx = cv::phaseCorrelate(canvas_f, frame_f, hann);
                // Only apply small corrections (< half bin size)
                if (std::abs(subpx.x) < kVoteBinSize && std::abs(subpx.y) < kVoteBinSize) {
                    matched_pos.x += (float)subpx.x;
                    matched_pos.y += (float)subpx.y;
                }
            }
        }

        // --- Jump rejection ---
        if (kEnableJumpRejection && matched_frame_counter_ > 0) {
            float jump = (float)cv::norm(matched_pos - global_camera_pos_);
            if (jump > kMaxJumpPx) {
                WhiteboardLog("[WhiteboardCanvas] Jump rejected: "
                              + std::to_string(jump) + "px (max="
                              + std::to_string(kMaxJumpPx) + ")");
                matched_pos = global_camera_pos_;
            }
        }

        global_camera_pos_ = matched_pos;

        PaintStrokesToChunks(group, binary, stroke_paint_bgr, global_camera_pos_,
                     person_mask_dilated);
        PaintRawFrameToChunks(group, frame, global_camera_pos_, person_mask_dilated);

        active_group_idx_ = best_group_idx;
        has_content_ = true;

        match_status = "MATCHED GR" + std::to_string(best_group_idx)
                     + " votes=" + std::to_string(best_votes)
                     + " pos=(" + std::to_string((int)global_camera_pos_.x)
                     + ","     + std::to_string((int)global_camera_pos_.y) + ")"
                     + " chunks=" + std::to_string(group.chunks.size());

    } else if (groups_.empty() && stroke_pixel_count >= kMinStrokePixelsForNewSC) {
        CreateSubCanvas(
            frame,
            binary,
            stroke_paint_bgr,
            frame_contours,
            person_mask_dilated,
            person_mask_dilated);
        created_new_sc = true;
        match_status = "NEW GROUP (total=" + std::to_string(groups_.size()) + ")";

    } else {
        match_status = "no match (strokes=" + std::to_string(stroke_pixel_count)
                     + " contours=" + std::to_string(frame_contours.size())
                     + " votes=" + std::to_string(best_votes) + ")";
    }

    WhiteboardLog("[WhiteboardCanvas] " + match_status);

    auto t_paint = std::chrono::steady_clock::now();
    double current_painting_ms = std::chrono::duration<double, std::milli>(t_paint - t_match).count();
    perf_stats_.stage5_painting_ms += current_painting_ms;
    {
        std::ostringstream stream;
        stream << "[Performance] Drawing (Painting) took: " << current_painting_ms << " ms";
        WhiteboardLog(stream.str());
    }

    perf_stats_.total_frame_contours += static_cast<double>(frame_contours.size());
    perf_stats_.total_canvas_contours += static_cast<double>(canvas_contours_.size());
    perf_stats_.total_candidate_pairs += static_cast<double>(candidate_pair_count);
    perf_stats_.total_accepted_pairs += static_cast<double>(accepted_pair_count);
    perf_stats_.total_best_votes += static_cast<double>(best_votes);
    perf_stats_.frame_count++;
    if (best_group_idx >= 0) {
        perf_stats_.matched_frames++;
    }

    if ((t_paint - perf_stats_.last_print_time) >= std::chrono::seconds(1)) {
        const double frames = static_cast<double>(std::max(1, perf_stats_.frame_count));
        const double matched_frames = static_cast<double>(std::max(1, perf_stats_.matched_frames));

        {
            std::ostringstream stream;
            stream << "[WhiteboardMatchAvg] frames=" << perf_stats_.frame_count
                   << " matchedFrames=" << perf_stats_.matched_frames
                   << " avgBestVotesAll=" << (perf_stats_.total_best_votes / frames)
                   << " avgBestVotesMatched="
                   << (perf_stats_.matched_frames > 0
                       ? (perf_stats_.total_best_votes / matched_frames)
                       : 0.0)
                   << " avgFrameContours=" << (perf_stats_.total_frame_contours / frames)
                   << " avgCanvasContours=" << (perf_stats_.total_canvas_contours / frames)
                   << " avgCandidatePairs=" << (perf_stats_.total_candidate_pairs / frames)
                   << " avgAcceptedPairs=" << (perf_stats_.total_accepted_pairs / frames)
                   << " maxDist=" << kMaxShapeDist;
            WhiteboardLog(stream.str());
        }

        {
            std::ostringstream stream;
            stream << "[WhiteboardPerfAvg] frames=" << perf_stats_.frame_count
                   << " motionMs=" << (perf_stats_.stage1_motion_ms / frames)
                   << " enhanceMs=" << (perf_stats_.stage3_enhance_ms / frames)
                   << " matchMs=" << (perf_stats_.stage4_matching_ms / frames)
                   << " paintMs=" << (perf_stats_.stage5_painting_ms / frames)
                   << " totalMs="
                   << ((perf_stats_.stage1_motion_ms + perf_stats_.stage3_enhance_ms +
                        perf_stats_.stage4_matching_ms + perf_stats_.stage5_painting_ms) /
                       frames);
            WhiteboardLog(stream.str());
        }

        perf_stats_.stage1_motion_ms = 0;
        perf_stats_.stage3_enhance_ms = 0;
        perf_stats_.stage4_matching_ms = 0;
        perf_stats_.stage5_painting_ms = 0;
        perf_stats_.total_frame_contours = 0;
        perf_stats_.total_canvas_contours = 0;
        perf_stats_.total_candidate_pairs = 0;
        perf_stats_.total_accepted_pairs = 0;
        perf_stats_.total_best_votes = 0;
        perf_stats_.frame_count = 0;
        perf_stats_.matched_frames = 0;
        perf_stats_.last_print_time = t_paint;
    }

    // -------------------------------------------------------------------
    // STAGE 6: Debug visualization
    // -------------------------------------------------------------------
    if (dbg) {
        // Only do expensive debug ops inside this block
        cv::Mat dbg_frame = frame.clone();
        if (has_person_mask) {
             // Optional: visual whitewash ONLY for debug view
             if (!person_mask_dilated.empty()) {
                 dbg_frame.setTo(cv::Scalar(255, 255, 255), person_mask_dilated);
             }
        }

        cv::Mat enhanced_bgr_dbg = stroke_paint_bgr.clone();

        PipelineDebugState ds;
        ds.frame              = dbg_frame;
        ds.gray_clean         = gray;
        ds.enhanced_bgr       = enhanced_bgr_dbg;
        ds.binary             = binary;
        ds.person_mask        = person_mask_dilated.empty()
            ? person_mask
            : person_mask_dilated;
        ds.motion_fraction    = motion_fraction;
        ds.stroke_pixel_count = stroke_pixel_count;
        ds.match_status       = match_status;
        ds.created_new_sc     = created_new_sc;
        RenderDebugGrid(ds);
    }
}


// ============================================================================
//  SECTION 7: Contour Shape Helpers
// ============================================================================

// Extracts contours from `binary` (CV_8U), offsets their centroids by
// `roi_offset` so they are in full-frame coordinates, computes Hu Moments.
std::vector<WhiteboardCanvas::ContourShape>
WhiteboardCanvas::ExtractContourShapes(const cv::Mat& binary,
                                        cv::Point2f roi_offset) const {
    std::vector<ContourShape> result;
    if (binary.empty()) return result;

    std::vector<std::vector<cv::Point>> raw;
    cv::findContours(binary, raw, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& c : raw) {
        const double area = cv::contourArea(c);
        if (area < kMinContourArea) continue;

        // Centroid via moments
        cv::Moments m = cv::moments(c);
        if (std::abs(m.m00) < 1e-6) continue;
        cv::Point2f centroid((float)(m.m10 / m.m00) + roi_offset.x,
                             (float)(m.m01 / m.m00) + roi_offset.y);

        // Hu Moments
        double hu[7];
        cv::HuMoments(m, hu);

        ContourShape shape;
        shape.contour  = c;
        shape.centroid = centroid;
        shape.area = area;
        std::copy(hu, hu + 7, shape.hu);

        result.push_back(std::move(shape));
    }

    return result;
}

// Rebuilds canvas_contours_ from the current stroke render cache of `group`.
// Contour centroids are stored in analysis-scale canvas coordinates.
void WhiteboardCanvas::RebuildCanvasContours(WhiteboardGroup& group) {
    canvas_contours_.clear();
    canvas_contours_dirty_ = false;

    if (group.stroke_cache_dirty) {
        RebuildStrokeRenderCache(group);
        group.stroke_cache_dirty = false;
    }
    if (group.stroke_render_cache.empty()) return;

    // Binarize the canvas rendering at full resolution (matching frame contours).
    // Stroke cache is inverted (white strokes on black), so use THRESH_BINARY.
    cv::Mat gray, canvas_bin;
    cv::cvtColor(group.stroke_render_cache, gray, cv::COLOR_BGR2GRAY);
    cv::threshold(gray, canvas_bin, 50, 255, cv::THRESH_BINARY);

    cv::Point2f offset((float)group.stroke_min_px_x,
                       (float)group.stroke_min_px_y);
    canvas_contours_ = ExtractContourShapes(canvas_bin, offset);
    {
        std::ostringstream stream;
        stream << "[Canvas] Rebuilt contours: " << canvas_contours_.size();
        WhiteboardLog(stream.str());
    }
}

// Reads a frame-sized gray region from the raw canvas chunks at the given
// global pixel position.  Returns empty Mat if the region is not fully covered.
cv::Mat WhiteboardCanvas::GetCanvasGrayRegion(WhiteboardGroup& group,
                                               int global_x, int global_y,
                                               int width, int height) {
    int gx0 = global_x, gy0 = global_y;
    int gx1 = gx0 + width, gy1 = gy0 + height;

    int sc_x = (int)std::floor((float)gx0 / kChunkSize);
    int sc_y = (int)std::floor((float)gy0 / kChunkSize);
    int ec_x = (int)std::floor((float)(gx1 - 1) / kChunkSize);
    int ec_y = (int)std::floor((float)(gy1 - 1) / kChunkSize);

    // Check all required chunks exist
    for (int cy = sc_y; cy <= ec_y; cy++) {
        for (int cx = sc_x; cx <= ec_x; cx++) {
            if (group.chunks.find(GetChunkHash(cx, cy)) == group.chunks.end()) {
                return cv::Mat();
            }
        }
    }

    cv::Mat region(height, width, CV_8UC3, cv::Scalar(255, 255, 255));
    for (int cy = sc_y; cy <= ec_y; cy++) {
        for (int cx = sc_x; cx <= ec_x; cx++) {
            int cpx = cx * kChunkSize, cpy = cy * kChunkSize;
            int ix0 = std::max(cpx, gx0), iy0 = std::max(cpy, gy0);
            int ix1 = std::min(cpx + kChunkSize, gx1);
            int iy1 = std::min(cpy + kChunkSize, gy1);
            if (ix0 >= ix1 || iy0 >= iy1) continue;

            cv::Rect chunk_roi(ix0 - cpx, iy0 - cpy, ix1 - ix0, iy1 - iy0);
            cv::Rect out_roi(ix0 - gx0, iy0 - gy0, ix1 - ix0, iy1 - iy0);
            group.chunks[GetChunkHash(cx, cy)]->raw_canvas(chunk_roi).copyTo(region(out_roi));
        }
    }

    cv::Mat gray_region;
    cv::cvtColor(region, gray_region, cv::COLOR_BGR2GRAY);
    return gray_region;
}

// ============================================================================
//  SECTION 8: Chunk Grid Management
// ============================================================================

uint64_t WhiteboardCanvas::GetChunkHash(int grid_x, int grid_y) const {
    uint32_t ux = (uint32_t)grid_x;
    uint32_t uy = (uint32_t)grid_y;
    return ((uint64_t)ux << 32) | uy;
}

void WhiteboardCanvas::EnsureChunkAllocated(WhiteboardGroup& group, int grid_x, int grid_y) {
    uint64_t hash = GetChunkHash(grid_x, grid_y);
    if (group.chunks.find(hash) == group.chunks.end()) {
        auto chunk = std::make_unique<Chunk>();
        chunk->stroke_canvas = cv::Mat(kChunkSize, kChunkSize, CV_8UC3, cv::Scalar(255, 255, 255));
        chunk->absence_counter = cv::Mat(kChunkSize, kChunkSize, CV_8U, cv::Scalar(0));
        chunk->raw_canvas = cv::Mat(kChunkSize, kChunkSize, CV_8UC3, cv::Scalar(255, 255, 255));
        chunk->grid_x = grid_x;
        chunk->grid_y = grid_y;
        group.chunks[hash] = std::move(chunk);

        group.stroke_cache_dirty = true;
        group.raw_cache_dirty = true;
    }
}

// ============================================================================
//  SECTION 9: Rendering to Chunks
// ============================================================================

void WhiteboardCanvas::PaintStrokesToChunks(WhiteboardGroup& group, const cv::Mat& binary,
                                             const cv::Mat& enhanced_bgr,
                                             cv::Point2f camera_pos,
                                             const cv::Mat& no_update_mask) {
    // Performance optimization: work only within the bounding rect of actual strokes.
    cv::Rect bbox = cv::boundingRect(binary);
    if (bbox.empty()) return;

    // Global pixel coordinates of the stroke bbox on the infinite canvas.
    int global_start_x = (int)std::round(camera_pos.x) + bbox.x;
    int global_start_y = (int)std::round(camera_pos.y) + bbox.y;
    int global_end_x   = global_start_x + bbox.width;
    int global_end_y   = global_start_y + bbox.height;

    // Keep the canvas height fixed to the first frame's height.
    // We allow horizontal growth only, so vertically out-of-range pixels are clipped.
    cv::Rect paint_bbox = bbox;
    int clip_top = std::max(group.stroke_min_px_y - global_start_y, 0);
    int clip_bottom = std::max(global_end_y - group.stroke_max_px_y, 0);
    paint_bbox.y += clip_top;
    paint_bbox.height -= (clip_top + clip_bottom);
    global_start_y += clip_top;
    global_end_y -= clip_bottom;

    if (paint_bbox.height <= 0 || global_start_y >= global_end_y) return;

    if (global_start_x < group.stroke_min_px_x) group.stroke_min_px_x = global_start_x;
    if (global_end_x > group.stroke_max_px_x) group.stroke_max_px_x = global_end_x;

    // Chunk range that overlaps the bbox.
    int start_chunk_x = (int)std::floor((float)global_start_x / kChunkSize);
    int start_chunk_y = (int)std::floor((float)global_start_y / kChunkSize);
    int end_chunk_x   = (int)std::floor((float)global_end_x   / kChunkSize);
    int end_chunk_y   = (int)std::floor((float)global_end_y   / kChunkSize);

    // Ensure every required chunk exists before we start reading from them.
    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++)
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++)
            EnsureChunkAllocated(group, cx, cy);

    // -----------------------------------------------------------------------
    // PASS 1 — Stitch chunk data into footprint Mats
    // -----------------------------------------------------------------------

    cv::Mat footprint(paint_bbox.height, paint_bbox.width, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::Mat absence_foot(paint_bbox.height, paint_bbox.width, CV_8U, cv::Scalar(0));

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            int chunk_px_x = cx * kChunkSize;
            int chunk_px_y = cy * kChunkSize;

            int ix0 = std::max(chunk_px_x, global_start_x);
            int iy0 = std::max(chunk_px_y, global_start_y);
            int ix1 = std::min(chunk_px_x + kChunkSize, global_end_x);
            int iy1 = std::min(chunk_px_y + kChunkSize, global_end_y);
            if (ix0 >= ix1 || iy0 >= iy1) continue;

            cv::Rect chunk_roi(ix0 - chunk_px_x, iy0 - chunk_px_y, ix1 - ix0, iy1 - iy0);
            cv::Rect foot_roi (ix0 - global_start_x, iy0 - global_start_y, ix1 - ix0, iy1 - iy0);

            uint64_t hash = GetChunkHash(cx, cy);
            auto& chunk = group.chunks[hash];
            chunk->stroke_canvas(chunk_roi).copyTo(footprint(foot_roi));
            chunk->absence_counter(chunk_roi).copyTo(absence_foot(foot_roi));
        }
    }

    cv::Mat new_strokes = binary(paint_bbox);
    cv::Mat colors_bbox = enhanced_bgr(paint_bbox);
    cv::Mat no_update_bbox = (!no_update_mask.empty() && no_update_mask.size() == binary.size())
        ? no_update_mask(paint_bbox)
        : cv::Mat(paint_bbox.height, paint_bbox.width, CV_8U, cv::Scalar(0));

    // Existing ink mask
    cv::Mat foot_gray, existing;
    cv::cvtColor(footprint, foot_gray, cv::COLOR_BGR2GRAY);
    cv::threshold(foot_gray, existing, 200, 255, cv::THRESH_BINARY_INV);

    // -----------------------------------------------------------------------
    // LAYER 1: Proximity Suppression
    // -----------------------------------------------------------------------
    cv::Mat prox_allow_mask(footprint.size(), CV_8U, cv::Scalar(255));

    if (kEnableProximitySuppression) {
        cv::Mat canvas_stroke_zone;
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
            cv::Size(2 * kProximityRadius + 1, 2 * kProximityRadius + 1));
        cv::dilate(existing, canvas_stroke_zone, kernel);
        cv::bitwise_not(canvas_stroke_zone, prox_allow_mask);
    }

    cv::Mat update_allowed_mask;
    cv::bitwise_not(no_update_bbox, update_allowed_mask);

    // -----------------------------------------------------------------------
    // LAYERS 2, 3, 4: Grid-based replace, ghost, and absence erasure
    // -----------------------------------------------------------------------
    cv::Mat cell_replace_mask(footprint.size(), CV_8U, cv::Scalar(0));
    cv::Mat cell_erase_mask(footprint.size(), CV_8U, cv::Scalar(0));
    cv::Mat ghost_block(footprint.size(), CV_8U, cv::Scalar(0));

    if (kEnableGridReplace || kEnableGhostBlock || kEnableAbsenceErasure) {
        for (int gy = 0; gy < footprint.rows; gy += kGridCellSize) {
            for (int gx = 0; gx < footprint.cols; gx += kGridCellSize) {
                int cw = std::min(kGridCellSize, footprint.cols - gx);
                int ch = std::min(kGridCellSize, footprint.rows - gy);
                cv::Rect cell(gx, gy, cw, ch);

                if (cv::countNonZero(no_update_bbox(cell)) > 0) {
                    continue;
                }

                int new_count   = cv::countNonZero(new_strokes(cell));
                int exist_count = cv::countNonZero(existing(cell));

                // Layer 4: Absence Erasure
                if (kEnableAbsenceErasure
                    && exist_count >= kMinCellStrokePixels && new_count < kAbsenceEraseThr) {
                    absence_foot(cell) += 1;
                    cv::Mat absence_roi = absence_foot(cell);
                    cv::Mat erase_flag = (absence_roi >= kAbsenceEraseFrames);
                    if (cv::countNonZero(erase_flag) > 0) {
                        cell_erase_mask(cell).setTo(255);
                        absence_roi.setTo(0);
                    }
                    continue;
                } else {
                    absence_foot(cell).setTo(0);
                }

                if (exist_count < kMinCellStrokePixels || new_count == 0) continue;

                cv::Mat inter;
                cv::bitwise_and(new_strokes(cell), existing(cell), inter);
                int inter_count = cv::countNonZero(inter);
                int union_count = new_count + exist_count - inter_count;

                float iou = (union_count > 0) ? (float)inter_count / (float)union_count : 0.0f;
                float overlap = (float)inter_count / (float)new_count;

                // Layer 2: Grid Replace
                if (kEnableGridReplace && iou < kCellReplaceIoU) {
                    cell_replace_mask(cell).setTo(255);
                    prox_allow_mask(cell).setTo(255);
                }
                // Layer 3: Ghost Block
                else if (kEnableGhostBlock && overlap > kCellGhostOverlap) {
                    ghost_block(cell).setTo(255);
                }
            }
        }
    }

    // Apply ghost block
    if (kEnableGhostBlock) {
        cv::Mat ghost_allow;
        cv::bitwise_not(ghost_block, ghost_allow);
        cv::bitwise_and(prox_allow_mask, ghost_allow, prox_allow_mask);
    }
    cv::bitwise_and(prox_allow_mask, update_allowed_mask, prox_allow_mask);

    // new_only: allowed strokes
    cv::Mat new_only;
    cv::bitwise_and(new_strokes, prox_allow_mask, new_only);

    // -----------------------------------------------------------------------
    // PASS 2 — Scatter modifications back into the Chunks
    // -----------------------------------------------------------------------
    bool has_writes = (cv::countNonZero(new_only) > 0 || 
                       cv::countNonZero(cell_replace_mask) > 0 || 
                       cv::countNonZero(cell_erase_mask) > 0);
                       
    // Note: absence memory map always updates because we track visibility
    has_writes = true;

    if (!has_writes) return;

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            int chunk_px_x = cx * kChunkSize;
            int chunk_px_y = cy * kChunkSize;

            int ix0 = std::max(chunk_px_x, global_start_x);
            int iy0 = std::max(chunk_px_y, global_start_y);
            int ix1 = std::min(chunk_px_x + kChunkSize, global_end_x);
            int iy1 = std::min(chunk_px_y + kChunkSize, global_end_y);
            if (ix0 >= ix1 || iy0 >= iy1) continue;

            cv::Rect chunk_roi(ix0 - chunk_px_x, iy0 - chunk_px_y, ix1 - ix0, iy1 - iy0);
            cv::Rect foot_roi (ix0 - global_start_x, iy0 - global_start_y, ix1 - ix0, iy1 - iy0);

            uint64_t hash = GetChunkHash(cx, cy);
            auto& chunk = group.chunks[hash];
            bool chunk_dirty = false;

            // Update absence counter
            absence_foot(foot_roi).copyTo(chunk->absence_counter(chunk_roi));

            // Layer 4 / 2: Erase old ink
            cv::Mat erase_roi = cell_erase_mask(foot_roi) | cell_replace_mask(foot_roi);
            if (cv::countNonZero(erase_roi) > 0) {
                chunk->stroke_canvas(chunk_roi).setTo(cv::Scalar(255, 255, 255), erase_roi);
                chunk_dirty = true;
            }

            // Paint new strokes
            cv::Mat sub_mask = new_only(foot_roi);
            if (cv::countNonZero(sub_mask) > 0) {
                colors_bbox(foot_roi).copyTo(chunk->stroke_canvas(chunk_roi), sub_mask);
                chunk_dirty = true;
            }

            if (chunk_dirty) {
                group.stroke_cache_dirty = true;
                canvas_contours_dirty_ = true;
            }
        }
    }
}

void WhiteboardCanvas::PaintRawFrameToChunks(WhiteboardGroup& group,
                                             const cv::Mat& frame_bgr,
                                             cv::Point2f camera_pos,
                                             const cv::Mat& no_update_mask) {
    if (frame_bgr.empty()) return;

    // --- Blur rejection: skip blurry frames ---
    if (kEnableBlurRejection) {
        cv::Mat gray_small;
        cv::resize(frame_bgr, gray_small, cv::Size(), 0.25, 0.25, cv::INTER_AREA);
        cv::Mat gray_ch;
        cv::cvtColor(gray_small, gray_ch, cv::COLOR_BGR2GRAY);
        cv::Mat lap;
        cv::Laplacian(gray_ch, lap, CV_64F);
        cv::Scalar mu, sigma;
        cv::meanStdDev(lap, mu, sigma);
        double variance = sigma.val[0] * sigma.val[0];
        if (variance < kBlurThreshold) {
            WhiteboardLog("[RawPaint] Blurry frame rejected (var="
                          + std::to_string(variance) + " thr="
                          + std::to_string(kBlurThreshold) + ")");
            return;
        }
    }

    // --- Build a paint region with edge margin ---
    const int margin = kRawEdgeMargin;
    const int fw = frame_bgr.cols, fh = frame_bgr.rows;
    // The sub-region of the frame that will actually be painted
    cv::Rect inner(margin, margin,
                   std::max(1, fw - 2 * margin),
                   std::max(1, fh - 2 * margin));

    int global_start_x = (int)std::round(camera_pos.x) + inner.x;
    int global_start_y = (int)std::round(camera_pos.y) + inner.y;
    int global_end_x = global_start_x + inner.width;
    int global_end_y = global_start_y + inner.height;

    if (global_start_x < group.raw_min_px_x) group.raw_min_px_x = global_start_x;
    if (global_start_y < group.raw_min_px_y) group.raw_min_px_y = global_start_y;
    if (global_end_x > group.raw_max_px_x) group.raw_max_px_x = global_end_x;
    if (global_end_y > group.raw_max_px_y) group.raw_max_px_y = global_end_y;

    // --- Build feather alpha mask (0-255) for the inner region ---
    cv::Mat feather_alpha;
    if (kEnableRawEdgeFeather && kRawFeatherWidth > 0) {
        feather_alpha = cv::Mat(inner.height, inner.width, CV_8U, cv::Scalar(255));
        const int fw_px = std::min(kRawFeatherWidth, std::min(inner.width, inner.height) / 2);
        for (int i = 0; i < fw_px; i++) {
            uchar val = (uchar)(255 * i / fw_px);
            // Top row
            feather_alpha.row(i).setTo(std::min(val, feather_alpha.at<uchar>(i, fw_px)));
            // Bottom row
            feather_alpha.row(inner.height - 1 - i).setTo(
                std::min(val, feather_alpha.at<uchar>(inner.height - 1 - i, fw_px)));
            // Left column
            for (int y = 0; y < inner.height; y++)
                feather_alpha.at<uchar>(y, i) = std::min(feather_alpha.at<uchar>(y, i), val);
            // Right column
            for (int y = 0; y < inner.height; y++)
                feather_alpha.at<uchar>(y, inner.width - 1 - i) =
                    std::min(feather_alpha.at<uchar>(y, inner.width - 1 - i), val);
        }
    }

    cv::Mat inner_frame = frame_bgr(inner);
    cv::Mat inner_no_update;
    if (!no_update_mask.empty() && no_update_mask.size() == frame_bgr.size()) {
        inner_no_update = no_update_mask(inner);
    }

    int start_chunk_x = (int)std::floor((float)global_start_x / kChunkSize);
    int start_chunk_y = (int)std::floor((float)global_start_y / kChunkSize);
    int end_chunk_x   = (int)std::floor((float)global_end_x   / kChunkSize);
    int end_chunk_y   = (int)std::floor((float)global_end_y   / kChunkSize);

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            EnsureChunkAllocated(group, cx, cy);

            int chunk_px_x = cx * kChunkSize;
            int chunk_px_y = cy * kChunkSize;

            int ix0 = std::max(chunk_px_x, global_start_x);
            int iy0 = std::max(chunk_px_y, global_start_y);
            int ix1 = std::min(chunk_px_x + kChunkSize, global_end_x);
            int iy1 = std::min(chunk_px_y + kChunkSize, global_end_y);
            if (ix0 >= ix1 || iy0 >= iy1) continue;

            cv::Rect chunk_roi(ix0 - chunk_px_x, iy0 - chunk_px_y, ix1 - ix0, iy1 - iy0);
            cv::Rect src_roi(ix0 - global_start_x, iy0 - global_start_y, ix1 - ix0, iy1 - iy0);

            // Valid mask (exclude person)
            cv::Mat valid_mask;
            if (!inner_no_update.empty()) {
                cv::bitwise_not(inner_no_update(src_roi), valid_mask);
            } else {
                valid_mask = cv::Mat(src_roi.height, src_roi.width, CV_8U, cv::Scalar(255));
            }
            if (cv::countNonZero(valid_mask) == 0) continue;

            uint64_t hash = GetChunkHash(cx, cy);
            auto& chunk = group.chunks[hash];

            if (kEnableRawEdgeFeather && !feather_alpha.empty()) {
                // Alpha blend: dst = (src * alpha + dst * (255 - alpha)) / 255
                cv::Mat src_patch = inner_frame(src_roi);
                cv::Mat dst_patch = chunk->raw_canvas(chunk_roi);
                cv::Mat alpha_patch = feather_alpha(src_roi);

                cv::Mat src_f, dst_f, alpha_f;
                src_patch.convertTo(src_f, CV_32FC3);
                dst_patch.convertTo(dst_f, CV_32FC3);
                alpha_patch.convertTo(alpha_f, CV_32F, 1.0 / 255.0);

                // Expand alpha to 3 channels
                cv::Mat alpha_3ch;
                cv::merge(std::vector<cv::Mat>{alpha_f, alpha_f, alpha_f}, alpha_3ch);

                cv::Mat blended_f = src_f.mul(alpha_3ch) + dst_f.mul(cv::Scalar(1, 1, 1) - alpha_3ch);
                cv::Mat blended;
                blended_f.convertTo(blended, CV_8UC3);
                blended.copyTo(chunk->raw_canvas(chunk_roi), valid_mask);
            } else {
                inner_frame(src_roi).copyTo(chunk->raw_canvas(chunk_roi), valid_mask);
            }

            group.raw_cache_dirty = true;
        }
    }
}

void WhiteboardCanvas::RebuildStrokeRenderCache(WhiteboardGroup& group) {
    if (group.chunks.empty()) {
        group.stroke_render_cache = cv::Mat();
        return;
    }

    int width = std::max(1, group.stroke_max_px_x - group.stroke_min_px_x);
    int height = std::max(1, group.stroke_max_px_y - group.stroke_min_px_y);
    group.stroke_render_cache = cv::Mat(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    for (const auto& pair : group.chunks) {
        const auto& chunk = pair.second;
        int chunk_left = chunk->grid_x * kChunkSize;
        int chunk_top = chunk->grid_y * kChunkSize;
        int chunk_right = chunk_left + kChunkSize;
        int chunk_bottom = chunk_top + kChunkSize;

        int copy_left = std::max(chunk_left, group.stroke_min_px_x);
        int copy_top = std::max(chunk_top, group.stroke_min_px_y);
        int copy_right = std::min(chunk_right, group.stroke_max_px_x);
        int copy_bottom = std::min(chunk_bottom, group.stroke_max_px_y);
        if (copy_left >= copy_right || copy_top >= copy_bottom) continue;

        cv::Rect src_roi(copy_left - chunk_left, copy_top - chunk_top,
                         copy_right - copy_left, copy_bottom - copy_top);
        cv::Rect dst_roi(copy_left - group.stroke_min_px_x, copy_top - group.stroke_min_px_y,
                         copy_right - copy_left, copy_bottom - copy_top);

        chunk->stroke_canvas(src_roi).copyTo(group.stroke_render_cache(dst_roi));
    }

    // Invert: black background with white strokes
    cv::bitwise_not(group.stroke_render_cache, group.stroke_render_cache);
}

void WhiteboardCanvas::RebuildRawRenderCache(WhiteboardGroup& group) {
    if (group.chunks.empty()) {
        group.raw_render_cache = cv::Mat();
        return;
    }

    int width = std::max(1, group.raw_max_px_x - group.raw_min_px_x);
    int height = std::max(1, group.raw_max_px_y - group.raw_min_px_y);
    group.raw_render_cache = cv::Mat(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    for (const auto& pair : group.chunks) {
        const auto& chunk = pair.second;
        int chunk_left = chunk->grid_x * kChunkSize;
        int chunk_top = chunk->grid_y * kChunkSize;
        int chunk_right = chunk_left + kChunkSize;
        int chunk_bottom = chunk_top + kChunkSize;

        int copy_left = std::max(chunk_left, group.raw_min_px_x);
        int copy_top = std::max(chunk_top, group.raw_min_px_y);
        int copy_right = std::min(chunk_right, group.raw_max_px_x);
        int copy_bottom = std::min(chunk_bottom, group.raw_max_px_y);
        if (copy_left >= copy_right || copy_top >= copy_bottom) continue;

        cv::Rect src_roi(copy_left - chunk_left, copy_top - chunk_top,
                         copy_right - copy_left, copy_bottom - copy_top);
        cv::Rect dst_roi(copy_left - group.raw_min_px_x, copy_top - group.raw_min_px_y,
                         copy_right - copy_left, copy_bottom - copy_top);

        chunk->raw_canvas(src_roi).copyTo(group.raw_render_cache(dst_roi));
    }
}

// ============================================================================
//  SECTION 12: Create new Whiteboard Group
// ============================================================================

void WhiteboardCanvas::CreateSubCanvas(const cv::Mat& frame_bgr,
                                        const cv::Mat& binary,
                                        const cv::Mat& enhanced_bgr,
                                        const std::vector<ContourShape>& seed_contours,
                                        const cv::Mat& stroke_no_update_mask,
                                        const cv::Mat& raw_no_update_mask) {
    auto group = std::make_unique<WhiteboardGroup>();
    group->debug_id = next_debug_id_++;
    const int canvas_width = std::max(1, frame_bgr.cols);
    const int canvas_height = std::max(1, frame_bgr.rows);

    group->stroke_min_px_x = 0;
    group->stroke_min_px_y = 0;
    group->stroke_max_px_x = canvas_width;
    group->stroke_max_px_y = canvas_height;
    group->raw_min_px_x = 0;
    group->raw_min_px_y = 0;
    group->raw_max_px_x = std::max(frame_w_, frame_bgr.cols);
    group->raw_max_px_y = std::max(frame_h_, frame_bgr.rows);

    // Seed at (0,0) global position
    global_camera_pos_ = cv::Point2f(0, 0);


    PaintStrokesToChunks(*group, binary, enhanced_bgr, global_camera_pos_,
                         stroke_no_update_mask);
    PaintRawFrameToChunks(*group, frame_bgr, global_camera_pos_, raw_no_update_mask);

    int idx = (int)groups_.size();
    groups_.push_back(std::move(group));
    active_group_idx_ = idx;
    
    // Automatically switch to viewing the canvas if user requested the canvas view
    // before any canvases were created
    if (canvas_view_mode_.load() && view_group_idx_ == -1) {
        view_group_idx_ = idx;
    }
    
    has_content_ = true;

    // Seed the canvas contour cache with the first frame's contours.
    // At (0,0), frame coords == canvas coords.
    canvas_contours_ = seed_contours;
    canvas_contours_dirty_ = false;

    {
        std::ostringstream stream;
        stream << "[WhiteboardCanvas] Created New Group " << idx
               << " with " << canvas_contours_.size() << " seed contours";
        WhiteboardLog(stream.str());
    }
}



// ============================================================================
//  SECTION 14: Debug visualization
// ============================================================================

void WhiteboardCanvas::RenderDebugGrid(const PipelineDebugState& state) {
    const int TW = 480, TH = 270;
    cv::Mat combined(TH * 2, TW * 3, CV_8UC3, cv::Scalar(20, 20, 20));

    auto PlaceTile = [&](cv::Mat src, int col, int row, int span = 1) {
        if (src.empty()) return;
        if (src.channels() == 1) cv::cvtColor(src, src, cv::COLOR_GRAY2BGR);
        cv::Mat tile;
        cv::resize(src, tile, cv::Size(TW * span, TH), 0, 0, cv::INTER_LINEAR);
        tile.copyTo(combined(cv::Rect(col * TW, row * TH, TW * span, TH)));
    };

    // Tile [0,0]: Input frame + status
    {
        cv::Mat w1 = state.frame.clone();
        
        bool has_pm = !state.person_mask.empty() && cv::countNonZero(state.person_mask) > 0;
        if (has_pm) {
            cv::Mat pm_colored(w1.size(), CV_8UC3, cv::Scalar(0, 255, 0)); // Green tint
            cv::Mat blended;
            cv::addWeighted(w1, 0.7, pm_colored, 0.3, 0, blended);
            blended.copyTo(w1, state.person_mask);
        }
        
        cv::Scalar status_color = state.created_new_sc
            ? cv::Scalar(0, 165, 255)
            : state.match_status.find("MATCHED") != std::string::npos
                ? cv::Scalar(0, 255, 0)
                : cv::Scalar(0, 0, 255);
        DebugText(w1, state.match_status, cv::Point(10, 30), 0.5, status_color);
        DebugText(w1, "motion=" + std::to_string((int)(state.motion_fraction * 100)) + "%"
                      " | Groups=" + std::to_string(groups_.size())
                      + (has_pm ? " | PM ON" : ""),
                  cv::Point(10, 60), 0.6, cv::Scalar(200, 200, 200));
        PlaceTile(w1, 0, 0);
    }

    // Tile [1,0]: Enhanced output
    {
        cv::Mat w2 = state.enhanced_bgr.empty()
                     ? (cv::Mat)(cv::Mat(TH, TW, CV_8UC3, cv::Scalar(40,40,40)))
                     : state.enhanced_bgr.clone();
        DebugText(w2, "Enhanced", cv::Point(10, 30));
        PlaceTile(w2, 1, 0);
    }

    // Tile [2,0]: Binary strokes
    {
        cv::Mat w3;
        cv::cvtColor(state.binary, w3, cv::COLOR_GRAY2BGR);
        DebugText(w3, "Binary: " + std::to_string(state.stroke_pixel_count) + " px",
                  cv::Point(10, 30));
        PlaceTile(w3, 2, 0);
    }

    // Tile [0,1]: Offset info
    {
        cv::Mat w4(frame_h_ > 0 ? frame_h_ : 720, frame_w_ > 0 ? frame_w_ : 1280,
                   CV_8UC3, cv::Scalar(40, 40, 40));
        if (active_group_idx_ >= 0 && active_group_idx_ < (int)groups_.size()) {
            auto& group = *groups_[active_group_idx_];
            const CanvasRenderMode render_mode = GetRenderMode();
            int min_px_x = 0;
            int min_px_y = 0;
            int max_px_x = frame_w_ > 0 ? frame_w_ : 1280;
            int max_px_y = frame_h_ > 0 ? frame_h_ : 720;
            GetRenderBoundsForMode(group, render_mode, min_px_x, min_px_y, max_px_x, max_px_y);
            DebugText(w4, "Group: " + std::to_string(active_group_idx_)
                          + " pos=(" + std::to_string((int)global_camera_pos_.x)
                          + "," + std::to_string((int)global_camera_pos_.y) + ")",
                      cv::Point(10, 30), 0.7, cv::Scalar(0, 255, 255));
            DebugText(w4, "Mode: " + std::string(RenderModeName(render_mode))
                          + " | Chunks: " + std::to_string(group.chunks.size())
                          + " | bounds: " + std::to_string(max_px_x - min_px_x) + "x"
                          + std::to_string(max_px_y - min_px_y),
                      cv::Point(10, 65), 0.6, cv::Scalar(200, 200, 200));
        }
        PlaceTile(w4, 0, 1);
    }

    // Tile [1,1]-[2,1]: Active canvas (spans 2 tiles)
    if (active_group_idx_ >= 0 && active_group_idx_ < (int)groups_.size()) {
        auto& group = *groups_[active_group_idx_];
        const CanvasRenderMode render_mode = GetRenderMode();
        if (render_mode == CanvasRenderMode::kRaw) {
            if (group.raw_cache_dirty) { RebuildRawRenderCache(group); group.raw_cache_dirty = false; }
        } else {
            if (group.stroke_cache_dirty) { RebuildStrokeRenderCache(group); group.stroke_cache_dirty = false; }
        }
        cv::Mat w5 = GetRenderCacheForMode(group, render_mode).clone();
        if(!w5.empty()) {
            DebugText(w5, "World GR" + std::to_string(active_group_idx_)
                          + " [" + RenderModeName(render_mode) + "]"
                          + " " + std::to_string(w5.cols) + "x" + std::to_string(w5.rows),
                      cv::Point(10, 30), 0.8, cv::Scalar(255, 0, 0));
            DebugText(w5, state.match_status, cv::Point(10, 62), 0.5,
                      state.match_status.find("MATCHED") != std::string::npos
                          ? cv::Scalar(0, 200, 0) : cv::Scalar(0, 0, 255));
            PlaceTile(w5, 1, 1, /*span=*/2);
        }
    }

    cv::imshow("WB Debug", combined);
    cv::waitKey(1);
}

// ============================================================================
//  SECTION 14: FFI exports
// ============================================================================

void SetPanoramaEnabled(bool enabled) {
    if (enabled) {
        if (!g_whiteboard_canvas) {
            g_whiteboard_canvas = new WhiteboardCanvas();
        } else {
            g_whiteboard_canvas->SetCanvasViewMode(false);
            g_whiteboard_canvas->Reset();
        }
        g_whiteboard_canvas->SyncRuntimeSettings();
        g_whiteboard_enabled.store(true);
    } else {
        g_whiteboard_enabled.store(false);
        if (g_whiteboard_canvas) {
            g_whiteboard_canvas->SetCanvasViewMode(false);
            g_whiteboard_canvas->Reset();
        }
    }
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
    WhiteboardLog(std::string("[WhiteboardCanvas] ") + (enabled ? "Enabled" : "Disabled"));
}

void ResetPanorama() {
    if (g_whiteboard_canvas) g_whiteboard_canvas->Reset();
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
}

void SetPanoramaViewport(float panX, float panY, float zoom) {
    g_canvas_pan_x.store(panX);
    g_canvas_pan_y.store(panY);
    g_canvas_zoom.store(zoom);
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
}

void GetPanoramaCanvasSize(int* width, int* height) {
    if (g_whiteboard_canvas) {
        cv::Size s = g_whiteboard_canvas->GetCanvasSize();
        if (width)  *width  = s.width;
        if (height) *height = s.height;
    } else {
        if (width)  *width  = 1920;
        if (height) *height = 1080;
    }
}

bool IsPanoramaEnabled()    { return g_whiteboard_enabled.load(); }

void SetCanvasViewMode(bool mode) {
    if (g_whiteboard_canvas) g_whiteboard_canvas->SetCanvasViewMode(mode);
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
}

bool IsCanvasViewMode() {
    return g_whiteboard_canvas && g_whiteboard_canvas->IsCanvasViewMode();
}

void SetCanvasRenderMode(int mode) {
    if (g_whiteboard_canvas) {
        g_whiteboard_canvas->SetRenderMode(
            mode == static_cast<int>(CanvasRenderMode::kRaw)
                ? CanvasRenderMode::kRaw
                : CanvasRenderMode::kStroke);
    }
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
}

int64_t GetCanvasTextureId() {
    return g_native_camera ? g_native_camera->GetTextureId() : -1;
}

bool GetCanvasOverviewRgba(uint8_t* buffer, int width, int height) {
    if (!g_whiteboard_canvas || !buffer || width <= 0 || height <= 0) {
        return false;
    }

    cv::Mat overview_bgr;
    if (!g_whiteboard_canvas->GetOverviewBlocking(cv::Size(width, height), overview_bgr)) {
        return false;
    }

    cv::Mat overview_rgba;
    cv::cvtColor(overview_bgr, overview_rgba, cv::COLOR_BGR2RGBA);
    cv::Mat buffer_view(height, width, CV_8UC4, buffer);
    overview_rgba.copyTo(buffer_view);
    return true;
}

bool GetCanvasViewportRgba(uint8_t* buffer,
                           int width,
                           int height,
                           float panX,
                           float panY,
                           float zoom) {
    if (!g_whiteboard_canvas || !buffer || width <= 0 || height <= 0) {
        return false;
    }

    cv::Mat viewport_bgr;
    if (!g_whiteboard_canvas->GetViewport(
            panX,
            panY,
            zoom,
            cv::Size(width, height),
            viewport_bgr)) {
        return false;
    }

    cv::Mat viewport_rgba;
    cv::cvtColor(viewport_bgr, viewport_rgba, cv::COLOR_BGR2RGBA);
    cv::Mat buffer_view(height, width, CV_8UC4, buffer);
    viewport_rgba.copyTo(buffer_view);
    return true;
}

void SetWhiteboardDebug(bool enabled) {
    g_whiteboard_debug.store(enabled);
    if (g_whiteboard_canvas) g_whiteboard_canvas->SyncRuntimeSettings();
    if (!enabled) cv::destroyAllWindows();
    WhiteboardLog(std::string("[WhiteboardCanvas] Debug windows ") + (enabled ? "ON" : "OFF"));
}

void SetCanvasEnhanceThreshold(float threshold) {
    g_canvas_enhance_threshold.store(threshold);
    if (g_whiteboard_canvas) g_whiteboard_canvas->SyncRuntimeSettings();
    {
        std::ostringstream stream;
        stream << "[WhiteboardCanvas] Enhance threshold=" << threshold;
        WhiteboardLog(stream.str());
    }
}

int GetSubCanvasCount() {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetSubCanvasCount();
}

int GetActiveSubCanvasIndex() {
    if (!g_whiteboard_canvas) return -1;
    return g_whiteboard_canvas->GetActiveSubCanvasIndex();
}

void SetActiveSubCanvas(int idx) {
    if (g_whiteboard_canvas) g_whiteboard_canvas->SetActiveSubCanvas(idx);
}

int GetSortedSubCanvasIndex(int pos) {
    if (!g_whiteboard_canvas) return -1;
    return g_whiteboard_canvas->GetSortedSubCanvasIndex(pos);
}

int GetSortedPosition(int idx) {
    if (!g_whiteboard_canvas) return -1;
    return g_whiteboard_canvas->GetSortedPosition(idx);
}
