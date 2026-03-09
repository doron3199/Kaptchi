// ============================================================================
// whiteboard_canvas.cpp -- ML Box-Anchored Canvas Stitcher
//
// PIPELINE OVERVIEW (runs on worker thread, once per accepted frame):
//
//   +---------------+
//   | [1] Motion    |  Frame-diff vs previous. Skip if < 0.1% pixels changed
//   |     Gate      |  (patience counter forces processing after 30 still frames)
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
//   | [4] Match     |  Consensus shift voting: every (canvas_box, frame_box)
//   |  via Box Vote |  pair of same type/size votes for a dx,dy shift.
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
#include "native_camera.h"
#include "whiteboard_enhance.h"
#include <algorithm>
#include <iostream>
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

static void DebugText(cv::Mat& img, const std::string& text,
                      cv::Point pos, double scale = 0.6,
                      cv::Scalar color = cv::Scalar(0, 255, 0)) {
    int thick = std::max(1, (int)(scale * 1.5));
    cv::putText(img, text, pos + cv::Point(1, 1),
                cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), thick + 1);
    cv::putText(img, text, pos,
                cv::FONT_HERSHEY_SIMPLEX, scale, color, thick);
}

} // namespace

// ============================================================================
//  SECTION 3: Constructor / Destructor
// ============================================================================

WhiteboardCanvas::WhiteboardCanvas() {
    stop_worker_ = false;
    perf_stats_.last_print_time = std::chrono::steady_clock::now();
    worker_thread_ = std::thread(&WhiteboardCanvas::WorkerLoop, this);
    std::cout << "[WhiteboardCanvas] Initialized (phase-correlation)" << std::endl;
}

WhiteboardCanvas::~WhiteboardCanvas() {
    stop_worker_ = true;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

// ============================================================================
//  SECTION 4: Public API
// ============================================================================

void WhiteboardCanvas::ProcessFrame(const cv::Mat& frame, const cv::Mat& person_mask) {
    if (frame.empty()) return;

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
    std::unique_lock<std::mutex> lock(state_mutex_);

    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];
    
    if (group.cache_dirty) {
        RebuildRenderCache(group);
        group.cache_dirty = false;
    }
    
    if (group.render_cache.empty()) return false;

    zoom = std::max(1.0f, zoom);
    int cw = group.render_cache.cols, ch = group.render_cache.rows;

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
    cv::resize(group.render_cache(roi), out_frame, viewSize, 0, 0, cv::INTER_LINEAR);
    return true;
}

bool WhiteboardCanvas::GetOverview(cv::Size viewSize, cv::Mat& out_frame) {
    std::unique_lock<std::mutex> lock(state_mutex_);

    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];

    if (group.cache_dirty) {
        RebuildRenderCache(group);
        group.cache_dirty = false;
    }

    if (group.render_cache.empty() || viewSize.width <= 0 || viewSize.height <= 0) {
        return false;
    }

    out_frame = cv::Mat(viewSize.height, viewSize.width, CV_8UC3,
                        cv::Scalar(255, 255, 255));

    const float src_aspect = (float)group.render_cache.cols /
                             (float)std::max(1, group.render_cache.rows);
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
    cv::resize(group.render_cache, scaled, cv::Size(draw_w, draw_h),
               0, 0, cv::INTER_AREA);

    const int offset_x = (viewSize.width - draw_w) / 2;
    const int offset_y = (viewSize.height - draw_h) / 2;
    scaled.copyTo(out_frame(cv::Rect(offset_x, offset_y, draw_w, draw_h)));
    return true;
}

void WhiteboardCanvas::Reset() {
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
    perf_stats_ = PerformanceStats();
    perf_stats_.last_print_time = std::chrono::steady_clock::now();
    std::cout << "[WhiteboardCanvas] Reset" << std::endl;
}

bool WhiteboardCanvas::HasContent()        const { return has_content_.load(); }
bool WhiteboardCanvas::IsCanvasViewMode()  const { return canvas_view_mode_.load(); }
void WhiteboardCanvas::SetCanvasViewMode(bool m) {
    if (m && !canvas_view_mode_.load()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        view_group_idx_ = active_group_idx_;
    }
    canvas_view_mode_ = m;
}
cv::Size WhiteboardCanvas::GetCanvasSize() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx >= 0 && idx < (int)groups_.size()) {
        const auto& group = *groups_[idx];
        if (!group.render_cache.empty()) {
            return cv::Size(group.render_cache.cols, group.render_cache.rows);
        }
        return cv::Size(std::max(1, group.max_px_x - group.min_px_x),
                        std::max(1, group.max_px_y - group.min_px_y));
    }
    return cv::Size(frame_w_ > 0 ? frame_w_ : 1920, frame_h_ > 0 ? frame_h_ : 1080);
}

int WhiteboardCanvas::GetSubCanvasCount() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return (int)groups_.size();
}

int WhiteboardCanvas::GetActiveSubCanvasIndex() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
}

void WhiteboardCanvas::SetActiveSubCanvas(int idx) {
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
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (pos < 0 || pos >= (int)groups_.size()) return -1;
    return pos;
}

int WhiteboardCanvas::GetSortedPosition(int idx) const {
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
            std::cerr << "[WhiteboardCanvas] CV exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[WhiteboardCanvas] Unknown exception in worker" << std::endl;
        }
    }
}

// ============================================================================
//  SECTION 6: Main pipeline -- ProcessFrameInternal
// ============================================================================

void WhiteboardCanvas::ProcessFrameInternal(const cv::Mat& frame, const cv::Mat& person_mask) {
    const bool dbg = g_whiteboard_debug.load();

    auto t_start = std::chrono::steady_clock::now();

    // -------------------------------------------------------------------
    // STAGE 1: Motion gate (Removed by user request to process every frame)
    // -------------------------------------------------------------------
    float motion_fraction = 1.0f;

    auto t_motion = std::chrono::steady_clock::now();
    perf_stats_.stage1_motion_ms += std::chrono::duration<double, std::milli>(t_motion - t_start).count();

    // -------------------------------------------------------------------
    // STAGE 2: Person occlusion mask (used as a no-update zone)
    // -------------------------------------------------------------------
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    bool has_person_mask = !person_mask.empty() && person_mask.size() == gray.size();
    cv::Mat person_mask_dilated;

    if (has_person_mask) {
        // FAST DILATION: Scale down person mask (1/8th scale)
        cv::Mat small_mask;
        cv::resize(person_mask, small_mask, cv::Size(), 0.125, 0.125, cv::INTER_NEAREST);
        
        int pad = std::max(1, (int)(std::max(small_mask.cols, small_mask.rows) * 0.10f));
        pad |= 1;
        cv::Mat k_dilate = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(pad, pad));
        cv::dilate(small_mask, small_mask, k_dilate);
        
        // Upscale back
        cv::resize(small_mask, person_mask_dilated, frame.size(), 0, 0, cv::INTER_NEAREST);
    }

    // -------------------------------------------------------------------
    // STAGE 3: Enhance + Binarize (Optimized)
    // -------------------------------------------------------------------
    // float enh_threshold = g_canvas_enhance_threshold.load();
    // cv::Mat enhanced = WhiteboardEnhance(process_frame, enh_threshold);

    // cv::Mat enhanced_gray;
    // cv::cvtColor(enhanced, enhanced_gray, cv::COLOR_BGR2GRAY);

    cv::Mat binary;
    // Binarize directly from gray input
    cv::adaptiveThreshold(gray, binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, 51, 5);
    if (has_person_mask) binary.setTo(0, person_mask_dilated);

    // Crop top 15% and bottom 10%
    const int top_cut    = (int)(gray.rows * 0.15);
    const int bottom_cut = (int)(gray.rows * 0.90);
    if (top_cut > 0)
        binary(cv::Rect(0, 0, binary.cols, top_cut)).setTo(0);
    if (bottom_cut < binary.rows)
        binary(cv::Rect(0, bottom_cut, binary.cols, binary.rows - bottom_cut)).setTo(0);
    int stroke_pixel_count = cv::countNonZero(binary);

    auto t_enhance = std::chrono::steady_clock::now();
    perf_stats_.stage3_enhance_ms += std::chrono::duration<double, std::milli>(t_enhance - t_motion).count();

    // -------------------------------------------------------------------
    // STAGE 3.5 & 4: Native Contour Matching (Throttle removed)
    // -------------------------------------------------------------------

    std::vector<ContourShape> frame_contours;
    // Native full-frame contour extraction (YOLO bypass)
    frame_contours = ExtractContourShapes(binary, cv::Point2f(0, 0));

    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (frame_w_ == 0) { frame_w_ = frame.cols; frame_h_ = frame.rows; }

    int         best_group_idx = -1;
    int         best_votes     = 0;
    cv::Point2f matched_pos(0, 0);
    std::string match_status;
    bool created_new_sc = false;

    // Rebuild canvas contour cache if stale
    if (canvas_contours_dirty_ && active_group_idx_ >= 0
        && active_group_idx_ < (int)groups_.size()) {
        RebuildCanvasContours(*groups_[active_group_idx_]);
    }

    if (active_group_idx_ >= 0 && active_group_idx_ < (int)groups_.size()
        && !canvas_contours_.empty() && !frame_contours.empty()) {

        std::unordered_map<int64_t, std::vector<cv::Point2f>> votes;

        for (const auto& cc : canvas_contours_) {
            for (const auto& fc : frame_contours) {
                double dist = cv::matchShapes(cc.contour, fc.contour,
                                              cv::CONTOURS_MATCH_I2, 0);
                if (dist > kMaxShapeDist) continue;

                float dx = cc.centroid.x - fc.centroid.x;
                float dy = cc.centroid.y - fc.centroid.y;

                int bin_x = (int)std::round(dx / 10.0f) * 10;
                int bin_y = (int)std::round(dy / 10.0f) * 10;
                int64_t key = ((int64_t)(uint32_t)bin_x << 32) | (uint32_t)bin_y;
                votes[key].push_back({dx, dy});
            }
        }

        for (const auto& kv : votes) {
            if ((int)kv.second.size() > best_votes) {
                best_votes = (int)kv.second.size();
                float sdx = 0, sdy = 0;
                for (const auto& pt : kv.second) { sdx += pt.x; sdy += pt.y; }
                matched_pos = cv::Point2f(sdx / best_votes, sdy / best_votes);
            }
        }

        if (best_votes >= kMinShapeVotes) {
            best_group_idx = active_group_idx_;
        }
    }

    auto t_match = std::chrono::steady_clock::now();
    double current_matching_ms = std::chrono::duration<double, std::milli>(t_match - t_enhance).count();
    perf_stats_.stage4_matching_ms += current_matching_ms;
    
    std::cout << "[Performance] Contours found: " << frame_contours.size() 
              << ", Canvas contours compared: " << canvas_contours_.size() << "\n";
    std::cout << "[Performance] Mapping (Contour Matching) took: " << current_matching_ms << " ms\n";

    // -------------------------------------------------------------------
    // STAGE 5: Paint to chunks or create a new group
    // -------------------------------------------------------------------
    if (best_group_idx >= 0) {
        global_camera_pos_ = matched_pos;
        auto& group = *groups_[best_group_idx];

        // Replace slow Enhance fetch with solid black strokes (BGR: 0, 0, 0)
        cv::Mat enhanced_bgr(frame.size(), CV_8UC3, cv::Scalar(0, 0, 0));

        PaintStrokesToChunks(group, binary, enhanced_bgr, global_camera_pos_,
                     person_mask_dilated);
        if (group.cache_dirty) canvas_contours_dirty_ = true;

        active_group_idx_ = best_group_idx;
        has_content_ = true;

        match_status = "MATCHED GR" + std::to_string(best_group_idx)
                     + " votes=" + std::to_string(best_votes)
                     + " pos=(" + std::to_string((int)global_camera_pos_.x)
                     + ","     + std::to_string((int)global_camera_pos_.y) + ")"
                     + " chunks=" + std::to_string(group.chunks.size());

    } else if (groups_.empty() && stroke_pixel_count >= kMinStrokePixelsForNewSC) {
        // Optimized CreateSubCanvas with black strokes
        cv::Mat enhanced_bgr(frame.size(), CV_8UC3, cv::Scalar(0, 0, 0));
        CreateSubCanvas(binary, enhanced_bgr, frame_contours, person_mask_dilated);
        created_new_sc = true;
        match_status = "NEW GROUP (total=" + std::to_string(groups_.size()) + ")";

    } else {
        match_status = "no match, skipped (strokes=" + std::to_string(stroke_pixel_count)
                     + " canvas_c=" + std::to_string(canvas_contours_.size())
                     + " frame_c="  + std::to_string(frame_contours.size())
                     + " best_votes=" + std::to_string(best_votes) + ")";
    }

    std::cout << "[WhiteboardCanvas] " << match_status << std::endl;

    auto t_paint = std::chrono::steady_clock::now();
    double current_painting_ms = std::chrono::duration<double, std::milli>(t_paint - t_match).count();
    perf_stats_.stage5_painting_ms += current_painting_ms;
    std::cout << "[Performance] Drawing (Painting) took: " << current_painting_ms << " ms\n";
    perf_stats_.frame_count++;

    // Print stats every 30 frames
    if (perf_stats_.frame_count >= 30) {
        std::cout << "\n[Performance] Average over " << perf_stats_.frame_count << " frames:\n"
                  << "  Stage 1 (Motion gate):  " << (perf_stats_.stage1_motion_ms / perf_stats_.frame_count) << " ms\n"
                  << "  Stage 3 (Enhance/Bin):  " << (perf_stats_.stage3_enhance_ms / perf_stats_.frame_count) << " ms\n"
                  << "  Stage 4 (Matching):     " << (perf_stats_.stage4_matching_ms / perf_stats_.frame_count) << " ms\n"
                  << "  Stage 5 (Painting):     " << (perf_stats_.stage5_painting_ms / perf_stats_.frame_count) << " ms\n"
                  << "  Total per frame:        " << ((perf_stats_.stage1_motion_ms + perf_stats_.stage3_enhance_ms + perf_stats_.stage4_matching_ms + perf_stats_.stage5_painting_ms) / perf_stats_.frame_count) << " ms\n\n";
                  
        perf_stats_.stage1_motion_ms = 0;
        perf_stats_.stage3_enhance_ms = 0;
        perf_stats_.stage4_matching_ms = 0;
        perf_stats_.stage5_painting_ms = 0;
        perf_stats_.frame_count = 0;
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
             dbg_frame.setTo(cv::Scalar(255, 255, 255), person_mask_dilated);
        }

        cv::Mat enhanced_bgr_dbg(frame.size(), CV_8UC3, cv::Scalar(0, 0, 0));

        PipelineDebugState ds;
        ds.frame              = dbg_frame;
        ds.gray_clean         = gray;
        ds.enhanced_bgr       = enhanced_bgr_dbg;
        ds.binary             = binary;
        ds.person_mask        = person_mask;
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
        if (cv::contourArea(c) < kMinContourArea) continue;

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
        std::copy(hu, hu + 7, shape.hu);
        result.push_back(std::move(shape));
    }
    return result;
}

// Rebuilds canvas_contours_ from the current render_cache of `group`.
// Contour centroids are stored in canvas pixel coordinates.
void WhiteboardCanvas::RebuildCanvasContours(WhiteboardGroup& group) {
    canvas_contours_.clear();
    canvas_contours_dirty_ = false;

    if (group.cache_dirty) {
        RebuildRenderCache(group);
        group.cache_dirty = false;
    }
    if (group.render_cache.empty()) return;

    // Binarize the canvas rendering
    cv::Mat gray, canvas_bin;
    cv::cvtColor(group.render_cache, gray, cv::COLOR_BGR2GRAY);
    cv::threshold(gray, canvas_bin, 200, 255, cv::THRESH_BINARY_INV);

    // Offset: render_cache pixel (0,0) = canvas pixel (min_px_x, min_px_y)
    cv::Point2f offset((float)group.min_px_x, (float)group.min_px_y);
    canvas_contours_ = ExtractContourShapes(canvas_bin, offset);
    std::cout << "[Canvas] Rebuilt contours: " << canvas_contours_.size() << "\n";
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
        chunk->canvas = cv::Mat(kChunkSize, kChunkSize, CV_8UC3, cv::Scalar(255, 255, 255));
        chunk->absence_counter = cv::Mat(kChunkSize, kChunkSize, CV_8U, cv::Scalar(0));
        chunk->grid_x = grid_x;
        chunk->grid_y = grid_y;
        group.chunks[hash] = std::move(chunk);

        group.cache_dirty = true;
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
    int clip_top = std::max(group.min_px_y - global_start_y, 0);
    int clip_bottom = std::max(global_end_y - group.max_px_y, 0);
    paint_bbox.y += clip_top;
    paint_bbox.height -= (clip_top + clip_bottom);
    global_start_y += clip_top;
    global_end_y -= clip_bottom;

    if (paint_bbox.height <= 0 || global_start_y >= global_end_y) return;

    if (global_start_x < group.min_px_x) group.min_px_x = global_start_x;
    if (global_end_x > group.max_px_x) group.max_px_x = global_end_x;

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
            chunk->canvas(chunk_roi).copyTo(footprint(foot_roi));
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
    cv::Mat canvas_stroke_zone;
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
        cv::Size(2 * kProximityRadius + 1, 2 * kProximityRadius + 1));
    cv::dilate(existing, canvas_stroke_zone, kernel);

    cv::Mat prox_allow_mask;
    cv::bitwise_not(canvas_stroke_zone, prox_allow_mask);

    cv::Mat update_allowed_mask;
    cv::bitwise_not(no_update_bbox, update_allowed_mask);

    // -----------------------------------------------------------------------
    // LAYERS 2, 3, 4: Grid-based replace, ghost, and absence erasure
    // -----------------------------------------------------------------------
    cv::Mat cell_replace_mask(footprint.size(), CV_8U, cv::Scalar(0));
    cv::Mat cell_erase_mask(footprint.size(), CV_8U, cv::Scalar(0));
    cv::Mat ghost_block(footprint.size(), CV_8U, cv::Scalar(0));

    for (int gy = 0; gy < footprint.rows; gy += kGridCellSize) {
        for (int gx = 0; gx < footprint.cols; gx += kGridCellSize) {
            int cw = std::min(kGridCellSize, footprint.cols - gx);
            int ch = std::min(kGridCellSize, footprint.rows - gy);
            cv::Rect cell(gx, gy, cw, ch);

            if (cv::countNonZero(no_update_bbox(cell)) > 0) {
                // Occluded region: preserve existing pixels and absence history.
                // The lecturer becomes a no-update zone instead of delete evidence.
                continue;
            }

            int new_count   = cv::countNonZero(new_strokes(cell));
            int exist_count = cv::countNonZero(existing(cell));

            // Layer 4: Absence Erasure
            if (exist_count >= kMinCellStrokePixels && new_count < kAbsenceEraseThr) {
                // Not enough strokes in incoming frame -> increment absence
                absence_foot(cell) += 1;
                cv::Mat absence_roi = absence_foot(cell);
                cv::Mat erase_flag = (absence_roi >= kAbsenceEraseFrames);
                if (cv::countNonZero(erase_flag) > 0) {
                    cell_erase_mask(cell).setTo(255);
                    absence_roi.setTo(0); // reset after erase
                }
                continue; // no need to evaluate replace/ghost
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
            if (iou < kCellReplaceIoU) {
                cell_replace_mask(cell).setTo(255);
                prox_allow_mask(cell).setTo(255); // Exempt from proximity block
            }
            // Layer 3: Ghost Block
            else if (overlap > kCellGhostOverlap) {
                ghost_block(cell).setTo(255);
            }
        }
    }

    // Apply ghost block
    cv::Mat ghost_allow;
    cv::bitwise_not(ghost_block, ghost_allow);
    cv::bitwise_and(prox_allow_mask, ghost_allow, prox_allow_mask);
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
                chunk->canvas(chunk_roi).setTo(cv::Scalar(255, 255, 255), erase_roi);
                chunk_dirty = true;
            }

            // Paint new strokes
            cv::Mat sub_mask = new_only(foot_roi);
            if (cv::countNonZero(sub_mask) > 0) {
                colors_bbox(foot_roi).copyTo(chunk->canvas(chunk_roi), sub_mask);
                chunk_dirty = true;
            }

            if (chunk_dirty) group.cache_dirty = true;
        }
    }
}

void WhiteboardCanvas::RebuildRenderCache(WhiteboardGroup& group) {
    if (group.chunks.empty()) {
        group.render_cache = cv::Mat();
        return;
    }

    int width = std::max(1, group.max_px_x - group.min_px_x);
    int height = std::max(1, group.max_px_y - group.min_px_y);
    group.render_cache = cv::Mat(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    for (const auto& pair : group.chunks) {
        const auto& chunk = pair.second;
        int chunk_left = chunk->grid_x * kChunkSize;
        int chunk_top = chunk->grid_y * kChunkSize;
        int chunk_right = chunk_left + kChunkSize;
        int chunk_bottom = chunk_top + kChunkSize;

        int copy_left = std::max(chunk_left, group.min_px_x);
        int copy_top = std::max(chunk_top, group.min_px_y);
        int copy_right = std::min(chunk_right, group.max_px_x);
        int copy_bottom = std::min(chunk_bottom, group.max_px_y);
        if (copy_left >= copy_right || copy_top >= copy_bottom) continue;

        cv::Rect src_roi(copy_left - chunk_left, copy_top - chunk_top,
                         copy_right - copy_left, copy_bottom - copy_top);
        cv::Rect dst_roi(copy_left - group.min_px_x, copy_top - group.min_px_y,
                         copy_right - copy_left, copy_bottom - copy_top);

        chunk->canvas(src_roi).copyTo(group.render_cache(dst_roi));
    }
}

// ============================================================================
//  SECTION 12: Create new Whiteboard Group
// ============================================================================

void WhiteboardCanvas::CreateSubCanvas(const cv::Mat& binary,
                                        const cv::Mat& enhanced_bgr,
                                        const std::vector<ContourShape>& seed_contours,
                                        const cv::Mat& no_update_mask) {
    auto group = std::make_unique<WhiteboardGroup>();
    group->debug_id = next_debug_id_++;
    const int canvas_width = std::max(frame_w_, binary.cols);
    const int canvas_height = std::max(frame_h_, binary.rows);

    group->min_px_x = 0;
    group->min_px_y = 0;
    group->max_px_x = canvas_width;
    group->max_px_y = canvas_height;

    // Seed at (0,0) global position
    global_camera_pos_ = cv::Point2f(0, 0);
    PaintStrokesToChunks(*group, binary, enhanced_bgr, global_camera_pos_,
                         no_update_mask);

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

    std::cout << "[WhiteboardCanvas] Created New Group " << idx
              << " with " << canvas_contours_.size() << " seed contours" << std::endl;
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
            DebugText(w4, "Group: " + std::to_string(active_group_idx_)
                          + " pos=(" + std::to_string((int)global_camera_pos_.x)
                          + "," + std::to_string((int)global_camera_pos_.y) + ")",
                      cv::Point(10, 30), 0.7, cv::Scalar(0, 255, 255));
            DebugText(w4, "Chunks: " + std::to_string(group.chunks.size())
                          + " bounds: " + std::to_string(group.max_px_x - group.min_px_x) + "x"
                          + std::to_string(group.max_px_y - group.min_px_y),
                      cv::Point(10, 65), 0.6, cv::Scalar(200, 200, 200));
        }
        PlaceTile(w4, 0, 1);
    }

    // Tile [1,1]-[2,1]: Active canvas (spans 2 tiles)
    if (active_group_idx_ >= 0 && active_group_idx_ < (int)groups_.size()) {
        auto& group = *groups_[active_group_idx_];
        if (group.cache_dirty) { RebuildRenderCache(group); group.cache_dirty = false; }
        cv::Mat w5 = group.render_cache.clone();
        if(!w5.empty()) {
            DebugText(w5, "World GR" + std::to_string(active_group_idx_)
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
        g_whiteboard_enabled.store(true);
    } else {
        g_whiteboard_enabled.store(false);
        if (g_whiteboard_canvas) {
            g_whiteboard_canvas->SetCanvasViewMode(false);
            g_whiteboard_canvas->Reset();
        }
    }
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
    std::cout << "[WhiteboardCanvas] " << (enabled ? "Enabled" : "Disabled") << std::endl;
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

int64_t GetCanvasTextureId() { return -1; }  // Phase 2

bool GetCanvasOverviewRgba(uint8_t* buffer, int width, int height) {
    if (!g_whiteboard_canvas || !buffer || width <= 0 || height <= 0) {
        return false;
    }

    cv::Mat overview_bgr;
    if (!g_whiteboard_canvas->GetOverview(cv::Size(width, height), overview_bgr)) {
        return false;
    }

    cv::Mat overview_rgba;
    cv::cvtColor(overview_bgr, overview_rgba, cv::COLOR_BGR2RGBA);
    cv::Mat buffer_view(height, width, CV_8UC4, buffer);
    overview_rgba.copyTo(buffer_view);
    return true;
}

void SetWhiteboardDebug(bool enabled) {
    g_whiteboard_debug.store(enabled);
    if (!enabled) cv::destroyAllWindows();
    std::cout << "[WhiteboardCanvas] Debug windows " << (enabled ? "ON" : "OFF") << std::endl;
}

void SetCanvasEnhanceThreshold(float threshold) {
    g_canvas_enhance_threshold.store(threshold);
    std::cout << "[WhiteboardCanvas] Enhance threshold=" << threshold << std::endl;
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
