// ============================================================================
// whiteboard_canvas.cpp -- Graph-Based Canvas Stitcher (simplified)
//
// PIPELINE (runs on worker thread once per accepted frame):
//
//   [1] Motion gate   -- skip frames with too much movement
//   [2] No-update mask -- person mask defines protected zone
//   [3] Binarize      -- adaptiveThreshold -> binary mask
//   [4] Blob extract  -- connected components -> FrameBlobs
//   [5] Match         -- spatial + total-shape match -> camera offset
//   [6] Graph update  -- refresh matched nodes, add new, absence-prune old
// ============================================================================

#include "whiteboard_canvas.h"
#include "whiteboard_canvas_process.h"
#include "native_camera.h"
#include "whiteboard_enhance.h"

#if __has_include(<opencv2/shape.hpp>)
#include <opencv2/shape.hpp>
#define KAPTCHI_HAS_OPENCV_SHAPE 1
#else
#define KAPTCHI_HAS_OPENCV_SHAPE 0
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>

// ============================================================================
//  SECTION 1: Globals
// ============================================================================

WhiteboardCanvas*  g_whiteboard_canvas  = nullptr;
std::atomic<bool>  g_whiteboard_enabled{false};
std::atomic<float> g_canvas_pan_x{0.5f};
std::atomic<float> g_canvas_pan_y{0.5f};
std::atomic<float> g_canvas_zoom{1.0f};
std::atomic<bool>  g_whiteboard_debug{false};
std::atomic<bool>  g_duplicate_debug_mode{false};
std::atomic<float> g_yolo_fps{2.0f};
std::atomic<float> g_canvas_enhance_threshold{4.0f};

// ============================================================================
//  SECTION 2: Static helpers
// ============================================================================

namespace {

using SteadyClock = std::chrono::steady_clock;

static constexpr int kEnhancePadding = 10;
static constexpr float kWhiteboardSideCropFraction = 0.03f;
static constexpr float kShapeCompareMinBboxRatio = 0.8f;
static constexpr float kShapeCompareLongAspectRatio = 5.0f;
static constexpr float kShapeCompareThinSideMaxPx = 30.0f;
static constexpr size_t kShapeContextSamplePointCount = 96;
static constexpr float kShapeCompareShapeContextDistanceScale = 1.0f;
static constexpr float kShapeCompareMatchShapesDistanceScale = 0.35f;
static constexpr float kShapeCompareShapeContextWeight = 0.6f;
static constexpr float kShapeCompareHuDistanceScale = 0.5f;
static constexpr float kShapeCompareHuWeight = 0.25f;
static constexpr float kShapeCompareIouWeight = 0.15f;
static constexpr float kShapeCompareStrongDifferenceThreshold = 0.2f;
static constexpr int kShapeCompareBlurKernelSize = 5;
static constexpr double kShapeCompareBlurSigma = 1.2;

enum DuplicateDebugReason : int {
    kDuplicateReasonPositionalOverlap = 1 << 0,
    kDuplicateReasonCentroidIou = 1 << 1,
    kDuplicateReasonBboxIou = 1 << 2,
    kDuplicateReasonShapeDifference = 1 << 3,
};

static void EnhanceFrameBlobs(std::vector<FrameBlob>& blobs,
                               const cv::Mat& frame_bgr, float threshold) {
    if (threshold < 0.0f || frame_bgr.empty()) return;
    for (auto& blob : blobs) {
        if (blob.color_pixels.empty()) continue;
        int px0 = std::max(0, blob.bbox.x - kEnhancePadding);
        int py0 = std::max(0, blob.bbox.y - kEnhancePadding);
        int px1 = std::min(frame_bgr.cols, blob.bbox.x + blob.bbox.width  + kEnhancePadding);
        int py1 = std::min(frame_bgr.rows, blob.bbox.y + blob.bbox.height + kEnhancePadding);
        cv::Rect padded(px0, py0, px1 - px0, py1 - py0);
        cv::Mat enhanced = WhiteboardEnhance(frame_bgr(padded).clone(), threshold);
        int cx = blob.bbox.x - px0;
        int cy = blob.bbox.y - py0;
        blob.color_pixels =
            enhanced(cv::Rect(cx, cy, blob.bbox.width, blob.bbox.height)).clone();
    }
}


static cv::Point2f ComputeGravityCenter(const cv::Mat& mask) {
    if (mask.empty()) return {};
    int sx = 0, sy = 0, cnt = 0;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x)
            if (mask.at<uchar>(y, x)) { sx += x; sy += y; ++cnt; }
    return cnt > 0 ? cv::Point2f((float)sx/cnt, (float)sy/cnt) : cv::Point2f{};
}

static float ComputeScaleForLongEdge(const cv::Size& size, int max_long_edge) {
    if (size.width <= 0 || size.height <= 0 || max_long_edge <= 0) return 1.0f;
    int le = std::max(size.width, size.height);
    return le <= max_long_edge ? 1.0f : (float)max_long_edge / (float)le;
}

static std::array<float, 7> ComputeLogHuFeatures(const double hu[7]) {
    std::array<float, 7> result;
    for (int i = 0; i < 7; i++) {
        double abs_h = std::abs(hu[i]);
        if (abs_h < 1e-30) {
            result[i] = -30.0f;
        } else {
            float sign = (hu[i] < 0.0) ? -1.0f : 1.0f;
            result[i]  = sign * (float)std::log10(abs_h);
        }
    }
    return result;
}

static float ComputeHuDistance(const double first[7], const double second[7]) {
    const auto first_hu = ComputeLogHuFeatures(first);
    const auto second_hu = ComputeLogHuFeatures(second);
    float hu_dist = 0.0f;
    for (int i = 0; i < 7; ++i) {
        const float diff = first_hu[i] - second_hu[i];
        hu_dist += diff * diff;
    }
    return std::sqrt(hu_dist);
}

static float ComputeDimensionRatio(int first, int second) {
    const float high = (float)std::max(first, second);
    const float low = (float)std::min(first, second);
    return high > 0.0f ? low / high : 1.0f;
}

static bool IsLongThinShape(const cv::Rect& bbox) {
    if (bbox.width <= 0 || bbox.height <= 0) return false;
    const float long_side = (float)std::max(bbox.width, bbox.height);
    const float short_side = (float)std::min(bbox.width, bbox.height);
    if (short_side <= 0.0f) return false;
    return short_side <= kShapeCompareThinSideMaxPx &&
           (long_side / short_side) > kShapeCompareLongAspectRatio;
}

static cv::Mat BuildShapeCompareMask(const cv::Mat& mask) {
    if (mask.empty() || mask.type() != CV_8UC1) return {};
    if (mask.rows < 3 || mask.cols < 3) return mask.clone();

    cv::Mat blurred;
    cv::GaussianBlur(
        mask,
        blurred,
        cv::Size(kShapeCompareBlurKernelSize, kShapeCompareBlurKernelSize),
        kShapeCompareBlurSigma,
        kShapeCompareBlurSigma,
        cv::BORDER_REPLICATE);

    cv::Mat smoothed;
    cv::threshold(blurred, smoothed, 127.0, 255.0, cv::THRESH_BINARY);
    return smoothed;
}

static bool ComputeHuFromMask(const cv::Mat& mask, double hu[7]) {
    const cv::Mat smoothed = BuildShapeCompareMask(mask);
    if (smoothed.empty() || cv::countNonZero(smoothed) <= 0) return false;
    const cv::Moments moments = cv::moments(smoothed, true);
    if (std::abs(moments.m00) <= 1e-6) return false;
    cv::HuMoments(moments, hu);
    return true;
}

static std::vector<cv::Point> ExtractLargestContour(const cv::Mat& mask) {
    std::vector<cv::Point> contour;
    if (mask.empty() || mask.type() != CV_8UC1) return contour;

    const cv::Mat smoothed = BuildShapeCompareMask(mask);
    const cv::Mat& source = smoothed.empty() ? mask : smoothed;

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(source.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    if (contours.empty()) return contour;

    size_t best_idx = 0;
    double best_area = -1.0;
    for (size_t idx = 0; idx < contours.size(); idx++) {
        const double area = std::abs(cv::contourArea(contours[idx]));
        if (area > best_area) {
            best_area = area;
            best_idx = idx;
        }
    }
    return contours[best_idx];
}

static std::vector<cv::Point> SampleContourForShapeContext(
        const std::vector<cv::Point>& contour) {
    if (contour.size() <= kShapeContextSamplePointCount) return contour;

    std::vector<cv::Point> sampled;
    sampled.reserve(kShapeContextSamplePointCount);
    const float stride = (float)contour.size() / (float)kShapeContextSamplePointCount;
    for (size_t idx = 0; idx < kShapeContextSamplePointCount; idx++) {
        const size_t source_idx = std::min(
            contour.size() - 1,
            (size_t)std::floor((float)idx * stride));
        sampled.push_back(contour[source_idx]);
    }
    return sampled;
}

static std::vector<cv::Point> GetContourForShapeContext(
        const std::vector<cv::Point>* contour,
        const cv::Mat* mask) {
    if (mask)
        return SampleContourForShapeContext(ExtractLargestContour(*mask));
    if (contour && contour->size() >= 3)
        return SampleContourForShapeContext(*contour);
    return {};
}

static bool ComputeDominantContourSimilarity(
        const std::vector<cv::Point>* first_contour,
        const cv::Mat* first_mask,
        const std::vector<cv::Point>* second_contour,
        const cv::Mat* second_mask,
        float& distance,
        float& similarity,
        bool& used_shape_context) {
    const std::vector<cv::Point> first =
        GetContourForShapeContext(first_contour, first_mask);
    const std::vector<cv::Point> second =
        GetContourForShapeContext(second_contour, second_mask);
    if (first.size() < 3 || second.size() < 3) return false;

#if KAPTCHI_HAS_OPENCV_SHAPE
    thread_local cv::Ptr<cv::ShapeContextDistanceExtractor> extractor =
        cv::createShapeContextDistanceExtractor();
    if (extractor.empty()) return false;

    try {
        distance = extractor->computeDistance(first, second);
    } catch (const cv::Exception&) {
        return false;
    }

    if (!std::isfinite(distance) || distance < 0.0f) return false;
    used_shape_context = true;
    similarity = kShapeCompareShapeContextDistanceScale /
                 (kShapeCompareShapeContextDistanceScale + distance);
#else
    distance = (float)cv::matchShapes(first, second, cv::CONTOURS_MATCH_I1, 0.0);
    if (!std::isfinite(distance) || distance < 0.0f) return false;
    used_shape_context = false;
    similarity = kShapeCompareMatchShapesDistanceScale /
                 (kShapeCompareMatchShapesDistanceScale + distance);
#endif
    similarity = std::clamp(similarity, 0.0f, 1.0f);
    return true;
}

static cv::Rect AlignRectToCentroid(const cv::Rect& rect,
                                    const cv::Point2f& rect_centroid,
                                    const cv::Point2f& target_centroid) {
    if (rect.width <= 0 || rect.height <= 0) return {};
    return cv::Rect(
        rect.x + (int)std::round(target_centroid.x - rect_centroid.x),
        rect.y + (int)std::round(target_centroid.y - rect_centroid.y),
        rect.width,
        rect.height);
}

static int CopyGraphNodesToBuffer(const WhiteboardGroup& group,
                                  float* buffer, int max_nodes) {
    if (!buffer || max_nodes <= 0) return 0;
    int count = 0;
    for (const auto& p : group.nodes) {
        if (count >= max_nodes) break;
        const DrawingNode& n = *p.second;
        auto edge_it = group.hard_edges.find(n.id);
        const int hard_edge_count =
            edge_it != group.hard_edges.end() ? (int)edge_it->second.size() : 0;
        float* c = buffer + count * 24;
        c[0]  = (float)n.id;
        c[1]  = (float)n.bbox_canvas.x;
        c[2]  = (float)n.bbox_canvas.y;
        c[3]  = (float)n.bbox_canvas.width;
        c[4]  = (float)n.bbox_canvas.height;
        c[5]  = n.centroid_canvas.x;
        c[6]  = n.centroid_canvas.y;
        c[7]  = (float)n.area;
        c[8]  = n.absence_score;
        c[9]  = (float)n.last_seen_frame;
        c[10] = (float)n.created_frame;
        c[11] = (float)hard_edge_count;
        c[12] = (float)group.stroke_min_px_x;
        c[13] = (float)group.stroke_min_px_y;
        c[14] = 0.0f;   // match_distance (removed)
        c[15] = n.user_locked ? 1.0f : 0.0f;
        c[16] = n.duplicate_debug_marked ? 1.0f : 0.0f;
        c[17] = (float)n.duplicate_debug_partner_id;
        c[18] = n.duplicate_debug_positional_overlap;
        c[19] = n.duplicate_debug_centroid_iou;
        c[20] = n.duplicate_debug_bbox_iou;
        c[21] = n.duplicate_debug_shape_difference;
        c[22] = (float)n.duplicate_debug_reason_mask;
        c[23] = 0.0f;
        count++;
    }
    return count;
}

static int CopyGraphHardEdgesToBuffer(const WhiteboardGroup& group,
                                      int* buffer, int max_edges) {
    if (!buffer || max_edges <= 0) return 0;
    int count = 0;
    for (const auto& entry : group.hard_edges) {
        const int node_id = entry.first;
        for (int neighbor_id : entry.second) {
            if (node_id >= neighbor_id) continue;
            if (count >= max_edges) return count;
            buffer[count * 2] = node_id;
            buffer[count * 2 + 1] = neighbor_id;
            count++;
        }
    }
    return count;
}

static bool CopyGraphBoundsToBuffer(const WhiteboardGroup& group, int* bounds) {
    if (!bounds) return false;
    bounds[0] = group.stroke_min_px_x;
    bounds[1] = group.stroke_min_px_y;
    bounds[2] = group.stroke_max_px_x;
    bounds[3] = group.stroke_max_px_y;
    return true;
}

static int CopyGraphContoursToBuffer(const WhiteboardGroup& group,
                                     float* buffer, int max_floats) {
    if (!buffer || max_floats <= 0) return 0;
    int written = 0;
    for (const auto& p : group.nodes) {
        const DrawingNode& n = *p.second;
        int np = (int)n.contour.size();
        int needed = 2 + np * 2;
        if (written + needed > max_floats) break;
        buffer[written++] = (float)n.id;
        buffer[written++] = (float)np;
        int ox = n.bbox_canvas.x, oy = n.bbox_canvas.y;
        for (const auto& pt : n.contour) {
            buffer[written++] = (float)(pt.x + ox);
            buffer[written++] = (float)(pt.y + oy);
        }
    }
    return written;
}

static const cv::Mat& GetRenderCacheForMode(const WhiteboardGroup& g, CanvasRenderMode m) {
    return m == CanvasRenderMode::kRaw ? g.raw_render_cache : g.stroke_render_cache;
}

static bool GetRenderBoundsForMode(const WhiteboardGroup& g, CanvasRenderMode m,
                                   int& mnx, int& mny, int& mxx, int& mxy) {
    if (m == CanvasRenderMode::kRaw) {
        mnx = g.raw_min_px_x; mny = g.raw_min_px_y;
        mxx = g.raw_max_px_x; mxy = g.raw_max_px_y;
    } else {
        mnx = g.stroke_min_px_x; mny = g.stroke_min_px_y;
        mxx = g.stroke_max_px_x; mxy = g.stroke_max_px_y;
    }
    return mxx > mnx && mxy > mny;
}

static cv::Rect BuildHorizontalCropRect(const cv::Size& sz) {
    if (sz.width <= 0 || sz.height <= 0) return {};
    const int side_crop = std::max(0, (int)std::lround(sz.width * kWhiteboardSideCropFraction));
    const int left = std::min(side_crop, std::max(0, sz.width - 1));
    const int right = std::max(left + 1, sz.width - side_crop);
    return cv::Rect(left, 0, right - left, sz.height);
}

static cv::Rect ComputeProcessingRoi(const cv::Size& sz) {
    return BuildHorizontalCropRect(sz);
}

static cv::Mat CropPersonMaskForProcessing(const cv::Mat& mask, const cv::Rect& roi) {
    if (mask.empty() || mask.type() != CV_8UC1 || roi.width <= 0 || roi.height <= 0) return {};
    cv::Rect mb(0, 0, mask.cols, mask.rows);
    cv::Rect clip = roi & mb;
    if (clip.width != roi.width || clip.height != roi.height) return {};
    return mask(clip).clone();
}

static cv::Mat BuildNoUpdateMask(const cv::Mat& gray, const cv::Mat& person_mask) {
    if (person_mask.empty() || person_mask.size() != gray.size() ||
        person_mask.type() != CV_8UC1) {
        return cv::Mat::zeros(gray.size(), CV_8UC1);
    }
    return person_mask.clone();
}


static cv::Rect ComputeMaskBoundingRect(const cv::Mat& mask) {
    if (mask.empty() || mask.type() != CV_8UC1) return {};
    if (cv::countNonZero(mask) <= 0) return {};
    return cv::boundingRect(mask);
}

static cv::Rect BuildCroppedFrameRect(int fw, int fh) {
    return BuildHorizontalCropRect(cv::Size(fw, fh));
}

static cv::Rect ExpandRectWithinFrame(const cv::Rect& r, const cv::Size& fs, int px, int py) {
    if (r.width <= 0 || r.height <= 0 || fs.width <= 0 || fs.height <= 0) return {};
    int x0 = std::max(0, r.x - std::max(0, px));
    int y0 = std::max(0, r.y - std::max(0, py));
    int x1 = std::min(fs.width,  r.x + r.width  + std::max(0, px));
    int y1 = std::min(fs.height, r.y + r.height + std::max(0, py));
    if (x1 <= x0 || y1 <= y0) return {};
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

static cv::Mat BuildFrameStrokeRejectMask(const cv::Size& fs,
                                          const cv::Rect& lecturer_rect,
                                          float min_margin_hint = 1.0f) {
    if (fs.width <= 0 || fs.height <= 0) return {};
    cv::Mat reject(fs, CV_8UC1, cv::Scalar(0));
    int min_margin = std::max(1, (int)std::ceil(min_margin_hint));
    int side = std::min(fs.width, std::max(min_margin, fs.width / 100));
    reject(cv::Rect(0, 0, side, fs.height)).setTo(255);
    int rs = std::max(0, fs.width - side);
    reject(cv::Rect(rs, 0, fs.width - rs, fs.height)).setTo(255);
    int lmx = std::max(min_margin, fs.width  / 100);
    int lmy = std::max(min_margin, fs.height / 100);
    cv::Rect exp = ExpandRectWithinFrame(lecturer_rect, fs, lmx, lmy);
    if (exp.width > 0) reject(exp).setTo(255);
    return reject;
}

static bool BlobTouchesRejectMask(const FrameBlob& blob, const cv::Mat& reject_mask) {
    if (reject_mask.empty() || blob.binary_mask.empty()) return false;
    cv::Rect mb(0, 0, reject_mask.cols, reject_mask.rows);
    cv::Rect clip = blob.bbox & mb;
    if (clip.width <= 0 || clip.height <= 0) return false;
    cv::Rect br(clip.x - blob.bbox.x, clip.y - blob.bbox.y, clip.width, clip.height);
    if (br.x < 0 || br.y < 0 ||
        br.x + br.width  > blob.binary_mask.cols ||
        br.y + br.height > blob.binary_mask.rows) return false;
    cv::Mat ov;
    cv::bitwise_and(blob.binary_mask(br), reject_mask(clip), ov);
    return cv::countNonZero(ov) > 0;
}

static void FilterBlobsForCanvas(std::vector<FrameBlob>& blobs, const cv::Mat& reject_mask,
                                  int min_width_bypass) {
    blobs.erase(
        std::remove_if(blobs.begin(), blobs.end(),
            [&](const FrameBlob& b) {
                if (b.bbox.width > min_width_bypass)
                    return false;
                return BlobTouchesRejectMask(b, reject_mask);
            }),
        blobs.end());
}

static cv::Rect TranslateFrameRectToCanvas(const cv::Rect& r, const cv::Point2f& offset) {
    if (r.width <= 0 || r.height <= 0) return {};
    return cv::Rect(r.x + (int)std::round(offset.x),
                    r.y + (int)std::round(offset.y),
                    r.width, r.height);
}

static cv::Rect TranslateCanvasRectToFrame(const cv::Rect& r, const cv::Point2f& offset) {
    if (r.width <= 0 || r.height <= 0) return {};
    return cv::Rect(r.x - (int)std::round(offset.x),
                    r.y - (int)std::round(offset.y),
                    r.width, r.height);
}

static cv::Rect ExpandRectByPixels(const cv::Rect& r, int px, int py) {
    if (r.width <= 0 || r.height <= 0) return {};
    int ex = std::max(0, px);
    int ey = std::max(0, py);
    return cv::Rect(r.x - ex, r.y - ey, r.width + ex * 2, r.height + ey * 2);
}

static float ComputeRectOverlapFraction(const cv::Rect& subject, const cv::Rect& occluder) {
    if (subject.width <= 0 || subject.height <= 0 ||
        occluder.width <= 0 || occluder.height <= 0) return 0.0f;
    const cv::Rect isect = subject & occluder;
    if (isect.width <= 0 || isect.height <= 0) return 0.0f;
    return (float)isect.area() / (float)std::max(1, subject.area());
}

static bool IsNodePlausiblyVisibleForAbsence(const DrawingNode& node,
                                             const cv::Point2f& frame_offset,
                                             const cv::Rect& cropped_frame,
                                             const cv::Size& frame_size,
                                             int frame_inset_px,
                                             float visible_fraction_min) {
    if (cropped_frame.width <= 0 || cropped_frame.height <= 0 ||
        frame_size.width <= 0 || frame_size.height <= 0) {
        return false;
    }

    const cv::Rect node_frame_bbox = TranslateCanvasRectToFrame(node.bbox_canvas, frame_offset);
    if (node_frame_bbox.width <= 0 || node_frame_bbox.height <= 0) return false;

    // Shrink the cropped frame inward to define the "observable region."
    // The inset exceeds the reject-mask edge strips so nodes in the
    // reject-mask dead zone (frame edges, lecturer boundary) are not penalized.
    const int inset_x = (frame_inset_px >= 0)
        ? frame_inset_px
        : std::max(1, frame_size.width / 50);
    const int inset_y = (frame_inset_px >= 0)
        ? frame_inset_px
        : std::max(1, frame_size.height / 50);

    const int ox = cropped_frame.x + inset_x;
    const int oy = cropped_frame.y + inset_y;
    const int ow = cropped_frame.width  - 2 * inset_x;
    const int oh = cropped_frame.height - 2 * inset_y;
    if (ow <= 0 || oh <= 0) return false;
    const cv::Rect observable_frame(ox, oy, ow, oh);

    return ComputeRectOverlapFraction(node_frame_bbox, observable_frame)
               >= visible_fraction_min;
}

static bool IsNodeOccludedByLecturerForAbsence(const DrawingNode& node,
                                               const cv::Rect& lecturer_canvas_rect,
                                               const cv::Size& frame_size,
                                               int lecturer_expansion_px,
                                               float lecturer_overlap_min) {
    if (lecturer_canvas_rect.width <= 0 || lecturer_canvas_rect.height <= 0) return false;

    // Use ~2% expansion by default, strictly larger than the reject mask's ~1%.
    const int exp_x = (lecturer_expansion_px >= 0)
        ? lecturer_expansion_px
        : std::max(1, frame_size.width / 50);
    const int exp_y = (lecturer_expansion_px >= 0)
        ? lecturer_expansion_px
        : std::max(1, frame_size.height / 50);
    const cv::Rect protected_lecturer_rect =
        ExpandRectByPixels(lecturer_canvas_rect, exp_x, exp_y);

    const cv::Point2i cp((int)std::round(node.centroid_canvas.x),
                         (int)std::round(node.centroid_canvas.y));
    if (protected_lecturer_rect.contains(cp)) return true;

    return ComputeRectOverlapFraction(node.bbox_canvas, protected_lecturer_rect) >=
        lecturer_overlap_min;
}

// ---------------------------------------------------------------------------
// MaskRelation
// ---------------------------------------------------------------------------
struct MaskRelation {
    bool  valid = false;
    int   overlap_px = 0, first_px = 0, second_px = 0;
    float overlap_over_min = 0.0f;
    float iou = 0.0f;
};

struct TotalShapeCompareResult {
    bool  valid = false;
    bool  used_dimension_only = false;
    bool  used_shape_context = false;
    float width_ratio = 0.0f;
    float height_ratio = 0.0f;
    float bbox_ratio = 0.0f;
    float shape_context_distance = -1.0f;
    float shape_context_similarity = 0.0f;
    float shape_context_difference = 1.0f;
    float mask_iou = 0.0f;
    float mask_iou_difference = 1.0f;
    float hu_distance = 0.0f;
    float hu_similarity = 0.0f;
    float hu_difference = 1.0f;
    float similarity = 0.0f;
    float difference = 1.0f;
};

struct DuplicateCandidateView {
    cv::Rect bbox;
    const cv::Mat* mask = nullptr;
    const std::vector<cv::Point>* contour = nullptr;
    cv::Point2f centroid;
    const double* hu = nullptr;
    int created_frame = 0;
};

struct DuplicateCheckResult {
    float positional_overlap = 0.0f;
    float centroid_iou = 0.0f;
    float bbox_iou = 0.0f;
    float shape_difference = 1.0f;
    int reason_mask = 0;
    bool same_creation_frame = false;
    bool is_duplicate = false;
};

static void RemoveHardEdges(WhiteboardGroup& group, int node_id);

static bool IsGhostNode(const DrawingNode& node) {
    return node.duplicate_debug_marked;
}

static bool IsNodeVisibleInMainCanvas(const DrawingNode& node) {
    return !IsGhostNode(node) && node.has_crossed_absence_seen_threshold;
}

static bool UpdateMainCanvasVisibilityFromAbsence(DrawingNode& node) {
    if (node.has_crossed_absence_seen_threshold) return false;
    if (node.absence_score < kAbsenceScoreSeenThreshold) return false;
    node.has_crossed_absence_seen_threshold = true;
    return true;
}

static void ClearDuplicateDebugInfo(DrawingNode& node) {
    node.duplicate_debug_marked = false;
    node.duplicate_debug_partner_id = -1;
    node.duplicate_debug_positional_overlap = 0.0f;
    node.duplicate_debug_centroid_iou = 0.0f;
    node.duplicate_debug_bbox_iou = 0.0f;
    node.duplicate_debug_shape_difference = 1.0f;
    node.duplicate_debug_reason_mask = 0;
}

static bool MarkDuplicateDebugInfo(DrawingNode& node,
                                   int partner_id,
                                   const DuplicateCheckResult& check) {
    const bool changed =
        !node.duplicate_debug_marked ||
        node.duplicate_debug_partner_id != partner_id ||
        node.duplicate_debug_reason_mask != check.reason_mask ||
        std::abs(node.duplicate_debug_positional_overlap - check.positional_overlap) > 1e-6f ||
        std::abs(node.duplicate_debug_centroid_iou - check.centroid_iou) > 1e-6f ||
        std::abs(node.duplicate_debug_bbox_iou - check.bbox_iou) > 1e-6f ||
        std::abs(node.duplicate_debug_shape_difference - check.shape_difference) > 1e-6f;
    node.duplicate_debug_marked = true;
    node.duplicate_debug_partner_id = partner_id;
    node.duplicate_debug_positional_overlap = check.positional_overlap;
    node.duplicate_debug_centroid_iou = check.centroid_iou;
    node.duplicate_debug_bbox_iou = check.bbox_iou;
    node.duplicate_debug_shape_difference = check.shape_difference;
    node.duplicate_debug_reason_mask = check.reason_mask;
    return changed;
}

static bool ConvertNodeToGhost(WhiteboardGroup& group,
                               DrawingNode& node,
                               int partner_id,
                               const DuplicateCheckResult& check) {
    const bool was_ghost = IsGhostNode(node);
    bool changed = MarkDuplicateDebugInfo(node, partner_id, check);
    if (!was_ghost) {
        group.spatial_index.Remove(node.id, node.centroid_canvas);
        RemoveHardEdges(group, node.id);
        changed = true;
    }
    return changed;
}

static bool RemoveDuplicateGhostNodes(WhiteboardGroup& group) {
    std::vector<int> ghost_ids;
    for (const auto& [nid, node_ptr] : group.nodes) {
        if (IsGhostNode(*node_ptr)) ghost_ids.push_back(nid);
    }
    for (int nid : ghost_ids) {
        RemoveHardEdges(group, nid);
        auto it = group.nodes.find(nid);
        if (it == group.nodes.end()) continue;
        group.spatial_index.Remove(nid, it->second->centroid_canvas);
        group.nodes.erase(it);
    }
    return !ghost_ids.empty();
}

static DrawingNode* AddNodeFromBlob(WhiteboardGroup& group,
                                    const FrameBlob& blob,
                                    const cv::Point2f& canvas_centroid,
                                    const cv::Rect& canvas_bbox,
                                                                        int current_frame,
                                                                        bool add_to_spatial_index = true) {
    auto node = std::make_unique<DrawingNode>();
    node->id = group.next_node_id++;
    node->binary_mask = blob.binary_mask.clone();
    if (!blob.color_pixels.empty()) node->color_pixels = blob.color_pixels.clone();
    node->bbox_canvas = canvas_bbox;
    node->centroid_canvas = canvas_centroid;
    node->contour = blob.contour;
    std::copy(blob.hu, blob.hu + 7, node->hu);
    node->area = blob.area;
    node->absence_score = kAbsenceScoreInitial;
    node->has_crossed_absence_seen_threshold =
        node->absence_score >= kAbsenceScoreSeenThreshold;
    node->last_seen_frame = current_frame;
    node->created_frame = current_frame;
    ClearDuplicateDebugInfo(*node);
    const int nid = node->id;
    DrawingNode* raw_node = node.get();
    if (add_to_spatial_index) {
        group.spatial_index.Insert(nid, canvas_centroid);
    }
    group.nodes[nid] = std::move(node);
    return raw_node;
}

static MaskRelation ComputeMaskRelation(const cv::Rect& fb, const cv::Mat& fm,
                                        const cv::Point2f&,
                                        const cv::Rect& sb, const cv::Mat& sm,
                                        const cv::Point2f&) {
    MaskRelation rel;
    if (fm.empty() || sm.empty()) return rel;
    if (fm.cols != fb.width || fm.rows != fb.height ||
        sm.cols != sb.width || sm.rows != sb.height) return rel;
    rel.first_px  = cv::countNonZero(fm);
    rel.second_px = cv::countNonZero(sm);
    if (rel.first_px <= 0 || rel.second_px <= 0) return rel;
    cv::Rect isect = fb & sb;
    if (isect.empty()) { rel.valid = true; return rel; }
    cv::Rect fl(isect.x - fb.x, isect.y - fb.y, isect.width, isect.height);
    cv::Rect sl(isect.x - sb.x, isect.y - sb.y, isect.width, isect.height);
    if (fl.x < 0 || fl.y < 0 || fl.x + fl.width > fm.cols || fl.y + fl.height > fm.rows ||
        sl.x < 0 || sl.y < 0 || sl.x + sl.width > sm.cols || sl.y + sl.height > sm.rows) {
        rel.valid = true; return rel;
    }
    cv::Mat ov; cv::bitwise_and(fm(fl), sm(sl), ov);
    rel.overlap_px = cv::countNonZero(ov);
    rel.valid = true;
    if (rel.overlap_px <= 0) return rel;
    int mn = std::min(rel.first_px, rel.second_px);
    int un = rel.first_px + rel.second_px - rel.overlap_px;
    rel.overlap_over_min = (float)rel.overlap_px / (float)std::max(1, mn);
    rel.iou = (float)rel.overlap_px / (float)std::max(1, un);
    return rel;
}

static float ComputeBboxIou(const cv::Rect& first, const cv::Rect& second) {
    const cv::Rect isect = first & second;
    if (isect.width <= 0 || isect.height <= 0) return 0.0f;
    const int union_area = first.area() + second.area() - isect.area();
    return (float)isect.area() / (float)std::max(1, union_area);
}

static TotalShapeCompareResult TotalShapeCompare(const cv::Rect& first_bbox,
                                                 const cv::Mat* first_mask,
                                                 const double* first_hu,
                                                 const std::vector<cv::Point>* first_contour,
                                                 const cv::Rect& second_bbox,
                                                 const cv::Mat* second_mask,
                                                 const double* second_hu,
                                                 const std::vector<cv::Point>* second_contour) {
    TotalShapeCompareResult result;
    if (first_bbox.width <= 0 || first_bbox.height <= 0 ||
        second_bbox.width <= 0 || second_bbox.height <= 0) {
        return result;
    }

    result.valid = true;
    result.width_ratio = ComputeDimensionRatio(first_bbox.width, second_bbox.width);
    result.height_ratio = ComputeDimensionRatio(first_bbox.height, second_bbox.height);
    result.bbox_ratio = std::min(result.width_ratio, result.height_ratio);

    if (IsLongThinShape(first_bbox) || IsLongThinShape(second_bbox)) {
        result.used_dimension_only = true;
        result.similarity = 0.5f * (result.width_ratio + result.height_ratio);
        result.similarity = std::clamp(result.similarity, 0.0f, 1.0f);
        result.difference = 1.0f - result.similarity;
        return result;
    }

    if (result.bbox_ratio < kShapeCompareMinBboxRatio) {
        return result;
    }

    float weighted_difference = 0.0f;
    float total_weight = 0.0f;
    bool strong_component_match = false;
    if (ComputeDominantContourSimilarity(first_contour,
                                         first_mask,
                                         second_contour,
                                         second_mask,
                                         result.shape_context_distance,
                                         result.shape_context_similarity,
                                         result.used_shape_context)) {
        result.shape_context_difference =
            std::clamp(1.0f - result.shape_context_similarity, 0.0f, 1.0f);
        weighted_difference +=
            kShapeCompareShapeContextWeight * result.shape_context_difference;
        total_weight += kShapeCompareShapeContextWeight;
        strong_component_match =
            strong_component_match ||
            result.shape_context_difference <= kShapeCompareStrongDifferenceThreshold;
    }

    double first_mask_hu[7] = {};
    double second_mask_hu[7] = {};
    const double* first_hu_for_compare = first_hu;
    const double* second_hu_for_compare = second_hu;
    if (first_mask && second_mask &&
        ComputeHuFromMask(*first_mask, first_mask_hu) &&
        ComputeHuFromMask(*second_mask, second_mask_hu)) {
        first_hu_for_compare = first_mask_hu;
        second_hu_for_compare = second_mask_hu;
    }

    if (first_hu_for_compare && second_hu_for_compare) {
        result.hu_distance = ComputeHuDistance(first_hu_for_compare, second_hu_for_compare);
        result.hu_similarity = std::clamp(
            1.0f - result.hu_distance / kShapeCompareHuDistanceScale,
            0.0f,
            1.0f);
        result.hu_difference = std::clamp(1.0f - result.hu_similarity, 0.0f, 1.0f);
        weighted_difference += kShapeCompareHuWeight * result.hu_difference;
        total_weight += kShapeCompareHuWeight;
        strong_component_match =
            strong_component_match ||
            result.hu_difference <= kShapeCompareStrongDifferenceThreshold;
    }

    const cv::Mat empty_mask;
    const cv::Mat& fm = first_mask ? *first_mask : empty_mask;
    const cv::Mat& sm = second_mask ? *second_mask : empty_mask;
    const MaskRelation relation = ComputeMaskRelation(
        first_bbox, fm, {}, second_bbox, sm, {});
    if (relation.valid) {
        result.mask_iou = std::clamp(relation.iou, 0.0f, 1.0f);
        result.mask_iou_difference = std::clamp(1.0f - result.mask_iou, 0.0f, 1.0f);
        weighted_difference += kShapeCompareIouWeight * result.mask_iou_difference;
        total_weight += kShapeCompareIouWeight;
        strong_component_match =
            strong_component_match ||
            result.mask_iou_difference <= kShapeCompareStrongDifferenceThreshold;
    }

    if (strong_component_match) {
        result.difference = 0.0f;
        result.similarity = 1.0f;
        return result;
    }

    if (total_weight > 0.0f) {
        result.difference = weighted_difference / total_weight;
    }
    result.difference = std::clamp(result.difference, 0.0f, 1.0f);
    result.similarity = 1.0f - result.difference;
    return result;
}

static DuplicateCheckResult EvaluateDuplicateCandidate(
        const DuplicateCandidateView& first,
        const DuplicateCandidateView& second,
        float duplicate_pos_overlap_threshold,
    float duplicate_centroid_iou_threshold,
        float duplicate_bbox_iou_threshold,
        float duplicate_max_shape_difference) {
    DuplicateCheckResult result;

    const cv::Mat empty_mask;
    const cv::Mat& first_mask = first.mask ? *first.mask : empty_mask;
    const cv::Mat& second_mask = second.mask ? *second.mask : empty_mask;

    const MaskRelation positional_relation = ComputeMaskRelation(
        first.bbox, first_mask, first.centroid,
        second.bbox, second_mask, second.centroid);
    result.positional_overlap = positional_relation.valid
        ? positional_relation.overlap_over_min
        : 0.0f;
    result.bbox_iou = ComputeBboxIou(first.bbox, second.bbox);

    const int dx = (int)std::round(first.centroid.x - second.centroid.x);
    const int dy = (int)std::round(first.centroid.y - second.centroid.y);
    const cv::Rect centroid_aligned_bbox(second.bbox.x + dx,
                                         second.bbox.y + dy,
                                         second.bbox.width,
                                         second.bbox.height);
    const MaskRelation centroid_relation = ComputeMaskRelation(
        first.bbox, first_mask, first.centroid,
        centroid_aligned_bbox, second_mask, first.centroid);
    result.centroid_iou = centroid_relation.valid
        ? centroid_relation.iou
        : 0.0f;
    result.same_creation_frame = first.created_frame == second.created_frame;
    result.shape_difference = TotalShapeCompare(
        first.bbox,
        first.mask,
        first.hu,
        first.contour,
        centroid_aligned_bbox,
        second.mask,
        second.hu,
        second.contour).difference;
    if (result.positional_overlap > duplicate_pos_overlap_threshold)
        result.reason_mask |= kDuplicateReasonPositionalOverlap;
    if (result.centroid_iou > duplicate_centroid_iou_threshold)
        result.reason_mask |= kDuplicateReasonCentroidIou;
    if (result.bbox_iou > duplicate_bbox_iou_threshold)
        result.reason_mask |= kDuplicateReasonBboxIou;
    if (!result.same_creation_frame &&
        result.shape_difference < duplicate_max_shape_difference) {
        result.reason_mask |= kDuplicateReasonShapeDifference;
    }
    result.is_duplicate = result.reason_mask != 0;
    return result;
}

static void RefreshNodeFromBlob(WhiteboardGroup& group, DrawingNode& node,
                                const FrameBlob& blob,
                                const cv::Point2f& canvas_centroid,
                                const cv::Rect& canvas_bbox) {
    group.spatial_index.Remove(node.id, node.centroid_canvas);
    node.centroid_canvas = canvas_centroid;
    node.bbox_canvas     = canvas_bbox;
    node.binary_mask     = blob.binary_mask.clone();
    if (!blob.color_pixels.empty()) node.color_pixels = blob.color_pixels.clone();
    node.contour = blob.contour;
    std::copy(blob.hu, blob.hu + 7, node.hu);
    node.area = blob.area;
    ClearDuplicateDebugInfo(node);
    group.spatial_index.Insert(node.id, canvas_centroid);
}

static bool InsertOrMergeBlobNode(WhiteboardGroup& group,
                                  const FrameBlob& blob,
                                  const cv::Point2f& canvas_centroid,
                                  const cv::Rect& canvas_bbox,
                                  int current_frame,
                                  bool enable_duplicate_merge,
                                  bool duplicate_debug_mode,
                                  float merge_search_radius_px,
                                  float duplicate_pos_overlap_threshold,
                                  float duplicate_centroid_iou_threshold,
                                  float duplicate_bbox_iou_threshold,
                                  float duplicate_max_shape_difference) {
    if (enable_duplicate_merge) {
        const float search_r = std::max({(float)canvas_bbox.width,
                                         (float)canvas_bbox.height,
                                         merge_search_radius_px});
        const auto nearby = group.spatial_index.QueryRadius(canvas_centroid, search_r);
        const DuplicateCandidateView blob_candidate{
            canvas_bbox, &blob.binary_mask, &blob.contour, canvas_centroid, blob.hu, current_frame};
        for (int nid : nearby) {
            if (group.user_deleted_ids.count(nid)) continue;
            auto nit = group.nodes.find(nid);
            if (nit == group.nodes.end()) continue;

            auto& existing = *nit->second;
            const DuplicateCandidateView existing_candidate{
                existing.bbox_canvas,
                &existing.binary_mask,
                &existing.contour,
                existing.centroid_canvas,
                existing.hu,
                existing.created_frame};
            const DuplicateCheckResult duplicate_check = EvaluateDuplicateCandidate(
                existing_candidate,
                blob_candidate,
                duplicate_pos_overlap_threshold,
                duplicate_centroid_iou_threshold,
                duplicate_bbox_iou_threshold,
                duplicate_max_shape_difference);

            if (!duplicate_check.is_duplicate) {
                continue;
            }

            if (duplicate_debug_mode) {
                DrawingNode* duplicate_node = AddNodeFromBlob(
                    group, blob, canvas_centroid, canvas_bbox, current_frame, false);
                if (duplicate_node) {
                    MarkDuplicateDebugInfo(*duplicate_node, existing.id, duplicate_check);
                    return true;
                }
                return false;
            }

            if (!existing.user_locked && blob.area > existing.area) {
                existing.last_seen_frame = current_frame;
                existing.created_frame = std::min(existing.created_frame, current_frame);
                RefreshNodeFromBlob(group, existing, blob, canvas_centroid, canvas_bbox);
                return true;
            }
            return false;
        }
    }

    return AddNodeFromBlob(group, blob, canvas_centroid, canvas_bbox, current_frame) != nullptr;
}

// ---------------------------------------------------------------------------
// Hard-edge helpers
// ---------------------------------------------------------------------------

static bool AddHardEdge(WhiteboardGroup& group, int first_id, int second_id) {
    if (first_id == second_id) return false;
    const bool inserted_first = group.hard_edges[first_id].insert(second_id).second;
    const bool inserted_second = group.hard_edges[second_id].insert(first_id).second;
    return inserted_first || inserted_second;
}

static void RemoveHardEdge(WhiteboardGroup& group, int first_id, int second_id) {
    auto first_it = group.hard_edges.find(first_id);
    if (first_it != group.hard_edges.end()) {
        first_it->second.erase(second_id);
        if (first_it->second.empty()) group.hard_edges.erase(first_it);
    }
    auto second_it = group.hard_edges.find(second_id);
    if (second_it != group.hard_edges.end()) {
        second_it->second.erase(first_id);
        if (second_it->second.empty()) group.hard_edges.erase(second_it);
    }
}

static bool HasHardEdgeBetween(const WhiteboardGroup& group, int first_id, int second_id) {
    const auto first_it = group.hard_edges.find(first_id);
    if (first_it == group.hard_edges.end()) return false;
    return first_it->second.count(second_id) > 0;
}

static DuplicateCheckResult SuppressDuplicateForHardEdge(
        const WhiteboardGroup& group,
        int first_id,
        int second_id,
        DuplicateCheckResult result) {
    if (!HasHardEdgeBetween(group, first_id, second_id)) return result;
    result.reason_mask = 0;
    result.is_duplicate = false;
    return result;
}

static float ComputeMean(const std::vector<float>& values) {
    if (values.empty()) return 0.0f;
    return std::accumulate(values.begin(), values.end(), 0.0f) /
           (float)values.size();
}

template <typename MatchLike>
static cv::Point2f ComputeMeanDeltaVector(const std::vector<MatchLike>& matches) {
    if (matches.empty()) return {};
    cv::Point2f sum_delta(0, 0);
    for (const auto& match : matches) sum_delta += match.delta_vec;
    return cv::Point2f(sum_delta.x / (float)matches.size(),
                       sum_delta.y / (float)matches.size());
}

template <typename MatchLike>
static std::vector<MatchLike> PruneMatchesAroundMean(const std::vector<MatchLike>& matches,
                                                     const cv::Point2f& mean_delta,
                                                     float threshold) {
    std::vector<MatchLike> inliers;
    if (matches.empty()) return inliers;
    const float threshold2 = threshold * threshold;
    for (const auto& match : matches) {
        const cv::Point2f diff = match.delta_vec - mean_delta;
        if (diff.x * diff.x + diff.y * diff.y <= threshold2)
            inliers.push_back(match);
    }
    return inliers;
}

static int GetHorizontalMatchPartition(float x, int frame_width, int partition_count) {
    if (partition_count <= 1 || frame_width <= 0) return 0;
    const float normalized = std::clamp(x / (float)frame_width, 0.0f, 0.999999f);
    return std::min(partition_count - 1,
                    std::max(0, (int)std::floor(normalized * (float)partition_count)));
}

// BFS to collect the full connected component reachable from start_id.
static std::unordered_set<int> GetHardEdgeComponent(
        const WhiteboardGroup& group, int start_id) {
    std::unordered_set<int> visited;
    auto it = group.hard_edges.find(start_id);
    if (it == group.hard_edges.end()) return visited;  // no edges at all
    std::vector<int> stack;
    stack.push_back(start_id);
    visited.insert(start_id);
    while (!stack.empty()) {
        int cur = stack.back(); stack.pop_back();
        auto eit = group.hard_edges.find(cur);
        if (eit == group.hard_edges.end()) continue;
        for (int neighbor : eit->second) {
            if (visited.insert(neighbor).second)
                stack.push_back(neighbor);
        }
    }
    return visited;
}

// Remove all hard-edge entries for a node (symmetric cleanup).
static void RemoveHardEdges(WhiteboardGroup& group, int node_id) {
    auto it = group.hard_edges.find(node_id);
    if (it == group.hard_edges.end()) return;
    const std::vector<int> neighbors(it->second.begin(), it->second.end());
    for (int neighbor : neighbors)
        RemoveHardEdge(group, node_id, neighbor);
    group.hard_edges.erase(node_id);
}

static void TransferHardEdges(WhiteboardGroup& group, int winner_id, int loser_id) {
    auto it = group.hard_edges.find(loser_id);
    if (it == group.hard_edges.end()) return;
    for (int neighbor : it->second) {
        if (neighbor != winner_id)
            AddHardEdge(group, winner_id, neighbor);
    }
}

// For nodes seen or created in the same frame, create hard edges between
// those whose centroids are within max_dist.
static bool CreateHardEdgesForFrame(
        WhiteboardGroup& group, const std::vector<int>& node_ids,
        float max_dist) {
    bool edges_added = false;
    const float max_dist2 = max_dist * max_dist;
    for (size_t i = 0; i < node_ids.size(); i++) {
        auto ai = group.nodes.find(node_ids[i]);
        if (ai == group.nodes.end()) continue;
        const cv::Point2f& ca = ai->second->centroid_canvas;
        for (size_t j = i + 1; j < node_ids.size(); j++) {
            auto aj = group.nodes.find(node_ids[j]);
            if (aj == group.nodes.end()) continue;
            cv::Point2f d = ca - aj->second->centroid_canvas;
            if (d.x * d.x + d.y * d.y <= max_dist2)
                edges_added = AddHardEdge(group, node_ids[i], node_ids[j]) || edges_added;
        }
    }
    return edges_added;
}

static bool PruneHardEdgesByDistance(WhiteboardGroup& group, float max_dist) {
    const float max_dist2 = max_dist * max_dist;
    std::vector<std::pair<int, int>> edges_to_break;
    for (const auto& [first_id, neighbors] : group.hard_edges) {
        auto first_it = group.nodes.find(first_id);
        if (first_it == group.nodes.end()) {
            for (int neighbor_id : neighbors) {
                if (first_id < neighbor_id)
                    edges_to_break.emplace_back(first_id, neighbor_id);
            }
            continue;
        }

        for (int neighbor_id : neighbors) {
            if (first_id >= neighbor_id) continue;
            auto second_it = group.nodes.find(neighbor_id);
            if (second_it == group.nodes.end()) {
                edges_to_break.emplace_back(first_id, neighbor_id);
                continue;
            }
            const cv::Point2f delta =
                first_it->second->centroid_canvas - second_it->second->centroid_canvas;
            if (delta.x * delta.x + delta.y * delta.y > max_dist2)
                edges_to_break.emplace_back(first_id, neighbor_id);
        }
    }

    for (const auto& edge : edges_to_break)
        RemoveHardEdge(group, edge.first, edge.second);
    return !edges_to_break.empty();
}

// Translate all nodes in a hard-edge component (except those in already_moved)
// by (dx, dy), updating spatial index, centroid, and bbox.
static void ApplyHardEdgeDelta(
        WhiteboardGroup& group, int source_id, float dx, float dy,
        const std::unordered_set<int>& already_moved) {
    auto component = GetHardEdgeComponent(group, source_id);
    int idx = (int)std::round(dx);
    int idy = (int)std::round(dy);
    for (int nid : component) {
        if (already_moved.count(nid)) continue;
        auto nit = group.nodes.find(nid);
        if (nit == group.nodes.end()) continue;
        auto& node = *nit->second;
        if (IsGhostNode(node)) continue;
        group.spatial_index.Remove(node.id, node.centroid_canvas);
        node.centroid_canvas.x += dx;
        node.centroid_canvas.y += dy;
        node.bbox_canvas.x += idx;
        node.bbox_canvas.y += idy;
        group.spatial_index.Insert(node.id, node.centroid_canvas);
    }
}

static int SelectDuplicateWinnerId(const DrawingNode& first, const DrawingNode& second) {
    if (first.user_locked != second.user_locked) {
        return first.user_locked ? first.id : second.id;
    }
    if (first.area != second.area) {
        return first.area > second.area ? first.id : second.id;
    }
    if (first.created_frame != second.created_frame) {
        return first.created_frame < second.created_frame ? first.id : second.id;
    }
    return first.id < second.id ? first.id : second.id;
}

static bool SweepGraphDuplicates(WhiteboardGroup& group,
                                 int current_frame,
                                 int dedupe_interval_frames,
                                 bool duplicate_debug_mode,
                                 float merge_search_radius_px,
                                 float duplicate_pos_overlap_threshold,
                                 float duplicate_centroid_iou_threshold,
                                 float duplicate_bbox_iou_threshold,
                                 float duplicate_max_shape_difference) {
    if (dedupe_interval_frames <= 0 || ((current_frame + 1) % dedupe_interval_frames) != 0) {
        return false;
    }
    if (group.nodes.size() < 2) return false;

    std::vector<int> node_ids;
    node_ids.reserve(group.nodes.size());
    for (const auto& [nid, node_ptr] : group.nodes) {
        if (!IsGhostNode(*node_ptr)) node_ids.push_back(nid);
    }
    std::sort(node_ids.begin(), node_ids.end());

    std::unordered_set<int> removed_ids;
    bool changed = false;

    for (int nid : node_ids) {
        if (removed_ids.count(nid) || group.user_deleted_ids.count(nid)) continue;
        auto it = group.nodes.find(nid);
        if (it == group.nodes.end()) continue;

        const DrawingNode& node = *it->second;
        if (IsGhostNode(node)) continue;
        const float search_r = std::max({(float)node.bbox_canvas.width,
                                         (float)node.bbox_canvas.height,
                                         merge_search_radius_px});
        const auto nearby = group.spatial_index.QueryRadius(node.centroid_canvas, search_r);

        for (int other_id : nearby) {
            if (other_id == nid || other_id < nid) continue;
            if (removed_ids.count(other_id) || group.user_deleted_ids.count(other_id)) continue;

            auto other_it = group.nodes.find(other_id);
            if (other_it == group.nodes.end()) continue;

            const DrawingNode& other = *other_it->second;
            if (IsGhostNode(other)) continue;
            const DuplicateCandidateView node_candidate{
                node.bbox_canvas, &node.binary_mask, &node.contour, node.centroid_canvas, node.hu,
                node.created_frame};
            const DuplicateCandidateView other_candidate{
                other.bbox_canvas, &other.binary_mask, &other.contour, other.centroid_canvas, other.hu,
                other.created_frame};
            const DuplicateCheckResult duplicate_check = EvaluateDuplicateCandidate(
                node_candidate,
                other_candidate,
                duplicate_pos_overlap_threshold,
                duplicate_centroid_iou_threshold,
                duplicate_bbox_iou_threshold,
                duplicate_max_shape_difference);
            const DuplicateCheckResult filtered_duplicate_check =
                SuppressDuplicateForHardEdge(group, nid, other_id, duplicate_check);
            if (!filtered_duplicate_check.is_duplicate) {
                continue;
            }

            const int keep_id = SelectDuplicateWinnerId(node, other);
            const int remove_id = keep_id == nid ? other_id : nid;
            if (duplicate_debug_mode) {
                auto remove_it = group.nodes.find(remove_id);
                if (remove_it != group.nodes.end()) {
                    changed = ConvertNodeToGhost(
                        group, *remove_it->second, keep_id, filtered_duplicate_check) || changed;
                }
                if (remove_id == nid) break;
                continue;
            }
            TransferHardEdges(group, keep_id, remove_id);
            removed_ids.insert(remove_id);
            changed = true;
            if (remove_id == nid) break;
        }
    }

    for (int remove_id : removed_ids) {
        RemoveHardEdges(group, remove_id);
        auto remove_it = group.nodes.find(remove_id);
        if (remove_it == group.nodes.end()) continue;
        group.spatial_index.Remove(remove_id, remove_it->second->centroid_canvas);
        group.nodes.erase(remove_it);
    }
    return changed;
}

static bool RenderOverviewToFrame(const cv::Mat& cache, cv::Size vs, cv::Mat& out) {
    if (cache.empty() || vs.width <= 0 || vs.height <= 0) return false;
    out = cv::Mat(vs.height, vs.width, CV_8UC3, cv::Scalar(255, 255, 255));
    float sa = (float)cache.cols / (float)std::max(1, cache.rows);
    float da = (float)vs.width   / (float)std::max(1, vs.height);
    int dw = vs.width, dh = vs.height;
    if (sa > da) dh = std::max(1, (int)std::round(dw / sa));
    else         dw = std::max(1, (int)std::round(dh * sa));
    cv::Mat scaled;
    cv::resize(cache, scaled, cv::Size(dw, dh), 0, 0, cv::INTER_AREA);
    int ox = (vs.width - dw) / 2, oy = (vs.height - dh) / 2;
    scaled.copyTo(out(cv::Rect(ox, oy, dw, dh)));
    return true;
}

static bool CopyBgrFrameToRgbaBuffer(const cv::Mat& bgr, uint8_t* buf, int w, int h) {
    if (bgr.empty() || !buf || w <= 0 || h <= 0) return false;
    cv::Mat rgba; cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    cv::Mat view(h, w, CV_8UC4, buf);
    rgba.copyTo(view);
    return true;
}

} // namespace

cv::Mat WhiteboardCanvas::BuildBinaryMask(const cv::Mat& gray, const cv::Mat& no_update_mask,
                                           int& stroke_pixel_count) {
    cv::Mat small_gray;
    cv::resize(gray, small_gray, cv::Size(), 0.5, 0.5, cv::INTER_AREA);
    cv::Mat small_binary;
    cv::adaptiveThreshold(small_gray, small_binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, kBinarizeBlockSize, kBinarizeOffset);
    {
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(small_binary, labels, stats, centroids);
        for (int i = 1; i < n; i++) {
            if (stats.at<int>(i, cv::CC_STAT_AREA) < kBinarizeMinBlobArea)
                small_binary.setTo(0, labels == i);
        }
    }
    cv::Mat binary;
    cv::resize(small_binary, binary, gray.size(), 0, 0, cv::INTER_NEAREST);
    binary.setTo(0, no_update_mask);
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT,
                         cv::Size(kDilationKernelSize, kDilationKernelSize));
    cv::dilate(binary, binary, kernel);
    stroke_pixel_count = cv::countNonZero(binary);
    return binary;
}

// ============================================================================
//  SECTION 3: Constructor / Destructor
// ============================================================================

WhiteboardCanvas::WhiteboardCanvas() {
    stop_worker_ = false;
    duplicate_debug_mode_ = g_duplicate_debug_mode.load();

    if (!IsWhiteboardCanvasHelperProcess()) {
        auto client = std::make_unique<WhiteboardCanvasHelperClient>();
        if (client && client->Start()) {
            helper_client_ = std::move(client);
            remote_process_ = true;
            helper_client_->SetCanvasViewMode(canvas_view_mode_.load());
            helper_client_->SetRenderMode(GetRenderMode());
            SyncRuntimeSettings();
            return;
        }
    }

    worker_thread_ = std::thread(&WhiteboardCanvas::WorkerLoop, this);
}

WhiteboardCanvas::~WhiteboardCanvas() {
    if (remote_process_) {
        if (helper_client_) { helper_client_->Stop(); helper_client_.reset(); }
        return;
    }
    stop_worker_ = true;
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
}

void WhiteboardCanvas::SyncRuntimeSettings() {
    if (remote_process_ && helper_client_) {
        helper_client_->SyncSettings(g_whiteboard_debug.load(),
                                     g_duplicate_debug_mode.load(),
                                     g_canvas_enhance_threshold.load(),
                                     g_yolo_fps.load());
    }
}

void WhiteboardCanvas::SetDuplicateDebugMode(bool enabled) {
    duplicate_debug_mode_ = enabled;
    if (remote_process_ && helper_client_) {
        SyncRuntimeSettings();
        return;
    }

    if (enabled) return;

    std::lock_guard<std::mutex> lock(state_mutex_);
    bool changed = false;
    for (auto& group : groups_) {
        if (!group) continue;
        if (!RemoveDuplicateGhostNodes(*group)) continue;
        UpdateGroupBounds(*group);
        group->stroke_cache_dirty = true;
        group->raw_cache_dirty = true;
        changed = true;
    }
    if (changed) BumpCanvasVersion();
}

bool WhiteboardCanvas::IsDuplicateDebugMode() const {
    return duplicate_debug_mode_;
}

void WhiteboardCanvas::InvalidateRenderCaches() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& g : groups_) { g->stroke_cache_dirty = true; g->raw_cache_dirty = true; }
    BumpCanvasVersion();
}

bool WhiteboardCanvas::EnsureRenderCacheReady(WhiteboardGroup& group,
                                               CanvasRenderMode mode) {
    if (mode == CanvasRenderMode::kRaw) {
        if (group.raw_cache_dirty) { RebuildRawRenderCache(group); group.raw_cache_dirty = false; }
    } else {
        if (group.stroke_cache_dirty) { RebuildStrokeRenderCache(group); group.stroke_cache_dirty = false; }
    }
    return !GetRenderCacheForMode(group, mode).empty();
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
    if (person_mask.empty() || person_mask.size() != frame.size() ||
        person_mask.type() != CV_8UC1) return;

    CanvasWorkItem item;
    frame.copyTo(item.frame);
    person_mask.copyTo(item.person_mask);

    std::unique_lock<std::mutex> lock(queue_mutex_);
    pending_item_ = std::move(item);
    lock.unlock();
    queue_cv_.notify_one();
}

bool WhiteboardCanvas::GetViewport(float panX, float panY, float zoom,
                                    cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_)
        return helper_client_->GetViewport(panX, panY, zoom, viewSize, out_frame);
    if (viewSize.width <= 0 || viewSize.height <= 0) return false;

    std::unique_lock<std::mutex> lock(state_mutex_);
    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];
    const CanvasRenderMode mode = GetRenderMode();
    if (!EnsureRenderCacheReady(group, mode)) return false;
    const cv::Mat& cache = GetRenderCacheForMode(group, mode);
    zoom = std::max(1.0f, zoom);
    int cw = cache.cols, ch = cache.rows;
    float va = (float)viewSize.width / (float)viewSize.height;
    float rh = (float)ch / zoom, rw = rh * va;
    if (rw > cw) { rw = (float)cw; rh = rw / va; }
    if (rh > ch) { rh = (float)ch; rw = rh * va; }
    float mcx = cw - rw, mcy = ch - rh;
    float cx = std::max(0.f, std::min(panX * mcx, mcx));
    float cy = std::max(0.f, std::min(panY * mcy, mcy));
    cv::Rect roi((int)cx, (int)cy, (int)rw, (int)rh);
    if (roi.x + roi.width  > cw) roi.width  = cw - roi.x;
    if (roi.y + roi.height > ch) roi.height = ch - roi.y;
    cv::resize(cache(roi), out_frame, viewSize, 0, 0, cv::INTER_LINEAR);
    return true;
}

bool WhiteboardCanvas::GetOverview(cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_)
        return helper_client_->GetOverview(viewSize, out_frame);
    std::unique_lock<std::mutex> lock(state_mutex_);
    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];
    const CanvasRenderMode mode = GetRenderMode();
    if (!EnsureRenderCacheReady(group, mode)) return false;
    return RenderOverviewToFrame(GetRenderCacheForMode(group, mode), viewSize, out_frame);
}

bool WhiteboardCanvas::GetOverviewBlocking(cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_)
        return helper_client_->GetOverview(viewSize, out_frame);
    if (viewSize.width <= 0 || viewSize.height <= 0) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx < 0 || idx >= (int)groups_.size()) return false;
    auto& group = *groups_[idx];
    const CanvasRenderMode mode = GetRenderMode();
    if (!EnsureRenderCacheReady(group, mode)) return false;
    return RenderOverviewToFrame(GetRenderCacheForMode(group, mode), viewSize, out_frame);
}

void WhiteboardCanvas::Reset() {
    if (remote_process_ && helper_client_) {
        helper_client_->Reset();
        has_content_ = false;
        return;
    }
    {
        std::lock_guard<std::mutex> q(queue_mutex_);
        pending_item_.reset();
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    groups_.clear();
    active_group_idx_ = -1;
    view_group_idx_   = -1;
    prev_gray_ = cv::Mat();
    has_content_ = false;
    BumpCanvasVersion();
    frame_w_ = frame_h_ = 0;
    processed_frame_id_ = 0;
}

bool WhiteboardCanvas::HasContent() const {
    if (remote_process_ && helper_client_) return helper_client_->HasContent();
    return has_content_.load();
}

bool WhiteboardCanvas::IsCanvasViewMode() const {
    if (remote_process_ && helper_client_) return helper_client_->IsCanvasViewMode();
    return canvas_view_mode_.load();
}

void WhiteboardCanvas::SetCanvasViewMode(bool m) {
    bool was = canvas_view_mode_.load();
    canvas_view_mode_ = m;
    if (remote_process_ && helper_client_) { helper_client_->SetCanvasViewMode(m); return; }
    if (m && !was) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        view_group_idx_ = active_group_idx_;
    }
    canvas_view_mode_.store(m);
}

void WhiteboardCanvas::SetRenderMode(CanvasRenderMode mode) {
    render_mode_.store(static_cast<int>(mode), std::memory_order_relaxed);
    if (remote_process_ && helper_client_) helper_client_->SetRenderMode(mode);
}

CanvasRenderMode WhiteboardCanvas::GetRenderMode() const {
    return render_mode_.load(std::memory_order_relaxed) ==
               static_cast<int>(CanvasRenderMode::kRaw)
        ? CanvasRenderMode::kRaw : CanvasRenderMode::kStroke;
}

bool WhiteboardCanvas::IsRemoteProcess() const {
    return remote_process_ && helper_client_ && helper_client_->IsReady();
}

cv::Size WhiteboardCanvas::GetCanvasSize() const {
    if (remote_process_ && helper_client_) return helper_client_->GetCanvasSize();
    std::lock_guard<std::mutex> lock(state_mutex_);
    int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (idx >= 0 && idx < (int)groups_.size()) {
        const auto& g = *groups_[idx];
        const auto& cache = GetRenderCacheForMode(g, GetRenderMode());
        if (!cache.empty()) return cv::Size(cache.cols, cache.rows);
        int mnx, mny, mxx, mxy;
        GetRenderBoundsForMode(g, GetRenderMode(), mnx, mny, mxx, mxy);
        return cv::Size(std::max(1, mxx - mnx), std::max(1, mxy - mny));
    }
    return cv::Size(frame_w_ > 0 ? frame_w_ : kDefaultCanvasWidth,
                    frame_h_ > 0 ? frame_h_ : kDefaultCanvasHeight);
}

int WhiteboardCanvas::GetSubCanvasCount() const {
    if (remote_process_ && helper_client_) return helper_client_->GetSubCanvasCount();
    std::lock_guard<std::mutex> lock(state_mutex_);
    return (int)groups_.size();
}

int WhiteboardCanvas::GetActiveSubCanvasIndex() const {
    if (remote_process_ && helper_client_) return helper_client_->GetActiveSubCanvasIndex();
    std::lock_guard<std::mutex> lock(state_mutex_);
    return canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
}

void WhiteboardCanvas::SetActiveSubCanvas(int idx) {
    if (remote_process_ && helper_client_) { helper_client_->SetActiveSubCanvas(idx); return; }
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (idx >= 0 && idx < (int)groups_.size()) {
        if (canvas_view_mode_.load()) view_group_idx_ = idx;
        else active_group_idx_ = idx;
    }
}

int WhiteboardCanvas::GetSortedSubCanvasIndex(int pos) const {
    if (remote_process_ && helper_client_) return helper_client_->GetSortedSubCanvasIndex(pos);
    std::lock_guard<std::mutex> lock(state_mutex_);
    return (pos >= 0 && pos < (int)groups_.size()) ? pos : -1;
}

int WhiteboardCanvas::GetSortedPosition(int idx) const {
    if (remote_process_ && helper_client_) return helper_client_->GetSortedPosition(idx);
    std::lock_guard<std::mutex> lock(state_mutex_);
    return (idx >= 0 && idx < (int)groups_.size()) ? idx : -1;
}

// ============================================================================
//  SECTION 5: Worker thread
// ============================================================================

void WhiteboardCanvas::WorkerLoop() {
    while (true) {
        CanvasWorkItem item;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stop_worker_.load() || pending_item_.has_value(); });
            if (stop_worker_.load() && !pending_item_.has_value()) break;
            item = std::move(*pending_item_);
            pending_item_.reset();
        }
        try {
            ProcessFrameInternal(item.frame, item.person_mask);
        } catch (const cv::Exception& e) {
            OutputDebugStringA((std::string("[WhiteboardCanvas] CV: ") + e.what() + "\n").c_str());
        } catch (...) {
            OutputDebugStringA("[WhiteboardCanvas] Unknown exception\n");
        }
    }
}

// ============================================================================
//  SECTION 6: Motion gate
// ============================================================================

bool WhiteboardCanvas::ApplyMotionGate(const cv::Mat& gray, float& motion_fraction,
                                        bool& motion_too_high) {
    motion_fraction = 0.0f;
    motion_too_high = false;

    cv::Mat mg;
    float scale = ComputeScaleForLongEdge(gray.size(), kMotionLongEdge);
    if (scale < 0.999f) cv::resize(gray, mg, cv::Size(), scale, scale, cv::INTER_AREA);
    else mg = gray;

    const bool has_prev_frame = !prev_gray_.empty() && prev_gray_.size() == mg.size();
    if (has_prev_frame) {
        cv::Mat diff;
        cv::absdiff(mg, prev_gray_, diff);
        cv::threshold(diff, diff, kMotionPixelThreshold, 255, cv::THRESH_BINARY);
        motion_fraction = (float)cv::countNonZero(diff) / (float)std::max<size_t>(1, diff.total());
        motion_too_high = motion_fraction > kMaxMotionFraction;
    } else {
        motion_gate_locked_ = false;
    }

    bool skip_frame = false;
    if (kEnableMotionGate && has_prev_frame) {
        if (motion_gate_locked_) {
            skip_frame = true;
            if (motion_fraction > kOpenMotionGateLockFraction) {
                motion_gate_locked_ = false;
            }
        } else if (motion_too_high) {
            skip_frame = true;
        } else {
            motion_gate_locked_ = true;
        }
    }

    mg.copyTo(prev_gray_);
    return skip_frame;
}

// ============================================================================
//  SECTION 7: Main pipeline
// ============================================================================

void WhiteboardCanvas::ProcessFrameInternal(const cv::Mat& uncut_frame,
                                             const cv::Mat& person_mask) {
    const int current_frame = processed_frame_id_++;

    const cv::Rect roi = ComputeProcessingRoi(uncut_frame.size());
    if (roi.width <= 0 || roi.height <= 0) return;

    cv::Mat frame = uncut_frame(roi);
    cv::Mat cropped_mask = CropPersonMaskForProcessing(person_mask, roi);

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    cv::Mat no_update_mask = BuildNoUpdateMask(gray, cropped_mask);
    const cv::Rect lecturer_rect = ComputeMaskBoundingRect(cropped_mask);

    cv::Mat reject_mask;
    if (kEnableFrameStrokeRejectFilter)
        reject_mask = BuildFrameStrokeRejectMask(frame.size(), lecturer_rect);

    // [1] Motion gate
    float mf = 0.0f; bool mth = false;
    if (ApplyMotionGate(gray, mf, mth)) return;

    // [2] Binarize
    int stroke_px = 0;
    cv::Mat binary = BuildBinaryMask(gray, no_update_mask, stroke_px);

    // [3] Extract blobs
    std::vector<FrameBlob> blobs = ExtractFrameBlobs(binary, frame);
    EnhanceFrameBlobs(blobs, frame, g_canvas_enhance_threshold.load());

    if (kEnableFrameStrokeRejectFilter && !reject_mask.empty())
        FilterBlobsForCanvas(blobs, reject_mask, kFrameStrokeRejectMinWidth);

    std::lock_guard<std::mutex> state_lock(state_mutex_);

    auto recompute_has_content = [&]() {
        for (const auto& gp : groups_) if (gp && !gp->nodes.empty()) { has_content_ = true; return; }
        has_content_ = false;
    };

    if (frame_w_ == 0) { frame_w_ = frame.cols; frame_h_ = frame.rows; }

    const bool has_active = active_group_idx_ >= 0 &&
                            active_group_idx_ < (int)groups_.size();
    int active_nodes = 0;
    if (has_active) {
        for (const auto& [_, node_ptr] : groups_[active_group_idx_]->nodes) {
            if (!IsGhostNode(*node_ptr)) active_nodes++;
        }
    }
    const bool graph_ready = has_active && active_nodes >= kStableGraphNodeThreshold;

    // [4] Match blobs to graph (total-shape comparison, no camera state)
    cv::Point2f frame_offset(0, 0);
    if (graph_ready && !blobs.empty()) {
        auto& group = *groups_[active_group_idx_];
        frame_offset = MatchBlobsToGraph(group, blobs);
    }

    // [5] Update graph or bootstrap
    if (!has_active) {
        if (!mth && stroke_px >= kMinStrokePixelsForNewSC && !blobs.empty()) {
            CreateSubCanvas(frame, binary, blobs, current_frame);
            recompute_has_content();
        }
    } else if (!graph_ready) {
        auto& group = *groups_[active_group_idx_];
        if (!mth && !blobs.empty()) {
            SeedGroupFromFrameBlobs(group, blobs, current_frame);
            recompute_has_content();
        }
    } else {
        auto& group = *groups_[active_group_idx_];
        const cv::Rect lecturer_canvas =
            TranslateFrameRectToCanvas(lecturer_rect, frame_offset);
        if (UpdateGraph(group, blobs, current_frame, frame_offset, lecturer_canvas))
            recompute_has_content();
    }

    if (groups_.empty() && !mth && stroke_px >= kMinStrokePixelsForNewSC && !blobs.empty()) {
        CreateSubCanvas(frame, binary, blobs, current_frame);
        recompute_has_content();
    }
}

// ============================================================================
//  SECTION 8: Frame Blob Extraction
// ============================================================================

std::vector<FrameBlob> WhiteboardCanvas::ExtractFrameBlobs(const cv::Mat& binary,
                                                            const cv::Mat& frame_bgr) const {
    std::vector<FrameBlob> result;
    if (binary.empty()) return result;

    cv::Mat labels, stats, centroids;
    int nlabels = cv::connectedComponentsWithStats(binary, labels, stats, centroids);
    if (nlabels <= 1) return result;

    struct SC { int label; cv::Rect bbox; cv::Point2f centroid; double area;
                std::vector<cv::Point> contour; };
    std::vector<SC> components;
    components.reserve(nlabels - 1);

    for (int i = 1; i < nlabels; i++) {
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < kMinContourArea) continue;
        int bx = std::max(0, stats.at<int>(i, cv::CC_STAT_LEFT));
        int by = std::max(0, stats.at<int>(i, cv::CC_STAT_TOP));
        int bw = std::min(stats.at<int>(i, cv::CC_STAT_WIDTH),  binary.cols - bx);
        int bh = std::min(stats.at<int>(i, cv::CC_STAT_HEIGHT), binary.rows - by);
        if (bw <= 0 || bh <= 0) continue;
        cv::Rect bbox(bx, by, bw, bh);
        cv::Mat cc_mask = (labels(bbox) == i);
        cv::Point2f lc = ComputeGravityCenter(cc_mask);
        cv::Point2f gc(lc.x + bx, lc.y + by);
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(cc_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
        if (contours.empty()) continue;
        int best_ci = 0; double best_ca = 0;
        for (int ci = 0; ci < (int)contours.size(); ci++) {
            double ca = cv::contourArea(contours[ci]);
            if (ca > best_ca) { best_ca = ca; best_ci = ci; }
        }
        components.push_back({i, bbox, gc, best_ca, contours[best_ci]});
    }
    if (components.empty()) return result;

    for (const auto& component : components) {
        cv::Rect g_bbox = component.bbox;

        // Skip very elongated blobs (whiteboard edge lines)
        float le = (float)std::max(g_bbox.width, g_bbox.height);
        float se = (float)std::min(g_bbox.width, g_bbox.height);
        if (se > 0 && le / se > kMaxAllowedRectangle) continue;

        // Skip blobs that span most of the frame (whiteboard edges / borders)
        if (frame_h_ > 0) {
            float max_dim = frame_h_ * kMaxBlobDimensionFraction;
            if (g_bbox.width > max_dim || g_bbox.height > max_dim) continue;
        }

        cv::Mat g_mask = cv::Mat::zeros(g_bbox.size(), CV_8UC1);
        cv::Mat cc_mask = (labels(component.bbox) == component.label);
        cc_mask.copyTo(g_mask, cc_mask);

        FrameBlob blob;
        blob.bbox = g_bbox;
        blob.binary_mask = g_mask;
        cv::Point2f lc = ComputeGravityCenter(g_mask);
        blob.centroid = lc + cv::Point2f((float)g_bbox.x, (float)g_bbox.y);
        blob.contour = component.contour;
        blob.area = component.area;
        cv::Moments m = cv::moments(blob.contour);
        cv::HuMoments(m, blob.hu);
        if (!frame_bgr.empty()) blob.color_pixels = frame_bgr(g_bbox).clone();
        result.push_back(std::move(blob));
    }
    return result;
}

// ============================================================================
//  SECTION 9: Graph Matching (3-step: GlobalShape → ShapeMatch → ShapeMatch)
// ============================================================================

// ---------------------------------------------------------------------------
// Step 1: Global shape pass — rough offset via total-shape matching + median vote
// ---------------------------------------------------------------------------
cv::Point2f WhiteboardCanvas::GlobalShapePass(WhiteboardGroup& group,
                                               const std::vector<FrameBlob>& blobs) {
    struct ShapeCandidate { cv::Point2f offset; float difference; };
    std::vector<ShapeCandidate> offset_vectors;

    for (const auto& blob : blobs) {
        // Search ALL canvas nodes — blob is in frame space, canvas nodes can be
        // anywhere, so spatial index queries around frame centroid won't work.
        int best_node = -1;
        float best_difference = kGlobalShapeMaxDifference;
        cv::Point2f best_offset;

        for (const auto& pair : group.nodes) {
            const auto& node = *pair.second;
            if (IsGhostNode(node)) continue;

            // Area ratio filter
            if (node.area > 0.0) {
                float ratio = (float)(blob.area / node.area);
                if (ratio < kAreaRatioMin || ratio > (1.0f / kAreaRatioMin)) continue;
            }

            const cv::Rect aligned_node_bbox = AlignRectToCentroid(
                node.bbox_canvas, node.centroid_canvas, blob.centroid);
            const TotalShapeCompareResult shape_compare = TotalShapeCompare(
                blob.bbox,
                &blob.binary_mask,
                blob.hu,
                &blob.contour,
                aligned_node_bbox,
                &node.binary_mask,
                node.hu,
                &node.contour);
            if (!shape_compare.valid) continue;

            if (shape_compare.difference < best_difference) {
                best_difference = shape_compare.difference;
                best_node = node.id;
                best_offset = node.centroid_canvas - blob.centroid;
            }
        }

        if (best_node >= 0) {
            offset_vectors.push_back({best_offset, best_difference});
        }
    }

    if (offset_vectors.empty()) return {};

    // Median vote for rough offset
    std::vector<float> dxs, dys;
    dxs.reserve(offset_vectors.size());
    dys.reserve(offset_vectors.size());
    for (const auto& ov : offset_vectors) {
        dxs.push_back(ov.offset.x);
        dys.push_back(ov.offset.y);
    }
    std::sort(dxs.begin(), dxs.end());
    std::sort(dys.begin(), dys.end());
    return cv::Point2f(dxs[dxs.size() / 2], dys[dys.size() / 2]);
}

// ---------------------------------------------------------------------------
// MatchBlobsToGraph — orchestrates all 3 steps
// ---------------------------------------------------------------------------
cv::Point2f WhiteboardCanvas::MatchBlobsToGraph(WhiteboardGroup& group,
                                                  std::vector<FrameBlob>& blobs) {
    if (blobs.empty() || group.nodes.empty()) return {};

    for (auto& b : blobs) { b.matched_node_id = -1; b.matched_offset = {}; }

    struct ShapeMatchCandidate {
        int node_id = -1;
        cv::Point2f delta_vec;
        float score = -1.0f;
    };

    auto find_best_shape_match = [&](const FrameBlob& blob,
                                     const cv::Point2f& offset,
                                     float search_radius,
                                     float min_score,
                                     const std::unordered_set<int>& excluded_nodes) {
        ShapeMatchCandidate best_match;
        float best_distance = search_radius;

        const cv::Point2f canvas_centroid = blob.centroid + offset;
        const cv::Rect canvas_bbox(
            blob.bbox.x + (int)std::round(offset.x),
            blob.bbox.y + (int)std::round(offset.y),
            blob.bbox.width,
            blob.bbox.height);

        const auto nearby = group.spatial_index.QueryRadius(canvas_centroid, search_radius);
        for (int nid : nearby) {
            if (excluded_nodes.count(nid)) continue;
            auto nit = group.nodes.find(nid);
            if (nit == group.nodes.end()) continue;
            const auto& node = *nit->second;
            if (IsGhostNode(node)) continue;

            if (node.area > 0.0) {
                float ratio = (float)(blob.area / node.area);
                if (ratio < kAreaRatioMin || ratio > (1.0f / kAreaRatioMin)) continue;
            }

            const cv::Rect aligned_node_bbox = AlignRectToCentroid(
                node.bbox_canvas, node.centroid_canvas, canvas_centroid);

            const TotalShapeCompareResult shape_compare = TotalShapeCompare(
                canvas_bbox,
                &blob.binary_mask,
                blob.hu,
                &blob.contour,
                aligned_node_bbox,
                &node.binary_mask,
                node.hu,
                &node.contour);
            if (!shape_compare.valid) continue;

            const float similarity = 1.0f - shape_compare.difference;
            if (similarity < min_score) continue;

            const float centroid_distance =
                (float)cv::norm(canvas_centroid - node.centroid_canvas);
            if (similarity > best_match.score ||
                (std::abs(similarity - best_match.score) < 1e-6f &&
                 centroid_distance < best_distance)) {
                best_match.node_id = nid;
                best_match.delta_vec = node.centroid_canvas - canvas_centroid;
                best_match.score = similarity;
                best_distance = centroid_distance;
            }
        }

        return best_match;
    };

    // =====================================================================
    // Step 1: Global shape pass — rough offset
    // =====================================================================
    cv::Point2f rough_offset = GlobalShapePass(group, blobs);

    // =====================================================================
    // Step 2: Shape matching — shift by rough offset, match with total-shape score,
    //         global prune, per-third prune → partition-specific offsets
    // =====================================================================
    struct ShapeMatch {
        int blob_idx;
        int node_id;
        cv::Point2f delta_vec;
        float score;
        int partition_idx;
    };
    std::vector<ShapeMatch> shape_matches;
    std::unordered_set<int> matched_nodes_step2;

    for (int i = 0; i < (int)blobs.size(); i++) {
        const auto& blob = blobs[i];
        const int partition_idx = GetHorizontalMatchPartition(
            blob.centroid.x, frame_w_, kHorizontalMatchPartitions);

        const ShapeMatchCandidate best_match = find_best_shape_match(
            blob, rough_offset, kShapeMatchSearchRadius, kShapeMatchMinScore,
            matched_nodes_step2);

        if (best_match.node_id >= 0) {
            shape_matches.push_back(
                {i, best_match.node_id, best_match.delta_vec, best_match.score, partition_idx});
            matched_nodes_step2.insert(best_match.node_id);
        }
    }

    if (shape_matches.empty()) return rough_offset;

    const cv::Point2f global_mean_delta = ComputeMeanDeltaVector(shape_matches);
    std::vector<ShapeMatch> global_inlier_matches = PruneMatchesAroundMean(
        shape_matches, global_mean_delta, kOutlierVectorThreshold);
    if (global_inlier_matches.empty())
        global_inlier_matches = shape_matches;

    const cv::Point2f precise_delta = ComputeMeanDeltaVector(global_inlier_matches);
    const cv::Point2f precise_offset = rough_offset + precise_delta;

    std::array<cv::Point2f, kHorizontalMatchPartitions> partition_offsets;
    partition_offsets.fill(precise_offset);

    for (int partition_idx = 0; partition_idx < kHorizontalMatchPartitions; partition_idx++) {
        std::vector<ShapeMatch> partition_matches;
        for (const auto& match : global_inlier_matches) {
            if (match.partition_idx == partition_idx)
                partition_matches.push_back(match);
        }
        if (partition_matches.empty()) continue;

        const cv::Point2f partition_mean_delta = ComputeMeanDeltaVector(partition_matches);
        std::vector<ShapeMatch> partition_inliers = PruneMatchesAroundMean(
            partition_matches, partition_mean_delta, kPartitionOutlierVectorThreshold);
        if (partition_inliers.empty())
            partition_inliers = partition_matches;

        partition_offsets[partition_idx] =
            rough_offset + ComputeMeanDeltaVector(partition_inliers);
    }

    for (auto& blob : blobs) {
        const int partition_idx = GetHorizontalMatchPartition(
            blob.centroid.x, frame_w_, kHorizontalMatchPartitions);
        blob.matched_offset = partition_offsets[partition_idx];
    }

    // =====================================================================
    // Step 3: Final shape-matching refinement with tighter radius
    // =====================================================================
    std::unordered_set<int> claimed_nodes;

    for (int i = 0; i < (int)blobs.size(); i++) {
        auto& blob = blobs[i];
        const ShapeMatchCandidate best_match = find_best_shape_match(
            blob, blob.matched_offset, kFinalShapeMatchSearchRadius,
            kFinalShapeMatchMinScore, claimed_nodes);
        if (best_match.node_id >= 0) {
            blob.matched_node_id = best_match.node_id;
            claimed_nodes.insert(best_match.node_id);
        }
    }

    return precise_offset;
}

// ============================================================================
//  SECTION 10: Graph Update
// ============================================================================

void WhiteboardCanvas::RemoveNodeFromGraph(WhiteboardGroup& group, int node_id) {
    auto it = group.nodes.find(node_id);
    if (it == group.nodes.end()) return;
    RemoveHardEdges(group, node_id);
    group.spatial_index.Remove(node_id, it->second->centroid_canvas);
    group.nodes.erase(it);
}

bool WhiteboardCanvas::UpdateGraph(WhiteboardGroup& group,
                                    std::vector<FrameBlob>& blobs,
                                    int current_frame, cv::Point2f frame_offset,
                                    const cv::Rect& lecturer_canvas_rect) {
    bool graph_changed = false;
    const cv::Rect cropped_frame = BuildCroppedFrameRect(frame_w_, frame_h_);
    const cv::Size frame_size(frame_w_, frame_h_);
    const int absence_frame_inset_px = kAbsenceFrameInsetPx;
    const int absence_lecturer_expansion_px = kAbsenceLecturerExpansionPx;
    const float absence_visible_fraction_min = kAbsenceVisibleFractionMin;
    const float absence_lecturer_overlap_min = kAbsenceLecturerOverlapMin;

    // --- 4a. Process matched nodes using replacement mode ---
    std::unordered_set<int> seen_node_ids;
    std::vector<int> seen_node_list;
    std::unordered_map<int, cv::Point2f> node_deltas;  // id -> (dx, dy)
    for (auto& blob : blobs) {
        if (blob.matched_node_id < 0) continue;
        auto nit = group.nodes.find(blob.matched_node_id);
        if (nit == group.nodes.end()) continue;
        auto& node = *nit->second;

        const cv::Point2f canvas_centroid = blob.centroid + blob.matched_offset;
        const cv::Rect canvas_bbox(
            blob.bbox.x + (int)std::round(blob.matched_offset.x),
            blob.bbox.y + (int)std::round(blob.matched_offset.y),
            blob.bbox.width, blob.bbox.height);

        node.last_seen_frame = current_frame;
        if (seen_node_ids.insert(node.id).second)
            seen_node_list.push_back(node.id);

        // Update absence score (node is healthy)
        node.absence_score = std::min(kAbsenceScoreMax, node.absence_score + kAbsenceIncrement);
        if (UpdateMainCanvasVisibilityFromAbsence(node)) {
            graph_changed = true;
        }
        node.match_count++;

        const cv::Point2f old_centroid = node.centroid_canvas;

        if (!node.user_locked) {
            switch (kReplacementMode) {
            case NodeReplacementMode::kAlwaysReplace: {
                cv::Point2f blended_centroid(
                    node.centroid_canvas.x * (1.0f - kLocationAverageAlpha) +
                        canvas_centroid.x * kLocationAverageAlpha,
                    node.centroid_canvas.y * (1.0f - kLocationAverageAlpha) +
                        canvas_centroid.y * kLocationAverageAlpha);
                int dx = (int)std::round(blended_centroid.x - canvas_centroid.x);
                int dy = (int)std::round(blended_centroid.y - canvas_centroid.y);
                cv::Rect blended_bbox(
                    canvas_bbox.x + dx,
                    canvas_bbox.y + dy,
                    canvas_bbox.width,
                    canvas_bbox.height);
                RefreshNodeFromBlob(group, node, blob, blended_centroid, blended_bbox);
                graph_changed = true;
                break;
            }

            case NodeReplacementMode::kIouThreshold: {
                const MaskRelation rel = ComputeMaskRelation(
                    canvas_bbox, blob.binary_mask, canvas_centroid,
                    node.bbox_canvas, node.binary_mask, node.centroid_canvas);
                if (!rel.valid || rel.iou < kIouReplaceThreshold) {
                    RefreshNodeFromBlob(group, node, blob, canvas_centroid, canvas_bbox);
                    graph_changed = true;
                }
                break;
            }

            case NodeReplacementMode::kPeriodicReplace:
                if (node.match_count % kPeriodicReplaceInterval == 0) {
                    RefreshNodeFromBlob(group, node, blob, canvas_centroid, canvas_bbox);
                    graph_changed = true;
                }
                break;

            case NodeReplacementMode::kLocationAverage: {
                // Blend centroid positions, keep existing content
                cv::Point2f new_centroid(
                    node.centroid_canvas.x * (1.0f - kLocationAverageAlpha) +
                        canvas_centroid.x * kLocationAverageAlpha,
                    node.centroid_canvas.y * (1.0f - kLocationAverageAlpha) +
                        canvas_centroid.y * kLocationAverageAlpha);
                int dx = (int)std::round(new_centroid.x - node.centroid_canvas.x);
                int dy = (int)std::round(new_centroid.y - node.centroid_canvas.y);
                if (dx != 0 || dy != 0) {
                    group.spatial_index.Remove(node.id, node.centroid_canvas);
                    node.centroid_canvas = new_centroid;
                    node.bbox_canvas.x += dx;
                    node.bbox_canvas.y += dy;
                    group.spatial_index.Insert(node.id, new_centroid);
                    graph_changed = true;
                }
                break;
            }
            }
        }

        // Track movement delta for hard-edge propagation
        cv::Point2f delta = node.centroid_canvas - old_centroid;
        node_deltas[node.id] = delta;
    }

    if (PruneHardEdgesByDistance(group, kHardEdgeMaxCentroidDist))
        graph_changed = true;

    if (seen_node_list.size() >= 2 &&
        CreateHardEdgesForFrame(group, seen_node_list,
                                kHardEdgeMaxCentroidDist)) {
        graph_changed = true;
    }

    // --- 4a+. Propagate hard-edge deltas to unmatched connected nodes ---
    {
        std::unordered_set<int> propagated;
        for (auto& [nid, delta] : node_deltas) {
            if (propagated.count(nid)) continue;
            auto component = GetHardEdgeComponent(group, nid);
            if (component.empty()) {
                propagated.insert(nid);
                continue;
            }

            std::vector<float> dxs;
            std::vector<float> dys;
            for (int cid : component) {
                auto dit = node_deltas.find(cid);
                if (dit == node_deltas.end()) continue;
                dxs.push_back(dit->second.x);
                dys.push_back(dit->second.y);
            }
            if (dxs.empty()) {
                for (int cid : component) propagated.insert(cid);
                continue;
            }

            for (int cid : component) propagated.insert(cid);
            if (dxs.empty()) continue;

            const float component_dx = ComputeMean(dxs);
            const float component_dy = ComputeMean(dys);
            if (component_dx == 0.0f && component_dy == 0.0f) continue;

            const int idx = (int)std::round(component_dx);
            const int idy = (int)std::round(component_dy);
            for (int cid : component) {
                if (seen_node_ids.count(cid)) continue;
                auto cit = group.nodes.find(cid);
                if (cit == group.nodes.end()) continue;
                auto& cnode = *cit->second;
                group.spatial_index.Remove(cnode.id, cnode.centroid_canvas);
                cnode.centroid_canvas.x += component_dx;
                cnode.centroid_canvas.y += component_dy;
                cnode.bbox_canvas.x += idx;
                cnode.bbox_canvas.y += idy;
                group.spatial_index.Insert(cnode.id, cnode.centroid_canvas);
                graph_changed = true;
            }
        }
    }

    // --- 4b. Absence tracking ---
    // Penalise unseen nodes only when they still project into the current cropped frame,
    // are not materially hidden by the lecturer, and are near a matched node.
    {
        std::vector<int> to_remove;
        for (auto& pair : group.nodes) {
            int nid = pair.first;
            auto& node = *pair.second;
            if (IsGhostNode(node)) continue;
            if (seen_node_ids.count(nid)) continue;

            if (!IsNodePlausiblyVisibleForAbsence(node, frame_offset, cropped_frame, frame_size,
                                                 absence_frame_inset_px,
                                                 absence_visible_fraction_min))
                continue;
            if (IsNodeOccludedByLecturerForAbsence(node, lecturer_canvas_rect, frame_size,
                                                  absence_lecturer_expansion_px,
                                                  absence_lecturer_overlap_min))
                continue;

            bool near_match = false;
            for (int sid : seen_node_ids) {
                auto sit = group.nodes.find(sid);
                if (sit == group.nodes.end()) continue;
                if ((float)cv::norm(node.centroid_canvas - sit->second->centroid_canvas)
                        < kAbsenceNearbyRadius) { near_match = true; break; }
            }
            if (!near_match) continue;

            node.absence_score -= kAbsenceDecrement;
            if (node.absence_score < 0.0f &&
                !node.user_locked)
                to_remove.push_back(nid);
        }
        for (int nid : to_remove) { RemoveNodeFromGraph(group, nid); graph_changed = true; }
    }

    // --- 4c. Dedupe unmatched frame blobs, then add surviving ones as new nodes ---
    const int matched_count = (int)seen_node_ids.size();
    std::vector<int> new_node_ids;
    if (matched_count >= kMinMatchesForNewNode) {
        for (auto& blob : blobs) {
            if (blob.matched_node_id >= 0) continue;

            // Skip blobs whose centroid falls outside the cropped frame region
            if (!cropped_frame.contains(cv::Point((int)blob.centroid.x, (int)blob.centroid.y)))
                continue;

            const cv::Point2f canvas_centroid = blob.centroid + blob.matched_offset;
            const cv::Rect canvas_bbox(
                blob.bbox.x + (int)std::round(blob.matched_offset.x),
                blob.bbox.y + (int)std::round(blob.matched_offset.y),
                blob.bbox.width, blob.bbox.height);

            const int id_before = group.next_node_id;
            if (InsertOrMergeBlobNode(group, blob, canvas_centroid, canvas_bbox, current_frame,
                                      kEnableInsertMergeDeduplication,
                                      duplicate_debug_mode_,
                                      kMergeSearchRadiusPx,
                                      kDuplicatePosOverlapThreshold,
                                      kDuplicateCentroidIouThreshold,
                                      kDuplicateBboxIouThreshold,
                                      kDuplicateMaxShapeDifference)) {
                graph_changed = true;
                // A brand-new node was created (not merged into existing)
                if (group.next_node_id != id_before &&
                    group.nodes.count(id_before) &&
                    !IsGhostNode(*group.nodes.at(id_before)))
                    new_node_ids.push_back(id_before);
            }
        }
        // Create hard edges between new nodes from this frame within distance threshold
        if (new_node_ids.size() >= 2 &&
            CreateHardEdgesForFrame(group, new_node_ids,
                                    kHardEdgeMaxCentroidDist)) {
            graph_changed = true;
        }
    }

    if (SweepGraphDuplicates(group, current_frame,
                             kGraphDedupeIntervalFrames,
                             duplicate_debug_mode_,
                             kMergeSearchRadiusPx,
                             kDuplicatePosOverlapThreshold,
                             kDuplicateCentroidIouThreshold,
                             kDuplicateBboxIouThreshold,
                             kDuplicateMaxShapeDifference)) {
        graph_changed = true;
    }

    if (graph_changed) {
        UpdateGroupBounds(group);
        group.stroke_cache_dirty = true;
        group.raw_cache_dirty    = true;
        BumpCanvasVersion();
    }
    return graph_changed;
}


// ============================================================================
//  SECTION 11: Rendering
// ============================================================================

void WhiteboardCanvas::UpdateGroupBounds(WhiteboardGroup& group) {
    if (group.nodes.empty()) return;
    int smx = INT_MAX, smy = INT_MAX, sMx = INT_MIN, sMy = INT_MIN;
    int rmx = INT_MAX, rmy = INT_MAX, rMx = INT_MIN, rMy = INT_MIN;
    for (const auto& pair : group.nodes) {
        const auto& node = *pair.second;
        const cv::Rect& bb = node.bbox_canvas;
        smx = std::min(smx, bb.x);           smy = std::min(smy, bb.y);
        sMx = std::max(sMx, bb.x + bb.width); sMy = std::max(sMy, bb.y + bb.height);
        if (!node.color_pixels.empty() || node.duplicate_debug_marked) {
            rmx = std::min(rmx, bb.x);           rmy = std::min(rmy, bb.y);
            rMx = std::max(rMx, bb.x + bb.width); rMy = std::max(rMy, bb.y + bb.height);
        }
    }
    if (smx < sMx && smy < sMy) {
        group.stroke_min_px_x = std::min(group.stroke_min_px_x, smx);
        group.stroke_min_px_y = std::min(group.stroke_min_px_y, smy);
        group.stroke_max_px_x = std::max(group.stroke_max_px_x, sMx);
        group.stroke_max_px_y = std::max(group.stroke_max_px_y, sMy);
    }
    if (rmx < rMx && rmy < rMy) {
        group.raw_min_px_x = std::min(group.raw_min_px_x, rmx);
        group.raw_min_px_y = std::min(group.raw_min_px_y, rmy);
        group.raw_max_px_x = std::max(group.raw_max_px_x, rMx);
        group.raw_max_px_y = std::max(group.raw_max_px_y, rMy);
    } else {
        group.raw_min_px_x = std::min(group.raw_min_px_x, group.stroke_min_px_x);
        group.raw_min_px_y = std::min(group.raw_min_px_y, group.stroke_min_px_y);
        group.raw_max_px_x = std::max(group.raw_max_px_x, group.stroke_max_px_x);
        group.raw_max_px_y = std::max(group.raw_max_px_y, group.stroke_max_px_y);
    }
    if (group.fixed_render_height > 0) {
        if (group.stroke_max_px_y - group.stroke_min_px_y < group.fixed_render_height)
            group.stroke_max_px_y = group.stroke_min_px_y + group.fixed_render_height;
        if (group.raw_max_px_y - group.raw_min_px_y < group.fixed_render_height)
            group.raw_max_px_y = group.raw_min_px_y + group.fixed_render_height;
    }
}

void WhiteboardCanvas::RebuildStrokeRenderCache(WhiteboardGroup& group) {
    if (group.nodes.empty()) { group.stroke_render_cache = cv::Mat(); return; }
    int W = std::max(1, group.stroke_max_px_x - group.stroke_min_px_x);
    int H = std::max(1, group.stroke_max_px_y - group.stroke_min_px_y);
    cv::Mat canvas(H, W, CV_8UC3, cv::Scalar(255, 255, 255));

    std::vector<const DrawingNode*> sorted;
    sorted.reserve(group.nodes.size());
    for (const auto& p : group.nodes) {
        if (!IsNodeVisibleInMainCanvas(*p.second)) continue;
        sorted.push_back(p.second.get());
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const DrawingNode* a, const DrawingNode* b) {
                  return a->created_frame < b->created_frame; });

    for (const auto* node : sorted) {
        cv::Rect dst(node->bbox_canvas.x - group.stroke_min_px_x,
                     node->bbox_canvas.y - group.stroke_min_px_y,
                     node->bbox_canvas.width, node->bbox_canvas.height);
        int cx0 = std::max(0, dst.x), cy0 = std::max(0, dst.y);
        int cx1 = std::min(W, dst.x + dst.width), cy1 = std::min(H, dst.y + dst.height);
        if (cx0 >= cx1 || cy0 >= cy1) continue;
        int sx0 = cx0 - dst.x, sy0 = cy0 - dst.y, sw = cx1 - cx0, sh = cy1 - cy0;
        if (sx0+sw > node->binary_mask.cols || sy0+sh > node->binary_mask.rows) continue;
        canvas(cv::Rect(cx0, cy0, sw, sh)).setTo(cv::Scalar(0,0,0),
            node->binary_mask(cv::Rect(sx0, sy0, sw, sh)));
    }
    cv::bitwise_not(canvas, canvas);
    group.stroke_render_cache = canvas;
}

void WhiteboardCanvas::RebuildRawRenderCache(WhiteboardGroup& group) {
    if (group.nodes.empty()) { group.raw_render_cache = cv::Mat(); return; }
    int W = std::max(1, group.raw_max_px_x - group.raw_min_px_x);
    int H = std::max(1, group.raw_max_px_y - group.raw_min_px_y);
    cv::Mat canvas(H, W, CV_8UC3, cv::Scalar(255, 255, 255));

    std::vector<const DrawingNode*> sorted;
    sorted.reserve(group.nodes.size());
    for (const auto& p : group.nodes)
        if (IsNodeVisibleInMainCanvas(*p.second) && !p.second->color_pixels.empty())
            sorted.push_back(p.second.get());
    std::sort(sorted.begin(), sorted.end(),
              [](const DrawingNode* a, const DrawingNode* b) {
                  return a->created_frame < b->created_frame; });

    for (const auto* node : sorted) {
        cv::Rect dst(node->bbox_canvas.x - group.raw_min_px_x,
                     node->bbox_canvas.y - group.raw_min_px_y,
                     node->bbox_canvas.width, node->bbox_canvas.height);
        int cx0 = std::max(0, dst.x), cy0 = std::max(0, dst.y);
        int cx1 = std::min(W, dst.x + dst.width), cy1 = std::min(H, dst.y + dst.height);
        if (cx0 >= cx1 || cy0 >= cy1) continue;
        int sx0 = cx0 - dst.x, sy0 = cy0 - dst.y, sw = cx1 - cx0, sh = cy1 - cy0;
        if (sx0+sw > node->binary_mask.cols  || sy0+sh > node->binary_mask.rows)  continue;
        if (sx0+sw > node->color_pixels.cols || sy0+sh > node->color_pixels.rows) continue;
        node->color_pixels(cv::Rect(sx0,sy0,sw,sh)).copyTo(
            canvas(cv::Rect(cx0,cy0,sw,sh)), node->binary_mask(cv::Rect(sx0,sy0,sw,sh)));
    }
    group.raw_render_cache = canvas;
}

// ============================================================================
//  SECTION 12: Sub-canvas creation
// ============================================================================

void WhiteboardCanvas::SeedGroupFromFrameBlobs(WhiteboardGroup& group,
                                                const std::vector<FrameBlob>& blobs,
                                                int current_frame) {
    group.nodes.clear();
    group.spatial_index.Clear();
    group.hard_edges.clear();
    group.next_node_id = 0;
    std::vector<int> new_node_ids;
    for (const auto& blob : blobs) {
        const int id_before = group.next_node_id;
        if (InsertOrMergeBlobNode(group, blob, blob.centroid, blob.bbox, current_frame,
                                  kEnableInsertMergeDeduplication,
                                  duplicate_debug_mode_,
                                  kMergeSearchRadiusPx,
                                  kDuplicatePosOverlapThreshold,
                                  kDuplicateCentroidIouThreshold,
                                  kDuplicateBboxIouThreshold,
                                  kDuplicateMaxShapeDifference) &&
            group.next_node_id != id_before &&
            group.nodes.count(id_before) &&
            !IsGhostNode(*group.nodes.at(id_before))) {
            new_node_ids.push_back(id_before);
        }
    }
    if (new_node_ids.size() >= 2)
        CreateHardEdgesForFrame(group, new_node_ids,
                                kHardEdgeMaxCentroidDist);
    UpdateGroupBounds(group);
    group.stroke_cache_dirty = true;
    group.raw_cache_dirty    = true;
    BumpCanvasVersion();
}

void WhiteboardCanvas::CreateSubCanvas(const cv::Mat& frame_bgr, const cv::Mat& binary,
                                        std::vector<FrameBlob>& blobs, int current_frame) {
    auto group = std::make_unique<WhiteboardGroup>();
    group->fixed_render_height = std::max(frame_h_, frame_bgr.rows);
    group->stroke_min_px_x = 0; group->stroke_min_px_y = 0;
    group->stroke_max_px_x = std::max(1, frame_bgr.cols);
    group->stroke_max_px_y = std::max(1, frame_bgr.rows);
    group->raw_min_px_x = 0; group->raw_min_px_y = 0;
    group->raw_max_px_x = std::max(frame_w_, frame_bgr.cols);
    group->raw_max_px_y = std::max(frame_h_, frame_bgr.rows);

    if (blobs.empty()) {
        blobs = ExtractFrameBlobs(binary, frame_bgr);
        EnhanceFrameBlobs(blobs, frame_bgr, g_canvas_enhance_threshold.load());
    }
    SeedGroupFromFrameBlobs(*group, blobs, current_frame);

    int idx = (int)groups_.size();
    groups_.push_back(std::move(group));
    active_group_idx_ = idx;
    if (canvas_view_mode_.load() && view_group_idx_ == -1) view_group_idx_ = idx;
    has_content_ = true;
}

// ============================================================================
//  SECTION 13: Graph node access methods
// ============================================================================

int WhiteboardCanvas::GetGraphNodeCount() const {
    if (remote_process_ && helper_client_) return helper_client_->GetGraphNodeCount();
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    return (int)groups_[gi]->nodes.size();
}

int WhiteboardCanvas::GetGraphNodes(float* buffer, int max_nodes) const {
    if (!buffer || max_nodes <= 0) return 0;
    if (remote_process_ && helper_client_) return helper_client_->GetGraphNodes(buffer, max_nodes);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    return CopyGraphNodesToBuffer(*groups_[gi], buffer, max_nodes);
}

int WhiteboardCanvas::GetGraphHardEdges(int* buffer, int max_edges) const {
    if (!buffer || max_edges <= 0) return 0;
    if (remote_process_ && helper_client_)
        return helper_client_->GetGraphHardEdges(buffer, max_edges);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    return CopyGraphHardEdgesToBuffer(*groups_[gi], buffer, max_edges);
}

int WhiteboardCanvas::GetGraphNodeNeighbors(int node_id, int* neighbors,
                                              int max_neighbors) const {
    if (!neighbors || max_neighbors <= 0) return 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    const auto edge_it = groups_[gi]->hard_edges.find(node_id);
    if (edge_it == groups_[gi]->hard_edges.end()) return 0;
    int count = 0;
    for (int neighbor_id : edge_it->second) {
        if (count >= max_neighbors) break;
        neighbors[count++] = neighbor_id;
    }
    return count;
}

bool WhiteboardCanvas::CompareGraphNodes(int id_a, int id_b, float* result) const {
    if (!result) return false;
    if (remote_process_ && helper_client_)
        return helper_client_->CompareGraphNodes(id_a, id_b, result);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;
    const auto& group = *groups_[gi];
    auto itA = group.nodes.find(id_a), itB = group.nodes.find(id_b);
    if (itA == group.nodes.end() || itB == group.nodes.end()) return false;
    const DrawingNode& a = *itA->second;
    const DrawingNode& b = *itB->second;

    const DuplicateCandidateView first_candidate{
        a.bbox_canvas, &a.binary_mask, &a.contour, a.centroid_canvas, a.hu, a.created_frame};
    const DuplicateCandidateView second_candidate{
        b.bbox_canvas, &b.binary_mask, &b.contour, b.centroid_canvas, b.hu, b.created_frame};
    const DuplicateCheckResult duplicate_check = EvaluateDuplicateCandidate(
        first_candidate,
        second_candidate,
        kDuplicatePosOverlapThreshold,
        kDuplicateCentroidIouThreshold,
        kDuplicateBboxIouThreshold,
        kDuplicateMaxShapeDifference);
    const DuplicateCheckResult filtered_duplicate_check =
        SuppressDuplicateForHardEdge(group, id_a, id_b, duplicate_check);

    const cv::Rect aligned_b = AlignRectToCentroid(
        b.bbox_canvas, b.centroid_canvas, a.centroid_canvas);
    const TotalShapeCompareResult shape_compare = TotalShapeCompare(
        a.bbox_canvas,
        &a.binary_mask,
        a.hu,
        &a.contour,
        aligned_b,
        &b.binary_mask,
        b.hu,
        &b.contour);
    result[0] = shape_compare.difference;

    cv::Point2f diff = a.centroid_canvas - b.centroid_canvas;
    result[1] = std::sqrt(diff.x*diff.x + diff.y*diff.y);

    cv::Rect isect = a.bbox_canvas & b.bbox_canvas;
    result[2] = (float)isect.area();

    float and_ov = 0, ov_ratio = 0;
    if (isect.area() > 0) {
        cv::Rect al(isect.x - a.bbox_canvas.x, isect.y - a.bbox_canvas.y, isect.width, isect.height);
        cv::Rect bl(isect.x - b.bbox_canvas.x, isect.y - b.bbox_canvas.y, isect.width, isect.height);
        al &= cv::Rect(0, 0, a.binary_mask.cols, a.binary_mask.rows);
        bl &= cv::Rect(0, 0, b.binary_mask.cols, b.binary_mask.rows);
        if (al.area() > 0 && bl.area() > 0 && al.size() == bl.size()) {
            cv::Mat ov; cv::bitwise_and(a.binary_mask(al), b.binary_mask(bl), ov);
            and_ov = (float)cv::countNonZero(ov);
            float mn = (float)std::min(cv::countNonZero(a.binary_mask), cv::countNonZero(b.binary_mask));
            if (mn > 0) ov_ratio = and_ov / mn;
        }
    }
    result[3] = and_ov;
    result[4] = ov_ratio;
    result[5] = filtered_duplicate_check.bbox_iou;
    float wa = (float)a.bbox_canvas.width, wb = (float)b.bbox_canvas.width;
    float ha = (float)a.bbox_canvas.height, hb = (float)b.bbox_canvas.height;
    result[6] = std::max(wa,wb) > 0 ? std::min(wa,wb)/std::max(wa,wb) : 0.0f;
    result[7] = std::max(ha,hb) > 0 ? std::min(ha,hb)/std::max(ha,hb) : 0.0f;

    // Centroid-aligned overlap
    float ca_ov = 0, ca_ratio = 0;
    {
        int sx = (int)std::round(a.centroid_canvas.x - b.centroid_canvas.x);
        int sy = (int)std::round(a.centroid_canvas.y - b.centroid_canvas.y);
        cv::Rect bs = b.bbox_canvas; bs.x += sx; bs.y += sy;
        cv::Rect ai = a.bbox_canvas & bs;
        if (ai.area() > 0) {
            cv::Rect al2(ai.x-a.bbox_canvas.x, ai.y-a.bbox_canvas.y, ai.width, ai.height);
            cv::Rect bl2(ai.x-bs.x, ai.y-bs.y, ai.width, ai.height);
            al2 &= cv::Rect(0,0,a.binary_mask.cols,a.binary_mask.rows);
            bl2 &= cv::Rect(0,0,b.binary_mask.cols,b.binary_mask.rows);
            if (al2.area() > 0 && bl2.area() > 0 && al2.size() == bl2.size()) {
                cv::Mat ov2; cv::bitwise_and(a.binary_mask(al2), b.binary_mask(bl2), ov2);
                ca_ov = (float)cv::countNonZero(ov2);
                float mn2 = (float)std::min(cv::countNonZero(a.binary_mask),
                                             cv::countNonZero(b.binary_mask));
                if (mn2 > 0) ca_ratio = ca_ov / mn2;
            }
        }
    }
    result[8] = ca_ov;
    result[9] = ca_ratio;
    result[10] = filtered_duplicate_check.centroid_iou;
    result[11] = filtered_duplicate_check.same_creation_frame ? 1.0f : 0.0f;
    result[12] = filtered_duplicate_check.is_duplicate ? 1.0f : 0.0f;
    result[13] = (float)filtered_duplicate_check.reason_mask;
    result[14] = shape_compare.shape_context_distance;
    result[15] = shape_compare.shape_context_difference;
    result[16] = shape_compare.used_shape_context ? 1.0f : 0.0f;
    result[17] = shape_compare.hu_distance;
    result[18] = shape_compare.hu_difference;
    return true;
}

bool WhiteboardCanvas::CompareGraphNodesAtOffset(int id_a, int id_b,
                                                  float dx, float dy, float* result) const {
    if (!result) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;
    const auto& group = *groups_[gi];
    auto itA = group.nodes.find(id_a), itB = group.nodes.find(id_b);
    if (itA == group.nodes.end() || itB == group.nodes.end()) return false;
    const DrawingNode& a = *itA->second;
    const DrawingNode& b = *itB->second;

    cv::Rect as = a.bbox_canvas; as.x += (int)std::round(dx); as.y += (int)std::round(dy);
    cv::Rect isect = as & b.bbox_canvas;
    float and_ov = 0, ov_ratio = 0, bbox_iou = 0;
    if (isect.area() > 0) {
        cv::Rect al(isect.x-as.x, isect.y-as.y, isect.width, isect.height);
        cv::Rect bl(isect.x-b.bbox_canvas.x, isect.y-b.bbox_canvas.y, isect.width, isect.height);
        al &= cv::Rect(0,0,a.binary_mask.cols,a.binary_mask.rows);
        bl &= cv::Rect(0,0,b.binary_mask.cols,b.binary_mask.rows);
        if (al.area() > 0 && bl.area() > 0 && al.size() == bl.size()) {
            cv::Mat ov; cv::bitwise_and(a.binary_mask(al), b.binary_mask(bl), ov);
            and_ov = (float)cv::countNonZero(ov);
            float mn = (float)std::min(cv::countNonZero(a.binary_mask), cv::countNonZero(b.binary_mask));
            if (mn > 0) ov_ratio = and_ov / mn;
        }
        float ua = (float)(as.area() + b.bbox_canvas.area() - isect.area());
        if (ua > 0) bbox_iou = (float)isect.area() / ua;
    }
    result[0] = and_ov; result[1] = ov_ratio; result[2] = bbox_iou;
    return true;
}

int WhiteboardCanvas::GetGraphNodeMasks(uint8_t* buffer, int max_bytes) const {
    if (!buffer || max_bytes <= 0) return 0;
    if (remote_process_ && helper_client_) return helper_client_->GetGraphNodeMasks(buffer, max_bytes);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    const auto& group = *groups_[gi];
    int offset = 0;
    for (const auto& [id, np] : group.nodes) {
        const DrawingNode& node = *np;
        if (node.binary_mask.empty()) continue;
        int w = node.binary_mask.cols, h = node.binary_mask.rows;
        int pbytes = w * h * 4, hbytes = 12;
        if (offset + hbytes + pbytes > max_bytes) break;
        int* hdr = reinterpret_cast<int*>(buffer + offset);
        hdr[0] = node.id; hdr[1] = w; hdr[2] = h;
        offset += hbytes;
        for (int y = 0; y < h; y++) {
            const uint8_t* mr = node.binary_mask.ptr<uint8_t>(y);
            const uint8_t* cr =
                (!node.color_pixels.empty() && y < node.color_pixels.rows)
                    ? node.color_pixels.ptr<uint8_t>(y)
                    : nullptr;
            uint8_t* or_ = buffer + offset + y * w * 4;
            for (int x = 0; x < w; x++) {
                if (mr[x]) {
                    if (cr && x < node.color_pixels.cols) {
                        or_[x*4+0] = cr[x*3+2];
                        or_[x*4+1] = cr[x*3+1];
                        or_[x*4+2] = cr[x*3+0];
                        or_[x*4+3] = 255;
                    } else {
                        or_[x*4+0] = 255;
                        or_[x*4+1] = 255;
                        or_[x*4+2] = 255;
                        or_[x*4+3] = 255;
                    }
                } else {
                    or_[x*4+0] = or_[x*4+1] = or_[x*4+2] = or_[x*4+3] = 0;
                }
            }
        }
        offset += pbytes;
    }
    return offset;
}

bool WhiteboardCanvas::MoveGraphNode(int node_id, float new_cx, float new_cy) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;
    auto& group = *groups_[gi];
    auto it = group.nodes.find(node_id);
    if (it == group.nodes.end()) return false;
    DrawingNode& node = *it->second;
    float dx = new_cx - node.centroid_canvas.x;
    float dy = new_cy - node.centroid_canvas.y;
    if (!IsGhostNode(node)) {
        group.spatial_index.Remove(node.id, node.centroid_canvas);
    }
    node.centroid_canvas.x = new_cx; node.centroid_canvas.y = new_cy;
    node.bbox_canvas.x += (int)std::round(dx);
    node.bbox_canvas.y += (int)std::round(dy);
    if (!IsGhostNode(node)) {
        group.spatial_index.Insert(node.id, node.centroid_canvas);
    }
    node.user_locked = true;
    // Propagate same delta to hard-edge connected nodes
    if (!IsGhostNode(node)) {
        std::unordered_set<int> moved_self{node_id};
        ApplyHardEdgeDelta(group, node_id, dx, dy, moved_self);
        PruneHardEdgesByDistance(group, kHardEdgeMaxCentroidDist);
    }
    group.stroke_cache_dirty = true;
    group.raw_cache_dirty    = true;
    BumpCanvasVersion();
    return true;
}

bool WhiteboardCanvas::DeleteGraphNode(int node_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;
    auto& group = *groups_[gi];
    if (group.nodes.find(node_id) == group.nodes.end()) return false;
    group.user_deleted_ids.insert(node_id);
    RemoveNodeFromGraph(group, node_id);
    group.stroke_cache_dirty = true;
    group.raw_cache_dirty    = true;
    BumpCanvasVersion();
    return true;
}

bool WhiteboardCanvas::ApplyUserEdits(const int* delete_ids, int delete_count,
                                       const float* moves, int move_count) {
    if (remote_process_ && helper_client_)
        return helper_client_->ApplyUserEdits(delete_ids, delete_count, moves, move_count);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;
    auto& group = *groups_[gi];
    bool changed = false;

    for (int i = 0; i < delete_count; i++) {
        int nid = delete_ids[i];
        group.user_deleted_ids.insert(nid);
        if (group.nodes.count(nid)) { RemoveNodeFromGraph(group, nid); changed = true; }
    }

    for (int i = 0; i < move_count; i++) {
        int nid = (int)moves[i*3+0];
        float ncx = moves[i*3+1], ncy = moves[i*3+2];
        auto it = group.nodes.find(nid);
        if (it == group.nodes.end()) continue;
        DrawingNode& node = *it->second;
        float dx = ncx - node.centroid_canvas.x, dy = ncy - node.centroid_canvas.y;
        if (!IsGhostNode(node)) {
            group.spatial_index.Remove(node.id, node.centroid_canvas);
        }
        node.centroid_canvas.x = ncx; node.centroid_canvas.y = ncy;
        node.bbox_canvas.x += (int)std::round(dx);
        node.bbox_canvas.y += (int)std::round(dy);
        if (!IsGhostNode(node)) {
            group.spatial_index.Insert(node.id, node.centroid_canvas);
        }
        node.user_locked = true;
        changed = true;
    }

    changed = PruneHardEdgesByDistance(group, kHardEdgeMaxCentroidDist) || changed;

    if (changed) {
        UpdateGroupBounds(group);
        group.stroke_cache_dirty = true;
        group.raw_cache_dirty    = true;
        BumpCanvasVersion();
    }
    return changed;
}

int WhiteboardCanvas::LockAllGraphNodes() {
    if (remote_process_ && helper_client_) return helper_client_->LockAllGraphNodes();
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    int count = 0;
    for (auto& p : groups_[gi]->nodes) { p.second->user_locked = true; count++; }
    return count;
}

bool WhiteboardCanvas::GetGraphCanvasBounds(int* bounds) const {
    if (!bounds) return false;
    if (remote_process_ && helper_client_) return helper_client_->GetGraphCanvasBounds(bounds);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;
    return CopyGraphBoundsToBuffer(*groups_[gi], bounds);
}

int WhiteboardCanvas::GetGraphNodeContours(float* buffer, int max_floats) const {
    if (!buffer || max_floats <= 0) return 0;
    if (remote_process_ && helper_client_)
        return helper_client_->GetGraphNodeContours(buffer, max_floats);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    return CopyGraphContoursToBuffer(*groups_[gi], buffer, max_floats);
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
}

void ResetPanorama() {
    if (g_whiteboard_canvas) g_whiteboard_canvas->Reset();
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
}

void SetPanoramaViewport(float panX, float panY, float zoom) {
    g_canvas_pan_x.store(panX); g_canvas_pan_y.store(panY); g_canvas_zoom.store(zoom);
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

bool IsPanoramaEnabled() { return g_whiteboard_enabled.load(); }

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
                ? CanvasRenderMode::kRaw : CanvasRenderMode::kStroke);
    }
    if (g_native_camera) g_native_camera->RefreshDisplayFrame();
}

int64_t GetCanvasTextureId() {
    return g_native_camera ? g_native_camera->GetTextureId() : -1;
}

bool GetCanvasOverviewRgba(uint8_t* buffer, int width, int height) {
    if (!g_whiteboard_canvas || !buffer || width <= 0 || height <= 0) return false;
    cv::Mat overview;
    if (!g_whiteboard_canvas->GetOverviewBlocking(cv::Size(width, height), overview)) return false;
    return CopyBgrFrameToRgbaBuffer(overview, buffer, width, height);
}

bool GetCanvasViewportRgba(uint8_t* buffer, int width, int height,
                            float panX, float panY, float zoom) {
    if (!g_whiteboard_canvas || !buffer || width <= 0 || height <= 0) return false;
    cv::Mat viewport;
    if (!g_whiteboard_canvas->GetViewport(panX, panY, zoom,
                                           cv::Size(width, height), viewport)) return false;
    return CopyBgrFrameToRgbaBuffer(viewport, buffer, width, height);
}

void SetWhiteboardDebug(bool enabled) {
    g_whiteboard_debug.store(enabled);
    if (g_whiteboard_canvas) g_whiteboard_canvas->SyncRuntimeSettings();
}

void SetDuplicateDebugMode(bool enabled) {
    g_duplicate_debug_mode.store(enabled);
    if (g_whiteboard_canvas) g_whiteboard_canvas->SetDuplicateDebugMode(enabled);
}

bool GetDuplicateDebugMode() {
    return g_duplicate_debug_mode.load();
}

void SetCanvasEnhanceThreshold(float threshold) {
    g_canvas_enhance_threshold.store(threshold);
    if (g_whiteboard_canvas) g_whiteboard_canvas->SyncRuntimeSettings();
}

int GetSubCanvasCount() {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetSubCanvasCount() : 0;
}
int GetActiveSubCanvasIndex() {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetActiveSubCanvasIndex() : -1;
}
void SetActiveSubCanvas(int idx) {
    if (g_whiteboard_canvas) g_whiteboard_canvas->SetActiveSubCanvas(idx);
}
int GetSortedSubCanvasIndex(int pos) {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetSortedSubCanvasIndex(pos) : -1;
}
int GetSortedPosition(int idx) {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetSortedPosition(idx) : -1;
}

int GetGraphNodeCount() {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetGraphNodeCount() : 0;
}
int GetGraphNodes(float* buffer, int max_nodes) {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetGraphNodes(buffer, max_nodes) : 0;
}
int GetGraphHardEdges(int* buffer, int max_edges) {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetGraphHardEdges(buffer, max_edges) : 0;
}
int GetGraphNodeNeighbors(int node_id, int* neighbors, int max_neighbors) {
    return g_whiteboard_canvas
        ? g_whiteboard_canvas->GetGraphNodeNeighbors(node_id, neighbors, max_neighbors) : 0;
}
bool CompareGraphNodes(int id_a, int id_b, float* result) {
    return g_whiteboard_canvas && g_whiteboard_canvas->CompareGraphNodes(id_a, id_b, result);
}
bool CompareGraphNodesAtOffset(int id_a, int id_b, float dx, float dy, float* result) {
    return g_whiteboard_canvas &&
        g_whiteboard_canvas->CompareGraphNodesAtOffset(id_a, id_b, dx, dy, result);
}
int GetGraphNodeMasks(uint8_t* buffer, int max_bytes) {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetGraphNodeMasks(buffer, max_bytes) : 0;
}
bool MoveGraphNode(int node_id, float new_cx, float new_cy) {
    return g_whiteboard_canvas && g_whiteboard_canvas->MoveGraphNode(node_id, new_cx, new_cy);
}
bool DeleteGraphNode(int node_id) {
    return g_whiteboard_canvas && g_whiteboard_canvas->DeleteGraphNode(node_id);
}
bool ApplyUserEdits(const int* delete_ids, int delete_count,
                    const float* moves, int move_count) {
    return g_whiteboard_canvas &&
        g_whiteboard_canvas->ApplyUserEdits(delete_ids, delete_count, moves, move_count);
}
int LockAllGraphNodes() {
    return g_whiteboard_canvas ? g_whiteboard_canvas->LockAllGraphNodes() : 0;
}
bool GetGraphCanvasBounds(int* bounds) {
    return g_whiteboard_canvas && g_whiteboard_canvas->GetGraphCanvasBounds(bounds);
}
int GetGraphNodeContours(float* buffer, int max_floats) {
    return g_whiteboard_canvas
        ? g_whiteboard_canvas->GetGraphNodeContours(buffer, max_floats) : 0;
}

// Debug snapshot stubs
bool CaptureGraphDebugSnapshot(int /*slot*/) { return false; }
int  GetGraphSnapshotNodeCount(int /*slot*/) { return 0; }
int  GetGraphSnapshotNodes(int /*slot*/, float* /*buf*/, int /*max*/) { return 0; }
bool GetGraphSnapshotCanvasBounds(int /*slot*/, int* /*bounds*/) { return false; }
int  GetGraphSnapshotNodeContours(int /*slot*/, float* /*buf*/, int /*max*/) { return 0; }
bool CompareGraphSnapshotNodes(int, int, int, int, float*) { return false; }
bool CombineGraphDebugSnapshots(int, int, int, int) { return false; }
bool CopyGraphDebugSnapshot(int, int) { return false; }

uint64_t GetCanvasVersion() {
    return g_whiteboard_canvas ? g_whiteboard_canvas->GetCanvasVersion() : 0;
}

bool GetCanvasFullResRgba(uint8_t* buffer, int max_w, int max_h, int* out_w, int* out_h) {
    if (!g_whiteboard_canvas || !buffer || max_w <= 0 || max_h <= 0) return false;
    cv::Mat overview;
    if (!g_whiteboard_canvas->GetOverviewBlocking(cv::Size(max_w, max_h), overview)) return false;
    if (out_w) *out_w = overview.cols;
    if (out_h) *out_h = overview.rows;
    return CopyBgrFrameToRgbaBuffer(overview, buffer, overview.cols, overview.rows);
}
