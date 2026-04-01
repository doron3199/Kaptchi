// ============================================================================
// whiteboard_canvas.cpp -- Graph-Based Canvas Stitcher (simplified)
//
// PIPELINE (runs on worker thread once per accepted frame):
//
//   [1] Motion gate   -- skip frames with too much movement
//   [2] No-update mask -- person mask defines protected zone
//   [3] Binarize      -- adaptiveThreshold -> binary mask
//   [4] Blob extract  -- connected components -> FrameBlobs
//   [5] Match         -- spatial + Hu-distance match -> camera offset
//   [6] Graph update  -- refresh matched nodes, add new, absence-prune old
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
std::atomic<float> g_yolo_fps{2.0f};
std::atomic<float> g_canvas_enhance_threshold{4.0f};

// ============================================================================
//  SECTION 2: Static helpers
// ============================================================================

namespace {

using SteadyClock = std::chrono::steady_clock;

static constexpr int kEnhancePadding = 10;

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

static int CopyGraphNodesToBuffer(const WhiteboardGroup& group,
                                  float* buffer, int max_nodes) {
    if (!buffer || max_nodes <= 0) return 0;
    int count = 0;
    for (const auto& p : group.nodes) {
        if (count >= max_nodes) break;
        const DrawingNode& n = *p.second;
        float* c = buffer + count * 16;
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
        c[11] = 0.0f;   // neighbor count (removed)
        c[12] = (float)group.stroke_min_px_x;
        c[13] = (float)group.stroke_min_px_y;
        c[14] = 0.0f;   // match_distance (removed)
        c[15] = n.user_locked ? 1.0f : 0.0f;
        count++;
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

static cv::Rect ComputeProcessingRoi(const cv::Size& sz) {
    if (sz.width <= 0 || sz.height <= 0) return {};
    int top   = sz.height / 20;
    int left  = sz.width  / 20;
    int bot   = std::max(top + 1, sz.height - top);
    int right = std::max(left + 1, sz.width  - left);
    return cv::Rect(left, top, right - left, bot - top);
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
    int ct = (int)(fh * 0.15), cb = fh - (int)(fh * 0.95);
    int cl = (int)(fw * 0.05), cr = fw - (int)(fw * 0.95);
    return cv::Rect(cl, ct, std::max(1, fw - cl - cr), std::max(1, fh - ct - cb));
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
                                          float cluster_radius = 20.0f) {
    if (fs.width <= 0 || fs.height <= 0) return {};
    cv::Mat reject(fs, CV_8UC1, cv::Scalar(0));
    int min_margin = (int)std::ceil(cluster_radius);
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

static void FilterBlobsForCanvas(std::vector<FrameBlob>& blobs, const cv::Mat& reject_mask) {
    blobs.erase(
        std::remove_if(blobs.begin(), blobs.end(),
            [&](const FrameBlob& b) { return BlobTouchesRejectMask(b, reject_mask); }),
        blobs.end());
}

// Returns overlap/min_area for the better of positional or centroid-aligned alignment.
// Shifts mask_b so centroid_b aligns with centroid_a and takes the max of the two results.
static float BestMaskOverlap(const cv::Rect& bbox_a, const cv::Mat& mask_a,
                              const cv::Point2f& centroid_a,
                              const cv::Rect& bbox_b, const cv::Mat& mask_b,
                              const cv::Point2f& centroid_b) {
    int a_px = mask_a.empty() ? 0 : cv::countNonZero(mask_a);
    int b_px = mask_b.empty() ? 0 : cv::countNonZero(mask_b);
    int min_px = std::min(a_px, b_px);
    if (min_px <= 0) return 0.0f;

    auto overlap_at = [&](int dx, int dy) -> float {
        cv::Rect shifted_b(bbox_b.x + dx, bbox_b.y + dy, bbox_b.width, bbox_b.height);
        const cv::Rect isect = bbox_a & shifted_b;
        if (isect.empty()) return 0.0f;
        cv::Rect al(isect.x - bbox_a.x,    isect.y - bbox_a.y,    isect.width, isect.height);
        cv::Rect bl(isect.x - shifted_b.x, isect.y - shifted_b.y, isect.width, isect.height);
        if (al.x < 0 || al.y < 0 || al.x+al.width > mask_a.cols ||
            al.y+al.height > mask_a.rows ||
            bl.x < 0 || bl.y < 0 || bl.x+bl.width > mask_b.cols ||
            bl.y+bl.height > mask_b.rows) return 0.0f;
        cv::Mat ov;
        cv::bitwise_and(mask_a(al), mask_b(bl), ov);
        return (float)cv::countNonZero(ov) / (float)min_px;
    };

    float pos = overlap_at(0, 0);
    if (pos >= 1.0f) return pos;
    int dx = (int)std::round(centroid_a.x - centroid_b.x);
    int dy = (int)std::round(centroid_a.y - centroid_b.y);
    return std::max(pos, overlap_at(dx, dy));
}

static cv::Rect TranslateFrameRectToCanvas(const cv::Rect& r, const cv::Point2f& offset) {
    if (r.width <= 0 || r.height <= 0) return {};
    return cv::Rect(r.x + (int)std::round(offset.x),
                    r.y + (int)std::round(offset.y),
                    r.width, r.height);
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
    group.spatial_index.Insert(node.id, canvas_centroid);
}

// ---------------------------------------------------------------------------
// FindBestAlignment -- sliding window to find best (dx,dy) to align mask_b
// onto mask_a, using one of three scoring modes.
// Returns the offset to SHIFT mask_b's bbox so it best aligns with mask_a.
// The search space is [-r, +r] in both axes.
// ---------------------------------------------------------------------------
struct AlignResult { int dx = 0; int dy = 0; float score = -1e30f; };

static AlignResult FindBestAlignment(
    const cv::Rect& bbox_a, const cv::Mat& mask_a,
    const cv::Rect& bbox_b, const cv::Mat& mask_b,
    int search_radius, AlignmentScoreMode mode)
{
    if (mask_a.empty() || mask_b.empty()) return {};

    const int a_px = cv::countNonZero(mask_a);
    const int b_px = cv::countNonZero(mask_b);
    if (a_px == 0 || b_px == 0) return {};

    // Build a canvas large enough to hold both masks at any offset in search range.
    // Origin of the canvas is (canvas_ox, canvas_oy) in the global coord system.
    const int canvas_x0 = std::min(bbox_a.x, bbox_b.x - search_radius);
    const int canvas_y0 = std::min(bbox_a.y, bbox_b.y - search_radius);
    const int canvas_x1 = std::max(bbox_a.x + bbox_a.width,
                                   bbox_b.x + bbox_b.width + search_radius);
    const int canvas_y1 = std::max(bbox_a.y + bbox_a.height,
                                   bbox_b.y + bbox_b.height + search_radius);
    const int cw = canvas_x1 - canvas_x0;
    const int ch = canvas_y1 - canvas_y0;
    if (cw <= 0 || ch <= 0) return {};

    // Place mask_a once on the canvas
    cv::Mat canvas_a = cv::Mat::zeros(ch, cw, CV_8UC1);
    {
        int ax = bbox_a.x - canvas_x0, ay = bbox_a.y - canvas_y0;
        if (ax >= 0 && ay >= 0 && ax + mask_a.cols <= cw && ay + mask_a.rows <= ch)
            mask_a.copyTo(canvas_a(cv::Rect(ax, ay, mask_a.cols, mask_a.rows)));
    }

    // For chamfer mode: precompute distance transform of A's edges
    cv::Mat dist_a;
    if (mode == AlignmentScoreMode::kChamfer) {
        cv::Mat edge_a;
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
        cv::morphologyEx(canvas_a, edge_a, cv::MORPH_GRADIENT, kernel);
        cv::Mat inv_edge;
        cv::bitwise_not(edge_a, inv_edge);
        cv::distanceTransform(inv_edge, dist_a, cv::DIST_L2, 3);
    }

    AlignResult best;

    for (int dy = -search_radius; dy <= search_radius; ++dy) {
        for (int dx = -search_radius; dx <= search_radius; ++dx) {
            // Place shifted mask_b on a temporary canvas
            int bx = bbox_b.x + dx - canvas_x0;
            int by = bbox_b.y + dy - canvas_y0;
            if (bx < 0 || by < 0 || bx + mask_b.cols > cw || by + mask_b.rows > ch)
                continue;

            cv::Rect b_roi(bx, by, mask_b.cols, mask_b.rows);

            float score = -1e30f;

            if (mode == AlignmentScoreMode::kIoU) {
                cv::Mat overlap;
                cv::bitwise_and(canvas_a(b_roi), mask_b, overlap);
                int and_px = cv::countNonZero(overlap);
                int union_px = a_px + b_px - and_px;
                score = (union_px > 0) ? (float)and_px / (float)union_px : 0.0f;

            } else if (mode == AlignmentScoreMode::kChamfer) {
                // Edge of shifted B
                cv::Mat edge_b;
                cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
                cv::morphologyEx(mask_b, edge_b, cv::MORPH_GRADIENT, kernel);
                // Sample dist_a at edge_b pixel locations
                cv::Mat dist_roi = dist_a(b_roi);
                double sum = 0.0; int cnt = 0;
                for (int y = 0; y < edge_b.rows; ++y) {
                    const uchar* ep = edge_b.ptr<uchar>(y);
                    const float* dp = dist_roi.ptr<float>(y);
                    for (int x = 0; x < edge_b.cols; ++x) {
                        if (ep[x]) { sum += dp[x]; ++cnt; }
                    }
                }
                float chamfer = (cnt > 0) ? (float)(sum / cnt) : 1e6f;
                score = -chamfer;  // lower distance = better

            } else if (mode == AlignmentScoreMode::kLargestBlob) {
                cv::Mat combined;
                cv::bitwise_or(canvas_a(b_roi), mask_b, combined);
                // We need the full combined canvas for connected components
                cv::Mat full_combined = canvas_a.clone();
                combined.copyTo(full_combined(b_roi));
                cv::Mat labels, stats, centroids;
                int n = cv::connectedComponentsWithStats(full_combined, labels, stats, centroids);
                int max_area = 0;
                for (int i = 1; i < n; ++i) {
                    int area = stats.at<int>(i, cv::CC_STAT_AREA);
                    if (area > max_area) max_area = area;
                }
                score = (float)max_area;
            }

            if (score > best.score) {
                best = {dx, dy, score};
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// MergeNodes -- Combine two nodes into one using winner-priority compositing
// at the best sliding-window alignment. Winner = larger node by area.
// ---------------------------------------------------------------------------
static void MergeNodes(WhiteboardGroup& group, DrawingNode& winner, DrawingNode& loser,
                       int search_radius, AlignmentScoreMode mode) {
    // Find best alignment offset for loser relative to its current position
    AlignResult align = FindBestAlignment(
        winner.bbox_canvas, winner.binary_mask,
        loser.bbox_canvas,  loser.binary_mask,
        search_radius, mode);

    // Aligned loser bbox
    cv::Rect aligned_loser_bbox(
        loser.bbox_canvas.x + align.dx, loser.bbox_canvas.y + align.dy,
        loser.bbox_canvas.width, loser.bbox_canvas.height);

    // Combined bounding box
    cv::Rect combined_bbox = winner.bbox_canvas | aligned_loser_bbox;

    // --- Binary mask: winner priority ---
    cv::Mat combined_mask = cv::Mat::zeros(combined_bbox.height, combined_bbox.width, CV_8UC1);
    {
        int wx = winner.bbox_canvas.x - combined_bbox.x;
        int wy = winner.bbox_canvas.y - combined_bbox.y;
        cv::Rect w_roi(wx, wy, winner.binary_mask.cols, winner.binary_mask.rows);
        winner.binary_mask.copyTo(combined_mask(w_roi));
    }
    {
        int lx = aligned_loser_bbox.x - combined_bbox.x;
        int ly = aligned_loser_bbox.y - combined_bbox.y;
        cv::Rect l_roi(lx, ly, loser.binary_mask.cols, loser.binary_mask.rows);
        // Only add loser pixels where winner has nothing
        cv::Mat winner_region = combined_mask(l_roi);
        cv::Mat loser_unique;
        cv::Mat inv_winner;
        cv::bitwise_not(winner_region, inv_winner);
        cv::bitwise_and(loser.binary_mask, inv_winner, loser_unique);
        combined_mask(l_roi) |= loser_unique;
    }

    // --- Color pixels: winner priority ---
    cv::Mat combined_color = cv::Mat::zeros(combined_bbox.height, combined_bbox.width, CV_8UC3);
    bool has_color = !winner.color_pixels.empty() || !loser.color_pixels.empty();
    if (has_color) {
        if (!winner.color_pixels.empty()) {
            int wx = winner.bbox_canvas.x - combined_bbox.x;
            int wy = winner.bbox_canvas.y - combined_bbox.y;
            winner.color_pixels.copyTo(combined_color(cv::Rect(wx, wy,
                winner.color_pixels.cols, winner.color_pixels.rows)));
        }
        if (!loser.color_pixels.empty()) {
            int lx = aligned_loser_bbox.x - combined_bbox.x;
            int ly = aligned_loser_bbox.y - combined_bbox.y;
            cv::Rect l_roi(lx, ly, loser.color_pixels.cols, loser.color_pixels.rows);
            // Only copy where loser_unique was set (winner had no mask)
            cv::Mat winner_region = cv::Mat::zeros(loser.color_pixels.rows,
                                                    loser.color_pixels.cols, CV_8UC1);
            {
                int wx_in_l = winner.bbox_canvas.x - aligned_loser_bbox.x;
                int wy_in_l = winner.bbox_canvas.y - aligned_loser_bbox.y;
                cv::Rect isect = cv::Rect(0, 0, loser.binary_mask.cols, loser.binary_mask.rows) &
                                 cv::Rect(wx_in_l, wy_in_l,
                                          winner.binary_mask.cols, winner.binary_mask.rows);
                if (!isect.empty()) {
                    cv::Rect src_roi(isect.x - wx_in_l, isect.y - wy_in_l,
                                     isect.width, isect.height);
                    winner.binary_mask(src_roi).copyTo(winner_region(isect));
                }
            }
            cv::Mat loser_only_mask;
            cv::Mat inv_wr;
            cv::bitwise_not(winner_region, inv_wr);
            cv::bitwise_and(loser.binary_mask, inv_wr, loser_only_mask);
            // 3-channel mask for copyTo
            cv::Mat mask3;
            cv::cvtColor(loser_only_mask, mask3, cv::COLOR_GRAY2BGR);
            cv::Mat loser_masked;
            cv::bitwise_and(loser.color_pixels, mask3, loser_masked);
            combined_color(l_roi) |= loser_masked;
        }
    }

    // --- Recompute derived properties ---
    cv::Point2f local_center = ComputeGravityCenter(combined_mask);
    cv::Point2f new_centroid(local_center.x + combined_bbox.x,
                              local_center.y + combined_bbox.y);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(combined_mask.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    int best_ci = -1;
    double best_ca = 0.0;
    for (int i = 0; i < (int)contours.size(); ++i) {
        double ca = cv::contourArea(contours[i]);
        if (ca > best_ca) { best_ca = ca; best_ci = i; }
    }

    // Update winner node
    group.spatial_index.Remove(winner.id, winner.centroid_canvas);
    winner.bbox_canvas = combined_bbox;
    winner.binary_mask = combined_mask;
    if (has_color) winner.color_pixels = combined_color;
    winner.centroid_canvas = new_centroid;
    if (best_ci >= 0) {
        winner.contour = contours[best_ci];
        winner.area = best_ca;
        cv::Moments m = cv::moments(contours[best_ci]);
        cv::HuMoments(m, winner.hu);
    } else {
        winner.area = (double)cv::countNonZero(combined_mask);
    }
    winner.created_frame = std::min(winner.created_frame, loser.created_frame);
    group.spatial_index.Insert(winner.id, new_centroid);
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
                                     g_canvas_enhance_threshold.load(),
                                     g_yolo_fps.load());
    }
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

    if (!prev_gray_.empty() && prev_gray_.size() == mg.size()) {
        cv::Mat diff;
        cv::absdiff(mg, prev_gray_, diff);
        cv::threshold(diff, diff, kMotionPixelThreshold, 255, cv::THRESH_BINARY);
        motion_fraction = (float)cv::countNonZero(diff) / (float)std::max<size_t>(1, diff.total());
        motion_too_high = motion_fraction > kMaxMotionFraction;
    }
    mg.copyTo(prev_gray_);
    // Skip the frame only when the gate is enabled
    return kEnableMotionGate && motion_too_high;
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
        reject_mask = BuildFrameStrokeRejectMask(frame.size(), lecturer_rect, kStrokeClusterRadius);

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
        FilterBlobsForCanvas(blobs, reject_mask);

    std::lock_guard<std::mutex> state_lock(state_mutex_);

    auto recompute_has_content = [&]() {
        for (const auto& gp : groups_) if (gp && !gp->nodes.empty()) { has_content_ = true; return; }
        has_content_ = false;
    };

    if (frame_w_ == 0) { frame_w_ = frame.cols; frame_h_ = frame.rows; }

    const bool has_active = active_group_idx_ >= 0 &&
                            active_group_idx_ < (int)groups_.size();
    const int active_nodes = has_active ? (int)groups_[active_group_idx_]->nodes.size() : 0;
    const bool graph_ready = has_active && active_nodes >= kStableGraphNodeThreshold;

    // [4] Match blobs to graph (pure Hu, no camera state)
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
        cv::findContours(cc_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) continue;
        int best_ci = 0; double best_ca = 0;
        for (int ci = 0; ci < (int)contours.size(); ci++) {
            double ca = cv::contourArea(contours[ci]);
            if (ca > best_ca) { best_ca = ca; best_ci = ci; }
        }
        components.push_back({i, bbox, gc, best_ca, contours[best_ci]});
    }
    if (components.empty()) return result;

    // Cluster nearby components with Union-Find
    std::vector<int> parent(components.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](auto& self, int i) -> int {
        return parent[i] == i ? i : (parent[i] = self(self, parent[i]));
    };
    auto unite = [&](int i, int j) {
        int ri = find(find, i), rj = find(find, j);
        if (ri != rj) parent[ri] = rj;
    };
    {
        int cell = std::max(1, (int)std::ceil(kStrokeClusterRadius));
        std::unordered_map<uint64_t, std::vector<int>> grid;
        auto gkey = [](int gx, int gy) -> uint64_t {
            return ((uint64_t)(uint32_t)gx << 32) | (uint32_t)gy;
        };
        for (int i = 0; i < (int)components.size(); i++) {
            int gx = (int)std::floor(components[i].centroid.x / cell);
            int gy = (int)std::floor(components[i].centroid.y / cell);
            grid[gkey(gx, gy)].push_back(i);
        }
        for (int i = 0; i < (int)components.size(); i++) {
            int gx = (int)std::floor(components[i].centroid.x / cell);
            int gy = (int)std::floor(components[i].centroid.y / cell);
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                auto it = grid.find(gkey(gx+dx, gy+dy));
                if (it == grid.end()) continue;
                for (int j : it->second) {
                    if (j <= i) continue;
                    float d = (float)cv::norm(components[i].centroid - components[j].centroid);
                    if (d < kStrokeClusterRadius) unite(i, j);
                }
            }
        }
    }

    // Merge clusters into FrameBlobs
    std::unordered_map<int, std::vector<int>> clusters;
    for (int i = 0; i < (int)components.size(); i++)
        clusters[find(find, i)].push_back(i);

    for (auto& cp : clusters) {
        const auto& ids = cp.second;
        cv::Rect g_bbox = components[ids[0]].bbox;
        for (size_t k = 1; k < ids.size(); k++) g_bbox |= components[ids[k]].bbox;

        // Skip very elongated blobs (whiteboard edge lines)
        float le = (float)std::max(g_bbox.width, g_bbox.height);
        float se = (float)std::min(g_bbox.width, g_bbox.height);
        if (se > 0 && le / se > kMaxAllowedRectangle) continue;

        cv::Mat g_mask = cv::Mat::zeros(g_bbox.size(), CV_8UC1);
        double max_area = -1; int best_idx = -1;
        for (int idx : ids) {
            const auto& sc = components[idx];
            cv::Rect rel(sc.bbox.x - g_bbox.x, sc.bbox.y - g_bbox.y, sc.bbox.width, sc.bbox.height);
            cv::Mat cc_mask = (labels(sc.bbox) == sc.label);
            cc_mask.copyTo(g_mask(rel), cc_mask);
            if (sc.area > max_area) { max_area = sc.area; best_idx = idx; }
        }
        if (best_idx < 0) continue;

        FrameBlob blob;
        blob.bbox = g_bbox;
        blob.binary_mask = g_mask;
        cv::Point2f lc = ComputeGravityCenter(g_mask);
        blob.centroid = lc + cv::Point2f((float)g_bbox.x, (float)g_bbox.y);
        blob.contour = components[best_idx].contour;
        blob.area = components[best_idx].area;
        cv::Moments m = cv::moments(blob.contour);
        cv::HuMoments(m, blob.hu);
        if (!frame_bgr.empty()) blob.color_pixels = frame_bgr(g_bbox).clone();
        result.push_back(std::move(blob));
    }
    return result;
}

// ============================================================================
//  SECTION 9: Graph Matching (simple spatial + Hu distance + median vote)
// ============================================================================

cv::Point2f WhiteboardCanvas::MatchBlobsToGraph(WhiteboardGroup& group,
                                                  std::vector<FrameBlob>& blobs) {
    if (blobs.empty() || group.nodes.empty()) return {};

    for (auto& b : blobs) { b.matched_node_id = -1; b.matched_offset = {}; }

    struct Candidate { int blob_idx; int node_id; cv::Point2f offset; float hu_dist; };
    std::vector<Candidate> candidates;

    for (int i = 0; i < (int)blobs.size(); i++) {
        const auto& blob = blobs[i];
        auto blob_hu = ComputeLogHuFeatures(blob.hu);

        int best_node = -1;
        float best_dist = (float)kHuDistanceThreshold;
        float second_dist = (float)kHuDistanceThreshold;
        cv::Point2f best_offset;

        for (const auto& pair : group.nodes) {
            const auto& node = *pair.second;

            // [1] Area ratio filter: reject grossly different sizes
            if (node.area > 0.0) {
                float ratio = (float)(blob.area / node.area);
                if (ratio < kAreaRatioMin || ratio > (1.0f / kAreaRatioMin)) continue;
            }

            auto node_hu = ComputeLogHuFeatures(node.hu);
            float hu_dist = 0.0f;
            for (int j = 0; j < 7; j++) {
                float d = blob_hu[j] - node_hu[j];
                hu_dist += d * d;
            }
            hu_dist = std::sqrt(hu_dist);
            if (hu_dist < best_dist) {
                second_dist = best_dist;
                best_dist   = hu_dist;
                best_node   = node.id;
                best_offset = node.centroid_canvas - blob.centroid;
            } else if (hu_dist < second_dist) {
                second_dist = hu_dist;
            }
        }

        // [2] Uniqueness (Lowe's ratio): reject ambiguous matches
        if (best_node < 0) continue;
        if (second_dist < (float)kHuDistanceThreshold &&
            best_dist / second_dist > kHuUniquenessRatio) continue;

        candidates.push_back({i, best_node, best_offset, best_dist});
    }

    if (candidates.empty()) return {};

    // [3] One-to-one: keep only the best blob per node
    std::unordered_map<int, int> node_to_best; // node_id -> candidate index
    for (int i = 0; i < (int)candidates.size(); i++) {
        auto it = node_to_best.find(candidates[i].node_id);
        if (it == node_to_best.end() || candidates[i].hu_dist < candidates[it->second].hu_dist)
            node_to_best[candidates[i].node_id] = i;
    }
    std::vector<Candidate> unique_candidates;
    unique_candidates.reserve(node_to_best.size());
    for (const auto& p : node_to_best)
        unique_candidates.push_back(candidates[p.second]);
    candidates = std::move(unique_candidates);

    if (candidates.empty()) return {};

    // Median vote for consensus frame offset
    std::vector<float> dxs, dys;
    dxs.reserve(candidates.size()); dys.reserve(candidates.size());
    for (const auto& c : candidates) { dxs.push_back(c.offset.x); dys.push_back(c.offset.y); }
    std::sort(dxs.begin(), dxs.end()); std::sort(dys.begin(), dys.end());
    cv::Point2f median(dxs[dxs.size()/2], dys[dys.size()/2]);

    // Tag inliers
    const float tol2 = kRansacTolerancePx * kRansacTolerancePx;
    for (const auto& c : candidates) {
        cv::Point2f diff = c.offset - median;
        if (diff.x*diff.x + diff.y*diff.y <= tol2) {
            blobs[c.blob_idx].matched_node_id = c.node_id;
            blobs[c.blob_idx].matched_offset  = c.offset;
        }
    }

    return median;
}

// ============================================================================
//  SECTION 10: Graph Update
// ============================================================================

void WhiteboardCanvas::RemoveNodeFromGraph(WhiteboardGroup& group, int node_id) {
    auto it = group.nodes.find(node_id);
    if (it == group.nodes.end()) return;
    group.spatial_index.Remove(node_id, it->second->centroid_canvas);
    group.nodes.erase(it);
}

bool WhiteboardCanvas::UpdateGraph(WhiteboardGroup& group,
                                    std::vector<FrameBlob>& blobs,
                                    int current_frame, cv::Point2f frame_offset,
                                    const cv::Rect& lecturer_canvas_rect) {
    bool graph_changed = false;
    const cv::Rect cropped_frame = BuildCroppedFrameRect(frame_w_, frame_h_);

    // --- 5a. Refresh matched nodes ---
    std::unordered_set<int> seen_node_ids;
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
        seen_node_ids.insert(node.id);

        // Update absence score (node is healthy)
        node.absence_score = std::min(kAbsenceScoreMax, node.absence_score + kAbsenceIncrement);

        if (!node.user_locked) {
            const MaskRelation rel = ComputeMaskRelation(
                canvas_bbox, blob.binary_mask, canvas_centroid,
                node.bbox_canvas, node.binary_mask, node.centroid_canvas);
            if (rel.valid && rel.overlap_over_min >= kBattleRefreshOverlap) {
                float shift = (float)cv::norm(canvas_centroid - node.centroid_canvas);
                if (blob.area > node.area || shift > kBattleRefreshShiftPx) {
                    RefreshNodeFromBlob(group, node, blob, canvas_centroid, canvas_bbox);
                    graph_changed = true;
                }
            }
        }
    }

    // --- 5b. Absence tracking ---
    // Penalise unseen nodes that are near a matched node (visible area proxy).
    {
        std::vector<int> to_remove;
        for (auto& pair : group.nodes) {
            int nid = pair.first;
            auto& node = *pair.second;
            if (seen_node_ids.count(nid)) continue;

            const cv::Point2i cp((int)std::round(node.centroid_canvas.x),
                                  (int)std::round(node.centroid_canvas.y));
            if (lecturer_canvas_rect.width > 0 && lecturer_canvas_rect.contains(cp)) continue;

            bool near_match = false;
            for (int sid : seen_node_ids) {
                auto sit = group.nodes.find(sid);
                if (sit == group.nodes.end()) continue;
                if ((float)cv::norm(node.centroid_canvas - sit->second->centroid_canvas)
                        < kAbsenceNearbyRadius) { near_match = true; break; }
            }
            if (!near_match) continue;

            node.absence_score -= kAbsenceDecrement;
            if (node.absence_score < 0.0f && !node.user_locked)
                to_remove.push_back(nid);
        }
        for (int nid : to_remove) { RemoveNodeFromGraph(group, nid); graph_changed = true; }
    }

    // --- 5c. Add unmatched blobs (only when frame_offset is trustworthy) ---
    const int matched_count = (int)seen_node_ids.size();
    if (matched_count >= kMinMatchesForNewNode) {
        for (auto& blob : blobs) {
            if (blob.matched_node_id >= 0) continue;

            // Skip blobs whose centroid falls outside the cropped frame region
            if (!cropped_frame.contains(cv::Point((int)blob.centroid.x, (int)blob.centroid.y)))
                continue;

            const cv::Point2f canvas_centroid = blob.centroid + frame_offset;
            const cv::Rect canvas_bbox(
                blob.bbox.x + (int)std::round(frame_offset.x),
                blob.bbox.y + (int)std::round(frame_offset.y),
                blob.bbox.width, blob.bbox.height);

            // Duplicate check: positional IoU, with centroid-aligned fallback.
            // Same-frame nodes are excluded (they may be legitimately similar strokes).
            const float search_r = std::max({(float)canvas_bbox.width,
                                               (float)canvas_bbox.height,
                                               kMergeSearchRadiusPx});
            const auto nearby = group.spatial_index.QueryRadius(canvas_centroid, search_r);
            bool is_dup = false;
            for (int nid : nearby) {
                if (group.user_deleted_ids.count(nid)) continue;
                auto nit = group.nodes.find(nid);
                if (nit == group.nodes.end()) continue;
                const auto& existing = *nit->second;
                if (existing.created_frame == current_frame) continue;

                float ov = BestMaskOverlap(
                    existing.bbox_canvas, existing.binary_mask, existing.centroid_canvas,
                    canvas_bbox,          blob.binary_mask,      canvas_centroid);

                if (ov > kDuplicateOverlapThreshold) {
                    is_dup = true;
                    if (!existing.user_locked && blob.area > existing.area) {
                        auto& node = *nit->second;
                        node.last_seen_frame = current_frame;
                        RefreshNodeFromBlob(group, node, blob, canvas_centroid, canvas_bbox);
                        graph_changed = true;
                    }
                    break;
                }
            }

            if (!is_dup) {
                auto node = std::make_unique<DrawingNode>();
                node->id = group.next_node_id++;
                node->binary_mask = blob.binary_mask.clone();
                if (!blob.color_pixels.empty()) node->color_pixels = blob.color_pixels.clone();
                node->bbox_canvas    = canvas_bbox;
                node->centroid_canvas = canvas_centroid;
                node->contour = blob.contour;
                std::copy(blob.hu, blob.hu + 7, node->hu);
                node->area = blob.area;
                node->absence_score = kAbsenceScoreInitial;
                node->last_seen_frame = current_frame;
                node->created_frame   = current_frame;
                const int nid = node->id;
                group.spatial_index.Insert(nid, canvas_centroid);
                group.nodes[nid] = std::move(node);
                graph_changed = true;
            }
        }
    }

    // --- 5d. Containment removal + overlap merge pass (every 4 frames) ---
    if (current_frame % 4 == 0) {
        std::vector<int> all_ids;
        all_ids.reserve(group.nodes.size());
        for (auto& pair : group.nodes) all_ids.push_back(pair.first);

        std::unordered_set<int> removed;
        for (size_t i = 0; i < all_ids.size(); ++i) {
            int aid = all_ids[i];
            if (removed.count(aid)) continue;
            auto ait = group.nodes.find(aid);
            if (ait == group.nodes.end()) continue;
            auto& a = *ait->second;

            const float search_r = std::max({(float)a.bbox_canvas.width,
                                               (float)a.bbox_canvas.height,
                                               kMergeSearchRadiusPx});
            auto nearby = group.spatial_index.QueryRadius(a.centroid_canvas, search_r);
            for (int bid : nearby) {
                if (bid == aid || removed.count(bid)) continue;
                auto bit = group.nodes.find(bid);
                if (bit == group.nodes.end()) continue;
                auto& b = *bit->second;

                // Skip nodes created in the same frame (e.g. "T" and "-")
                if (a.created_frame == b.created_frame) continue;

                MaskRelation rel = ComputeMaskRelation(
                    a.bbox_canvas, a.binary_mask, a.centroid_canvas,
                    b.bbox_canvas, b.binary_mask, b.centroid_canvas);
                if (!rel.valid || rel.overlap_px == 0) continue;

                // Identify smaller/larger by mask pixel count
                bool a_is_smaller = (rel.first_px <= rel.second_px);
                int smaller_id = a_is_smaller ? aid : bid;
                int larger_id  = a_is_smaller ? bid : aid;

                // Tier 1: Containment — smaller node is mostly inside larger → remove smaller
                if (rel.overlap_over_min >= kContainmentRemoveThreshold) {
                    removed.insert(smaller_id);
                    RemoveNodeFromGraph(group, smaller_id);
                    graph_changed = true;
                    if (smaller_id == aid) break;
                    continue;
                }

                // Tier 2: Overlap merge — significant shared area → combine with alignment
                if (rel.overlap_over_min >= kOverlapMergeThreshold) {
                    auto lit = group.nodes.find(larger_id);
                    auto sit = group.nodes.find(smaller_id);
                    if (lit != group.nodes.end() && sit != group.nodes.end()) {
                        MergeNodes(group, *lit->second, *sit->second,
                                   kAlignSearchRadius, kAlignmentMode);
                        removed.insert(smaller_id);
                        RemoveNodeFromGraph(group, smaller_id);
                        graph_changed = true;
                        if (smaller_id == aid) break;
                    }
                }
            }
        }
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
        if (!node.color_pixels.empty()) {
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
    for (const auto& p : group.nodes) sorted.push_back(p.second.get());
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
        if (!p.second->color_pixels.empty()) sorted.push_back(p.second.get());
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
        if (sx0+sw > node->color_pixels.cols || sy0+sh > node->color_pixels.rows) continue;
        if (sx0+sw > node->binary_mask.cols  || sy0+sh > node->binary_mask.rows)  continue;
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
    group.next_node_id = 0;
    for (const auto& blob : blobs) {
        auto node = std::make_unique<DrawingNode>();
        node->id = group.next_node_id++;
        node->binary_mask = blob.binary_mask.clone();
        if (!blob.color_pixels.empty()) node->color_pixels = blob.color_pixels.clone();
        node->bbox_canvas    = blob.bbox;
        node->centroid_canvas = blob.centroid;
        node->contour = blob.contour;
        std::copy(blob.hu, blob.hu + 7, node->hu);
        node->area = blob.area;
        node->absence_score = kAbsenceScoreInitial;
        node->last_seen_frame = current_frame;
        node->created_frame   = current_frame;
        int nid = node->id;
        group.spatial_index.Insert(nid, blob.centroid);
        group.nodes[nid] = std::move(node);
    }
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

int WhiteboardCanvas::GetGraphNodeNeighbors(int /*node_id*/, int* /*neighbors*/,
                                              int /*max_neighbors*/) const {
    return 0;  // neighbor graph removed
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

    double sd = 1e9;
    if (!a.contour.empty() && !b.contour.empty())
        sd = cv::matchShapes(a.contour, b.contour, cv::CONTOURS_MATCH_I2, 0);
    result[0] = (float)sd;

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
    float ua = (float)(a.bbox_canvas.area() + b.bbox_canvas.area() - isect.area());
    result[5] = ua > 0 ? (float)isect.area() / ua : 0.0f;
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
        if (node.color_pixels.empty() || node.binary_mask.empty()) continue;
        int w = node.color_pixels.cols, h = node.color_pixels.rows;
        int pbytes = w * h * 4, hbytes = 12;
        if (offset + hbytes + pbytes > max_bytes) break;
        int* hdr = reinterpret_cast<int*>(buffer + offset);
        hdr[0] = node.id; hdr[1] = w; hdr[2] = h;
        offset += hbytes;
        for (int y = 0; y < h; y++) {
            const uint8_t* mr = node.binary_mask.ptr<uint8_t>(y);
            const uint8_t* cr = node.color_pixels.ptr<uint8_t>(y);
            uint8_t* or_ = buffer + offset + y * w * 4;
            for (int x = 0; x < w; x++) {
                if (mr[x]) {
                    or_[x*4+0] = cr[x*3+2]; or_[x*4+1] = cr[x*3+1];
                    or_[x*4+2] = cr[x*3+0]; or_[x*4+3] = 255;
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
    group.spatial_index.Remove(node.id, node.centroid_canvas);
    node.centroid_canvas.x = new_cx; node.centroid_canvas.y = new_cy;
    node.bbox_canvas.x += (int)std::round(dx);
    node.bbox_canvas.y += (int)std::round(dy);
    group.spatial_index.Insert(node.id, node.centroid_canvas);
    node.user_locked = true;
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
        group.spatial_index.Remove(node.id, node.centroid_canvas);
        node.centroid_canvas.x = ncx; node.centroid_canvas.y = ncy;
        node.bbox_canvas.x += (int)std::round(dx);
        node.bbox_canvas.y += (int)std::round(dy);
        group.spatial_index.Insert(node.id, node.centroid_canvas);
        node.user_locked = true;
        changed = true;
    }

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
