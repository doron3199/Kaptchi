// ============================================================================
// whiteboard_canvas.cpp -- Graph-Based Canvas Stitcher
//
// PIPELINE OVERVIEW (runs on worker thread, once per accepted frame):
//
//   +---------------+
//   | [1] Motion    |  Frame-diff vs previous. Skip if motion is too small
//   |     Gate      |  or too large; still frames force processing after 8 skips.
//   +-------+-------+
//           |
//   +-------v-------+
//   | [2] No-Update |  Mandatory person mask plus fixed edge exclusions define
//   |     Mask      |  a no-update / no-erase zone for the rest of the pipeline.
//   +-------+-------+
//           |
//   +-------v-------+
//   | [3] Enhance   |  adaptiveThreshold -> binary mask.
//   |   + Binarize  |
//   +-------+-------+
//           |
//   +-------v-------+
//   | [4] Graph     |  Extract a frame graph, match nodes by long/short-edge
//   |   Matching    |  similarity + edge-distance consistency.
//   +-------+-------+
//           |
//   +-------v-------+  Battle logic: coexist/refresh/replace per-entity.
//   | [5] Graph     |  Absence tracking per-node for natural erasure.
//   |   Update      |
//   +---------------+
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
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_set>

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

using SteadyClock = std::chrono::steady_clock;

static void BuildFrameBlobNeighborGraph(std::vector<FrameBlob>& blobs, int k);

// ---------------------------------------------------------------------------
// EnhanceFrameBlobs — apply WhiteboardEnhance to each blob's color_pixels
// so the raw render cache shows clean dark strokes on white background.
// Each blob bbox is padded before enhancement to avoid DoG edge artifacts,
// then cropped back to the original bbox.
// A negative threshold disables enhancement entirely.
// ---------------------------------------------------------------------------
static constexpr int kEnhancePadding = 10; // > DOG_K/2 = 7

static void EnhanceFrameBlobs(std::vector<FrameBlob>& blobs,
                               const cv::Mat& frame_bgr,
                               float threshold) {
    if (threshold < 0.0f || frame_bgr.empty()) return;
    for (auto& blob : blobs) {
        if (blob.color_pixels.empty()) continue;
        // Expand bbox by padding, clamp to frame bounds
        int px0 = std::max(0, blob.bbox.x - kEnhancePadding);
        int py0 = std::max(0, blob.bbox.y - kEnhancePadding);
        int px1 = std::min(frame_bgr.cols, blob.bbox.x + blob.bbox.width + kEnhancePadding);
        int py1 = std::min(frame_bgr.rows, blob.bbox.y + blob.bbox.height + kEnhancePadding);
        cv::Rect padded(px0, py0, px1 - px0, py1 - py0);
        cv::Mat enhanced = WhiteboardEnhance(frame_bgr(padded).clone(), threshold);
        // Crop back to original bbox within the padded result
        int cx = blob.bbox.x - px0;
        int cy = blob.bbox.y - py0;
        blob.color_pixels = enhanced(cv::Rect(cx, cy,
                                               blob.bbox.width, blob.bbox.height)).clone();
    }
}

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

static double ElapsedMs(const SteadyClock::time_point& start,
                        const SteadyClock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
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

static cv::Point2f GetGroupBoundsCenter(const WhiteboardGroup& group) {
    return cv::Point2f(
        (static_cast<float>(group.stroke_min_px_x) +
         static_cast<float>(group.stroke_max_px_x)) *
            0.5f,
        (static_cast<float>(group.stroke_min_px_y) +
         static_cast<float>(group.stroke_max_px_y)) *
            0.5f);
}

static float RatioSimilarity(float a, float b) {
    if (a <= 0.0f || b <= 0.0f) return 0.0f;
    return std::min(a, b) / std::max(a, b);
}

static float LongEdgeSimilarity(const cv::Rect& a, const cv::Rect& b) {
    return RatioSimilarity(
        static_cast<float>(std::max(a.width, a.height)),
        static_cast<float>(std::max(b.width, b.height)));
}

static float ShortEdgeSimilarity(const cv::Rect& a, const cv::Rect& b) {
    return RatioSimilarity(
        static_cast<float>(std::min(a.width, a.height)),
        static_cast<float>(std::min(b.width, b.height)));
}

static cv::Point2f ComputeGravityCenter(const cv::Mat& mask) {
    if (mask.empty()) return cv::Point2f(0, 0);
    int sum_x = 0, sum_y = 0, count = 0;
    for (int y = 0; y < mask.rows; ++y) {
        for (int x = 0; x < mask.cols; ++x) {
            if (mask.at<uchar>(y, x) > 0) {
                sum_x += x;
                sum_y += y;
                ++count;
            }
        }
    }
    if (count > 0) {
        return cv::Point2f(sum_x / (float)count, sum_y / (float)count);
    }
    return cv::Point2f(0, 0);
}

// ---------------------------------------------------------------------------
// KD-Tree + RANSAC matching types and functions
// ---------------------------------------------------------------------------

struct KdTreeCandidate {
    int source_index;      // blob index (per-frame) or node_id (snapshot)
    int target_id;         // graph node_id
    cv::Point2f offset;    // target.centroid - source.centroid
    float hu_dist;         // L2 distance in log-Hu space
    float bbox_sim;        // average of long+short edge similarity
};

struct GraphMatchResult {
    cv::Point2f offset{0, 0};
    std::vector<std::pair<int, int>> inlier_pairs;  // (source_index, target_id)
    int vote_count = 0;
    bool valid = false;
};

static std::array<float, 7> ComputeLogHuFeatures(const double hu[7]) {
    std::array<float, 7> result;
    for (int i = 0; i < 7; i++) {
        const double h = hu[i];
        const double abs_h = std::abs(h);
        if (abs_h < 1e-30) {
            result[i] = -30.0f;
        } else {
            const float sign = (h < 0.0) ? -1.0f : 1.0f;
            result[i] = sign * static_cast<float>(std::log10(abs_h));
        }
    }
    return result;
}

static GraphMatchResult MatchWithKdTreeRansac(
    const cv::Mat& source_features,       // N x 7, CV_32F
    const std::vector<cv::Point2f>& source_centroids,
    const std::vector<cv::Rect>& source_bboxes,
    const std::vector<int>& source_ids,   // maps row -> source id
    const cv::Mat& target_features,       // M x 7, CV_32F
    const std::vector<cv::Point2f>& target_centroids,
    const std::vector<cv::Rect>& target_bboxes,
    const std::vector<int>& target_ids,   // maps row -> target id
    double hu_threshold,
    float bbox_similarity,
    float ransac_tolerance,
    int ransac_max_iterations,
    int min_inliers,
    int knn_k = 1,
    const cv::Point2f& prior_offset = cv::Point2f(0, 0)) {

    GraphMatchResult result;
    const int N = source_features.rows;
    const int M = target_features.rows;
    if (N == 0 || M == 0) return result;

    // Clamp k to available targets
    const int k = std::min(knn_k, M);

    // Build KD-Tree on target features
    cv::flann::Index kd_tree(target_features, cv::flann::KDTreeIndexParams(1));

    // Find candidates via k-NN search
    std::vector<KdTreeCandidate> candidates;
    candidates.reserve(N * k);

    cv::Mat indices(1, k, CV_32S);
    cv::Mat dists(1, k, CV_32F);

    for (int i = 0; i < N; i++) {
        cv::Mat query = source_features.row(i);
        kd_tree.knnSearch(query, indices, dists, k);

        for (int n = 0; n < k; n++) {
            const float l2_dist = std::sqrt(dists.at<float>(0, n));
            if (l2_dist >= static_cast<float>(hu_threshold)) continue;

            const int target_row = indices.at<int>(0, n);
            if (target_row < 0 || target_row >= M) continue;

            const float long_sim = LongEdgeSimilarity(source_bboxes[i], target_bboxes[target_row]);
            const float short_sim = ShortEdgeSimilarity(source_bboxes[i], target_bboxes[target_row]);
            if (long_sim < bbox_similarity || short_sim < bbox_similarity) continue;

            KdTreeCandidate c;
            c.source_index = source_ids[i];
            c.target_id = target_ids[target_row];
            c.offset = target_centroids[target_row] - source_centroids[i];
            c.hu_dist = l2_dist;
            c.bbox_sim = (long_sim + short_sim) * 0.5f;
            candidates.push_back(c);
        }
    }

    if (candidates.empty()) return result;

    // RANSAC: sweep all candidates as hypotheses
    const int num_iterations = std::min(
        static_cast<int>(candidates.size()), ransac_max_iterations);

    int best_inlier_count = 0;
    cv::Point2f best_offset(0, 0);
    std::vector<int> best_inlier_indices;

    const float tol2 = ransac_tolerance * ransac_tolerance;

    for (int iter = 0; iter < num_iterations; iter++) {
        const cv::Point2f hypothesis = candidates[iter].offset;

        std::vector<int> inlier_indices;
        inlier_indices.reserve(candidates.size());

        for (int j = 0; j < static_cast<int>(candidates.size()); j++) {
            const cv::Point2f predicted = candidates[j].offset;
            const cv::Point2f diff = predicted - hypothesis;
            if (diff.x * diff.x + diff.y * diff.y <= tol2) {
                inlier_indices.push_back(j);
            }
        }

        const int inlier_count = static_cast<int>(inlier_indices.size());
        bool is_better = false;
        if (inlier_count > best_inlier_count) {
            is_better = true;
        } else if (inlier_count == best_inlier_count && inlier_count > 0) {
            // Tiebreaker: prefer hypothesis closer to prior_offset
            const float dist_new = static_cast<float>(cv::norm(hypothesis - prior_offset));
            const float dist_old = static_cast<float>(cv::norm(best_offset - prior_offset));
            if (dist_new < dist_old) is_better = true;
        }

        if (is_better) {
            best_inlier_count = inlier_count;
            best_inlier_indices = std::move(inlier_indices);

            // Refine with median offset from inliers
            std::vector<float> dxs, dys;
            dxs.reserve(best_inlier_indices.size());
            dys.reserve(best_inlier_indices.size());
            for (int idx : best_inlier_indices) {
                dxs.push_back(candidates[idx].offset.x);
                dys.push_back(candidates[idx].offset.y);
            }
            std::sort(dxs.begin(), dxs.end());
            std::sort(dys.begin(), dys.end());
            const int mid = static_cast<int>(dxs.size() / 2);
            best_offset = cv::Point2f(dxs[mid], dys[mid]);
        }
    }

    if (best_inlier_count < min_inliers) return result;

    result.valid = true;
    result.offset = best_offset;
    result.vote_count = best_inlier_count;
    result.inlier_pairs.reserve(best_inlier_indices.size());
    for (int idx : best_inlier_indices) {
        result.inlier_pairs.emplace_back(
            candidates[idx].source_index, candidates[idx].target_id);
    }

    return result;
}

static float RawDimensionRatio(int numerator, int denominator) {
    if (denominator <= 0) return 0.0f;
    return static_cast<float>(numerator) / static_cast<float>(denominator);
}

static int CopyGraphNodesToBuffer(const WhiteboardGroup& group,
                                  float* buffer,
                                  int max_nodes) {
    if (!buffer || max_nodes <= 0) return 0;

    int count = 0;
    for (const auto& pair : group.nodes) {
        if (count >= max_nodes) break;

        const DrawingNode& node = *pair.second;
        float* cursor = buffer + count * 15;
        cursor[0] = static_cast<float>(node.id);
        cursor[1] = static_cast<float>(node.bbox_canvas.x);
        cursor[2] = static_cast<float>(node.bbox_canvas.y);
        cursor[3] = static_cast<float>(node.bbox_canvas.width);
        cursor[4] = static_cast<float>(node.bbox_canvas.height);
        cursor[5] = node.centroid_canvas.x;
        cursor[6] = node.centroid_canvas.y;
        cursor[7] = static_cast<float>(node.area);
        cursor[8] = node.absence_score;
        cursor[9] = static_cast<float>(node.last_seen_frame);
        cursor[10] = static_cast<float>(node.created_frame);
        cursor[11] = static_cast<float>(node.neighbor_ids.size());
        cursor[12] = static_cast<float>(group.stroke_min_px_x);
        cursor[13] = static_cast<float>(group.stroke_min_px_y);
        cursor[14] = static_cast<float>(node.match_distance);
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
                                     float* buffer,
                                     int max_floats) {
    if (!buffer || max_floats <= 0) return 0;

    int written = 0;
    for (const auto& pair : group.nodes) {
        const DrawingNode& node = *pair.second;
        const int num_points = static_cast<int>(node.contour.size());
        const int needed = 2 + num_points * 2;
        if (written + needed > max_floats) break;

        buffer[written++] = static_cast<float>(node.id);
        buffer[written++] = static_cast<float>(num_points);

        const int origin_x = node.bbox_canvas.x;
        const int origin_y = node.bbox_canvas.y;
        for (const auto& point : node.contour) {
            buffer[written++] = static_cast<float>(point.x + origin_x);
            buffer[written++] = static_cast<float>(point.y + origin_y);
        }
    }

    return written;
}

static bool FillSnapshotNodeComparisonResult(const DrawingNode& first,
                                             const DrawingNode& second,
                                             float* result) {
    if (!result) return false;

    double shape_dist = 1e9;
    if (!first.contour.empty() && !second.contour.empty()) {
        shape_dist = cv::matchShapes(
            first.contour, second.contour, cv::CONTOURS_MATCH_I2, 0);
    }

    const float width_ratio = RawDimensionRatio(
        first.bbox_canvas.width, std::max(1, second.bbox_canvas.width));
    const float height_ratio = RawDimensionRatio(
        first.bbox_canvas.height, std::max(1, second.bbox_canvas.height));
    const float long_edge_similarity =
        LongEdgeSimilarity(first.bbox_canvas, second.bbox_canvas);
    const float short_edge_similarity =
        ShortEdgeSimilarity(first.bbox_canvas, second.bbox_canvas);

    result[0] = static_cast<float>(shape_dist);
    result[1] = width_ratio;
    result[2] = height_ratio;
    result[3] = long_edge_similarity;
    result[4] = short_edge_similarity;
    result[5] = (long_edge_similarity + short_edge_similarity) * 0.5f;
    return true;
}

static std::unique_ptr<WhiteboardGroup> CloneGraphGroup(const WhiteboardGroup& source) {
    auto clone = std::make_unique<WhiteboardGroup>();
    clone->debug_id = source.debug_id;
    clone->stroke_min_px_x = source.stroke_min_px_x;
    clone->stroke_min_px_y = source.stroke_min_px_y;
    clone->stroke_max_px_x = source.stroke_max_px_x;
    clone->stroke_max_px_y = source.stroke_max_px_y;
    clone->raw_min_px_x = source.raw_min_px_x;
    clone->raw_min_px_y = source.raw_min_px_y;
    clone->raw_max_px_x = source.raw_max_px_x;
    clone->raw_max_px_y = source.raw_max_px_y;
    clone->fixed_render_height = source.fixed_render_height;
    clone->last_lecturer_rect = source.last_lecturer_rect;

    std::vector<int> node_ids;
    node_ids.reserve(source.nodes.size());
    for (const auto& pair : source.nodes) {
        node_ids.push_back(pair.first);
    }
    std::sort(node_ids.begin(), node_ids.end());

    int next_node_id = 0;
    for (int node_id : node_ids) {
        auto it = source.nodes.find(node_id);
        if (it == source.nodes.end()) continue;

        const DrawingNode& source_node = *it->second;
        auto node = std::make_unique<DrawingNode>();
        node->id = source_node.id;
        node->binary_mask = source_node.binary_mask.clone();
        if (!source_node.color_pixels.empty()) {
            node->color_pixels = source_node.color_pixels.clone();
        }
        node->bbox_canvas = source_node.bbox_canvas;
        node->centroid_canvas = source_node.centroid_canvas;
        node->contour = source_node.contour;
        std::copy(source_node.hu, source_node.hu + 7, node->hu);
        node->area = source_node.area;
        node->max_area_seen = source_node.max_area_seen;
        node->absence_score = source_node.absence_score;
        node->last_seen_frame = source_node.last_seen_frame;
        node->created_frame = source_node.created_frame;
        node->seen_count = source_node.seen_count;
        node->in_view_count = source_node.in_view_count;
        node->match_distance = source_node.match_distance;
        node->neighbor_ids = source_node.neighbor_ids;

        clone->spatial_index.Insert(node->id, node->centroid_canvas);
        next_node_id = std::max(next_node_id, node->id + 1);
        clone->nodes[node->id] = std::move(node);
    }

    clone->next_node_id = next_node_id;
    clone->next_candidate_id = 0;
    clone->stroke_cache_dirty = true;
    clone->raw_cache_dirty = true;
    return clone;
}

static void AppendShiftedNodeToGroup(WhiteboardGroup& target,
                                     const DrawingNode& source,
                                     const cv::Point2f& translation,
                                     int node_id,
                                     int current_frame) {
    auto node = std::make_unique<DrawingNode>();
    node->id = node_id;
    node->binary_mask = source.binary_mask.clone();
    if (!source.color_pixels.empty()) {
        node->color_pixels = source.color_pixels.clone();
    }
    node->bbox_canvas = cv::Rect(
        source.bbox_canvas.x + static_cast<int>(std::round(translation.x)),
        source.bbox_canvas.y + static_cast<int>(std::round(translation.y)),
        source.bbox_canvas.width,
        source.bbox_canvas.height);
    node->centroid_canvas = source.centroid_canvas + translation;
    node->contour = source.contour;
    std::copy(source.hu, source.hu + 7, node->hu);
    node->area = source.area;
    node->max_area_seen = std::max(source.max_area_seen, source.area);
    node->absence_score = kAbsenceScoreInitial;
    node->last_seen_frame = current_frame;
    node->created_frame = current_frame;
    node->seen_count = std::max(1, source.seen_count);
    node->in_view_count = source.in_view_count;

    target.spatial_index.Insert(node_id, node->centroid_canvas);
    target.nodes[node_id] = std::move(node);
}

static void AppendUniqueId(std::vector<int>& ids, int id) {
    if (id < 0) return;
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
    }
}

static void DeduplicateIds(std::vector<int>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

static int AppendFrameBlobToGroup(WhiteboardGroup& target,
                                  const FrameBlob& blob,
                                  const cv::Point2f& translation,
                                  const std::vector<int>& neighbor_ids,
                                  int current_frame) {
    auto node = std::make_unique<DrawingNode>();
    node->id = target.next_node_id++;
    node->binary_mask = blob.binary_mask.clone();
    if (!blob.color_pixels.empty()) {
        node->color_pixels = blob.color_pixels.clone();
    }
    node->bbox_canvas = cv::Rect(
        blob.bbox.x + static_cast<int>(std::round(translation.x)),
        blob.bbox.y + static_cast<int>(std::round(translation.y)),
        blob.bbox.width,
        blob.bbox.height);
    node->centroid_canvas = blob.centroid + translation;
    node->contour = blob.contour;
    std::copy(blob.hu, blob.hu + 7, node->hu);
    node->area = blob.area;
    node->max_area_seen = blob.area;
    node->absence_score = kAbsenceScoreInitial;
    node->last_seen_frame = current_frame;
    node->created_frame = current_frame;
    node->seen_count = 1;
    node->neighbor_ids = neighbor_ids;
    DeduplicateIds(node->neighbor_ids);

    const int node_id = node->id;
    target.spatial_index.Insert(node_id, node->centroid_canvas);
    target.nodes[node_id] = std::move(node);
    return node_id;
}

static cv::Rect BuildCroppedFrameRect(int frame_w, int frame_h) {
    const int crop_top = static_cast<int>(frame_h * 0.15);
    const int crop_bottom = frame_h - static_cast<int>(frame_h * 0.95);
    const int crop_left = static_cast<int>(frame_w * 0.05);
    const int crop_right = frame_w - static_cast<int>(frame_w * 0.95);
    return cv::Rect(
        crop_left,
        crop_top,
        std::max(1, frame_w - crop_left - crop_right),
        std::max(1, frame_h - crop_top - crop_bottom));
}

static cv::Rect ComputeMaskBoundingRect(const cv::Mat& mask) {
    if (mask.empty() || mask.type() != CV_8UC1) {
        return cv::Rect();
    }

    if (cv::countNonZero(mask) <= 0) {
        return cv::Rect();
    }

    return cv::boundingRect(mask);
}

static cv::Rect TranslateFrameRectToCanvas(const cv::Rect& frame_rect,
                                           const cv::Point2f& camera_offset) {
    if (frame_rect.width <= 0 || frame_rect.height <= 0) {
        return cv::Rect();
    }

    return cv::Rect(
        frame_rect.x + static_cast<int>(std::round(camera_offset.x)),
        frame_rect.y + static_cast<int>(std::round(camera_offset.y)),
        frame_rect.width,
        frame_rect.height);
}

static cv::Rect ComputeProcessingRoi(const cv::Size& frame_size) {
    if (frame_size.width <= 0 || frame_size.height <= 0) {
        return cv::Rect();
    }

    const int top = frame_size.height / 20;
    const int left = frame_size.width / 20;
    const int bottom = std::max(top + 1, frame_size.height - top);
    const int right = std::max(left + 1, frame_size.width - left);
    return cv::Rect(left, top, right - left, bottom - top);
}

static cv::Mat CropPersonMaskForProcessing(const cv::Mat& person_mask,
                                           const cv::Rect& roi) {
    if (person_mask.empty() || person_mask.type() != CV_8UC1 ||
        roi.width <= 0 || roi.height <= 0) {
        return cv::Mat();
    }

    const cv::Rect mask_bounds(0, 0, person_mask.cols, person_mask.rows);
    const cv::Rect clipped_roi = roi & mask_bounds;
    if (clipped_roi.width != roi.width || clipped_roi.height != roi.height) {
        return cv::Mat();
    }

    return person_mask(clipped_roi).clone();
}

static cv::Rect ExpandRectWithinFrame(const cv::Rect& rect,
                                      const cv::Size& frame_size,
                                      int pad_x,
                                      int pad_y) {
    if (rect.width <= 0 || rect.height <= 0 ||
        frame_size.width <= 0 || frame_size.height <= 0) {
        return cv::Rect();
    }

    const int x0 = std::max(0, rect.x - std::max(0, pad_x));
    const int y0 = std::max(0, rect.y - std::max(0, pad_y));
    const int x1 = std::min(frame_size.width,
                            rect.x + rect.width + std::max(0, pad_x));
    const int y1 = std::min(frame_size.height,
                            rect.y + rect.height + std::max(0, pad_y));
    if (x1 <= x0 || y1 <= y0) {
        return cv::Rect();
    }

    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

static cv::Mat BuildFrameStrokeRejectMask(const cv::Size& frame_size,
                                          const cv::Rect& lecturer_frame_rect) {
    if (frame_size.width <= 0 || frame_size.height <= 0) {
        return cv::Mat();
    }

    cv::Mat reject_mask(frame_size, CV_8UC1, cv::Scalar(0));
    const int side_margin = std::min(frame_size.width,
                                     std::max(5, frame_size.width / 100));
    reject_mask(cv::Rect(0, 0, side_margin, frame_size.height)).setTo(255);

    const int right_start = std::max(0, frame_size.width - side_margin);
    reject_mask(cv::Rect(right_start, 0,
                         frame_size.width - right_start,
                         frame_size.height)).setTo(255);

    const int lecturer_margin_x = std::max(5, frame_size.width / 100);
    const int lecturer_margin_y = std::max(5, frame_size.height / 100);
    const cv::Rect expanded_lecturer = ExpandRectWithinFrame(
        lecturer_frame_rect,
        frame_size,
        lecturer_margin_x,
        lecturer_margin_y);
    if (expanded_lecturer.width > 0 && expanded_lecturer.height > 0) {
        reject_mask(expanded_lecturer).setTo(255);
    }

    return reject_mask;
}

static bool BlobTouchesRejectMask(const FrameBlob& blob,
                                  const cv::Mat& reject_mask) {
    if (reject_mask.empty() || blob.binary_mask.empty()) {
        return false;
    }

    const cv::Rect mask_bounds(0, 0, reject_mask.cols, reject_mask.rows);
    const cv::Rect clipped_bbox = blob.bbox & mask_bounds;
    if (clipped_bbox.width <= 0 || clipped_bbox.height <= 0) {
        return false;
    }

    const cv::Rect blob_roi(
        clipped_bbox.x - blob.bbox.x,
        clipped_bbox.y - blob.bbox.y,
        clipped_bbox.width,
        clipped_bbox.height);
    if (blob_roi.x < 0 || blob_roi.y < 0 ||
        blob_roi.x + blob_roi.width > blob.binary_mask.cols ||
        blob_roi.y + blob_roi.height > blob.binary_mask.rows) {
        return false;
    }

    cv::Mat overlap_mask;
    cv::bitwise_and(blob.binary_mask(blob_roi),
                    reject_mask(clipped_bbox),
                    overlap_mask);
    return cv::countNonZero(overlap_mask) > 0;
}

static cv::Mat BuildNoUpdateMask(const cv::Mat& gray,
                                 const cv::Mat& person_mask) {
    if (person_mask.empty() || person_mask.size() != gray.size() ||
        person_mask.type() != CV_8UC1) {
        return cv::Mat::zeros(gray.size(), CV_8UC1);
    }

    return person_mask.clone();
}


static cv::Mat BuildBinaryMask(const cv::Mat& gray,
                               const cv::Mat& no_update_mask,
                               int& stroke_pixel_count) {
    // Downscale 2x for faster adaptive threshold (block=51 on full res is ~700ms)
    cv::Mat small_gray;
    cv::resize(gray, small_gray, cv::Size(), 0.5, 0.5, cv::INTER_AREA);

    cv::Mat small_binary;
    cv::adaptiveThreshold(small_gray, small_binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, 25, 6);

    // Remove tiny connected components at half-res (area threshold halved²)
    {
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(small_binary, labels, stats, centroids);
        for (int i = 1; i < n; i++) {
            if (stats.at<int>(i, cv::CC_STAT_AREA) < 3) {
                small_binary.setTo(0, labels == i);
            }
        }
    }

    // Upscale back to original size
    cv::Mat binary;
    cv::resize(small_binary, binary, gray.size(), 0, 0, cv::INTER_NEAREST);

    binary.setTo(0, no_update_mask);

    // Dilate to connect close strokes
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
    cv::dilate(binary, binary, kernel);
    stroke_pixel_count = cv::countNonZero(binary);
    return binary;
}

static bool RenderOverviewToFrame(const cv::Mat& render_cache,
                                  cv::Size view_size,
                                  cv::Mat& out_frame) {
    if (render_cache.empty() || view_size.width <= 0 || view_size.height <= 0) {
        return false;
    }

    out_frame = cv::Mat(view_size.height, view_size.width, CV_8UC3,
                        cv::Scalar(255, 255, 255));

    const float src_aspect = static_cast<float>(render_cache.cols) /
                             static_cast<float>(std::max(1, render_cache.rows));
    const float dst_aspect = static_cast<float>(view_size.width) /
                             static_cast<float>(std::max(1, view_size.height));

    int draw_w = view_size.width;
    int draw_h = view_size.height;
    if (src_aspect > dst_aspect) {
        draw_h = std::max(1, static_cast<int>(std::round(draw_w / src_aspect)));
    } else {
        draw_w = std::max(1, static_cast<int>(std::round(draw_h * src_aspect)));
    }

    cv::Mat scaled;
    cv::resize(render_cache, scaled, cv::Size(draw_w, draw_h),
               0, 0, cv::INTER_AREA);

    const int offset_x = (view_size.width - draw_w) / 2;
    const int offset_y = (view_size.height - draw_h) / 2;
    scaled.copyTo(out_frame(cv::Rect(offset_x, offset_y, draw_w, draw_h)));
    return true;
}

static bool CopyBgrFrameToRgbaBuffer(const cv::Mat& frame_bgr,
                                     uint8_t* buffer,
                                     int width,
                                     int height) {
    if (frame_bgr.empty() || !buffer || width <= 0 || height <= 0) {
        return false;
    }

    cv::Mat frame_rgba;
    cv::cvtColor(frame_bgr, frame_rgba, cv::COLOR_BGR2RGBA);
    cv::Mat buffer_view(height, width, CV_8UC4, buffer);
    frame_rgba.copyTo(buffer_view);
    return true;
}

// ---------------------------------------------------------------------------
// MaskRelation — pixel-level overlap metrics between two masks
// ---------------------------------------------------------------------------
struct MaskRelation {
    bool valid = false;
    int overlap_px = 0;
    int first_px = 0;
    int second_px = 0;
    float overlap_over_min = 0.0f;
    float overlap_over_first = 0.0f;
    float overlap_over_second = 0.0f;
    float iou = 0.0f;
    float area_ratio = 0.0f;
    float centroid_distance = 0.0f;
};

static MaskRelation ComputeMaskRelation(const cv::Rect& first_bbox,
                                        const cv::Mat& first_mask,
                                        const cv::Point2f& first_centroid,
                                        const cv::Rect& second_bbox,
                                        const cv::Mat& second_mask,
                                        const cv::Point2f& second_centroid) {
    MaskRelation relation;
    relation.centroid_distance = static_cast<float>(
        cv::norm(first_centroid - second_centroid));

    if (first_mask.empty() || second_mask.empty()) return relation;
    if (first_mask.cols != first_bbox.width || first_mask.rows != first_bbox.height ||
        second_mask.cols != second_bbox.width || second_mask.rows != second_bbox.height) {
        return relation;
    }

    relation.first_px = cv::countNonZero(first_mask);
    relation.second_px = cv::countNonZero(second_mask);
    if (relation.first_px <= 0 || relation.second_px <= 0) return relation;

    relation.area_ratio = static_cast<float>(
        std::min(relation.first_px, relation.second_px) /
        std::max(1.0, static_cast<double>(std::max(relation.first_px, relation.second_px))));

    const cv::Rect intersection = first_bbox & second_bbox;
    if (intersection.empty()) {
        relation.valid = true;
        return relation;
    }

    const int ux0 = std::min(first_bbox.x, second_bbox.x);
    const int uy0 = std::min(first_bbox.y, second_bbox.y);
    const int ux1 = std::max(first_bbox.x + first_bbox.width,
                             second_bbox.x + second_bbox.width);
    const int uy1 = std::max(first_bbox.y + first_bbox.height,
                             second_bbox.y + second_bbox.height);
    const int uw = ux1 - ux0;
    const int uh = uy1 - uy0;
    if (uw <= 0 || uh <= 0) return relation;

    cv::Mat first_in_union(uh, uw, CV_8UC1, cv::Scalar(0));
    cv::Mat second_in_union(uh, uw, CV_8UC1, cv::Scalar(0));
    first_mask.copyTo(first_in_union(cv::Rect(
        first_bbox.x - ux0, first_bbox.y - uy0,
        first_bbox.width, first_bbox.height)));
    second_mask.copyTo(second_in_union(cv::Rect(
        second_bbox.x - ux0, second_bbox.y - uy0,
        second_bbox.width, second_bbox.height)));

    cv::Mat overlap_mask;
    cv::bitwise_and(first_in_union, second_in_union, overlap_mask);
    relation.overlap_px = cv::countNonZero(overlap_mask);
    relation.valid = true;

    if (relation.overlap_px <= 0) return relation;

    const int min_px = std::min(relation.first_px, relation.second_px);
    const int union_px = relation.first_px + relation.second_px - relation.overlap_px;
    relation.overlap_over_min = static_cast<float>(relation.overlap_px) /
                                static_cast<float>(std::max(1, min_px));
    relation.overlap_over_first = static_cast<float>(relation.overlap_px) /
                                  static_cast<float>(std::max(1, relation.first_px));
    relation.overlap_over_second = static_cast<float>(relation.overlap_px) /
                                   static_cast<float>(std::max(1, relation.second_px));
    relation.iou = static_cast<float>(relation.overlap_px) /
                   static_cast<float>(std::max(1, union_px));
    return relation;
}

// ---------------------------------------------------------------------------
// RefreshNodeFromBlob — update a node's visual data from a matched blob
// ---------------------------------------------------------------------------
static void RefreshNodeFromBlob(WhiteboardGroup& group,
                                DrawingNode& node,
                                const FrameBlob& blob,
                                const cv::Point2f& canvas_centroid,
                                const cv::Rect& canvas_bbox) {
    group.spatial_index.Remove(node.id, node.centroid_canvas);
    node.centroid_canvas = canvas_centroid;
    node.bbox_canvas = canvas_bbox;
    node.binary_mask = blob.binary_mask.clone();
    if (!blob.color_pixels.empty()) {
        node.color_pixels = blob.color_pixels.clone();
    }
    node.contour = blob.contour;
    std::copy(blob.hu, blob.hu + 7, node.hu);
    node.area = blob.area;
    node.max_area_seen = std::max(std::max(node.max_area_seen, node.area), blob.area);
    group.spatial_index.Insert(node.id, canvas_centroid);
}

static void BuildFrameBlobNeighborGraph(std::vector<FrameBlob>& blobs, int k) {
    for (auto& blob : blobs) {
        blob.neighbor_blob_indices.clear();
    }

    for (int i = 0; i < static_cast<int>(blobs.size()); i++) {
        std::vector<std::pair<float, int>> candidates;
        candidates.reserve(blobs.size());
        for (int j = 0; j < static_cast<int>(blobs.size()); j++) {
            if (i == j) continue;
            const cv::Point2f delta = blobs[i].centroid - blobs[j].centroid;
            const float dist2 = delta.x * delta.x + delta.y * delta.y;
            candidates.push_back({dist2, j});
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const std::pair<float, int>& a,
                     const std::pair<float, int>& b) {
                      if (std::abs(a.first - b.first) > 1e-4f) return a.first < b.first;
                      return a.second < b.second;
                  });

        const int use_count = std::min(k, static_cast<int>(candidates.size()));
        for (int idx = 0; idx < use_count; idx++) {
            const int neighbor = candidates[idx].second;
            AppendUniqueId(blobs[i].neighbor_blob_indices, neighbor);
            AppendUniqueId(blobs[neighbor].neighbor_blob_indices, i);
        }
    }

    for (auto& blob : blobs) {
        DeduplicateIds(blob.neighbor_blob_indices);
    }
}

static void FilterFrameBlobsForCanvas(std::vector<FrameBlob>& blobs,
                                      const cv::Mat& reject_mask,
                                      int neighbor_count) {
    if (blobs.empty()) {
        return;
    }

    blobs.erase(
        std::remove_if(
            blobs.begin(),
            blobs.end(),
            [&](const FrameBlob& blob) {
                return BlobTouchesRejectMask(blob, reject_mask);
            }),
        blobs.end());

    BuildFrameBlobNeighborGraph(blobs, neighbor_count);
}

static void RebuildGroupNeighborGraph(WhiteboardGroup& group, int k) {
    for (auto& pair : group.nodes) {
        pair.second->neighbor_ids.clear();
    }

    for (auto& pair : group.nodes) {
        auto& node = *pair.second;
        const auto nearest = group.spatial_index.QueryKNearest(node.centroid_canvas, k + 1);
        for (int neighbor_id : nearest) {
            if (neighbor_id == node.id) continue;
            AppendUniqueId(node.neighbor_ids, neighbor_id);
            auto nit = group.nodes.find(neighbor_id);
            if (nit != group.nodes.end()) {
                AppendUniqueId(nit->second->neighbor_ids, node.id);
            }
        }
    }

    for (auto& pair : group.nodes) {
        DeduplicateIds(pair.second->neighbor_ids);
    }
}

static void DrawGraphOverlay(cv::Mat& canvas,
                             const WhiteboardGroup& group,
                             int min_px_x,
                             int min_px_y,
                             double last_fps,
                             int last_votes) {
    if (canvas.empty()) {
        return;
    }

    const int width = canvas.cols;
    const int height = canvas.rows;
    const cv::Rect canvas_bounds(0, 0, width, height);

    auto to_render_rect = [&](const cv::Rect& rect) {
        return cv::Rect(
            rect.x - min_px_x,
            rect.y - min_px_y,
            rect.width,
            rect.height);
    };

    auto to_render_point = [&](const cv::Point2f& point) {
        return cv::Point(
            static_cast<int>(std::round(point.x)) - min_px_x,
            static_cast<int>(std::round(point.y)) - min_px_y);
    };

    // --- FPS + stats banner (top-right, with background) ---
    {
        std::ostringstream fps_ss;
        fps_ss << std::fixed << std::setprecision(1)
               << "FPS: " << last_fps
               << "  Nodes: " << group.nodes.size()
               << "  Cands: " << group.candidates.size()
               << "  Votes: " << last_votes;
        const std::string fps_text = fps_ss.str();
        int baseline = 0;
        const double font_scale = 1.2;
        const int thickness = 2;
        cv::Size text_size = cv::getTextSize(fps_text, cv::FONT_HERSHEY_SIMPLEX,
                                             font_scale, thickness, &baseline);
        const int pad = 10;
        cv::Rect banner_rc(width - text_size.width - pad * 3, 0,
                           text_size.width + pad * 3,
                           text_size.height + baseline + pad * 2);
        banner_rc &= canvas_bounds;
        if (!banner_rc.empty()) {
            cv::Mat roi = canvas(banner_rc);
            roi = roi * 0.3;
        }
        cv::Point fps_pos(width - text_size.width - pad * 2,
                          text_size.height + pad);
        cv::putText(canvas, fps_text, fps_pos + cv::Point(1, 1),
                    cv::FONT_HERSHEY_SIMPLEX, font_scale,
                    cv::Scalar(0, 0, 0), thickness + 1);
        cv::putText(canvas, fps_text, fps_pos,
                    cv::FONT_HERSHEY_SIMPLEX, font_scale,
                    cv::Scalar(0, 255, 255), thickness);
    }

    // --- Candidate nodes ---
    for (const auto& cp : group.candidates) {
        const auto& cand = *cp.second;
        cv::Rect rc = to_render_rect(cand.bbox_canvas);
        rc &= canvas_bounds;
        if (rc.empty()) continue;
        cv::rectangle(canvas, rc, cv::Scalar(0, 255, 255), 1);
        const cv::Point c_rc = to_render_point(cand.centroid_canvas);
        cv::circle(canvas, c_rc, 3, cv::Scalar(0, 255, 255), -1);
        std::string label = "C" + std::to_string(cand.id)
                          + " s" + std::to_string(cand.seen_count);
        cv::Point label_pos(rc.x, rc.y + rc.height + 28);
        DebugText(canvas, label, label_pos, 1.0, cv::Scalar(0, 255, 255));
    }

    // --- Graph nodes ---
    for (const auto& pair : group.nodes) {
        const auto& node = *pair.second;
        cv::Rect rc = to_render_rect(node.bbox_canvas);
        rc &= canvas_bounds;
        if (rc.empty()) continue;

        const cv::Scalar box_color(255, 255, 0);
        cv::rectangle(canvas, rc, box_color, 1);

        const cv::Point c_rc = to_render_point(node.centroid_canvas);
        for (int nid : node.neighbor_ids) {
            if (nid < node.id) continue;
            auto nit = group.nodes.find(nid);
            if (nit == group.nodes.end()) continue;
            const cv::Point n_rc = to_render_point(nit->second->centroid_canvas);
            cv::line(canvas, c_rc, n_rc, cv::Scalar(0, 200, 0), 1);
        }
        cv::circle(canvas, c_rc, 3, cv::Scalar(0, 0, 255), -1);

        std::string label = std::to_string(node.id);
        label += " s" + std::to_string(node.seen_count);
        cv::Point label_pos(rc.x, rc.y + rc.height + 28);
        DebugText(canvas, label, label_pos, 0.9, cv::Scalar(200, 200, 200));
    }

}

} // namespace

// ============================================================================
//  SECTION 3: Constructor / Destructor
// ============================================================================

WhiteboardCanvas::WhiteboardCanvas() {
    stop_worker_ = false;
    perf_stats_.window_start_time = SteadyClock::now();

    if (!IsWhiteboardCanvasHelperProcess()) {
        auto helper_client = std::make_unique<WhiteboardCanvasHelperClient>();
        if (helper_client && helper_client->Start()) {
            helper_client_ = std::move(helper_client);
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

const char* WhiteboardCanvas::ProfileStepName(ProfileStep step) {
    switch (step) {
        case ProfileStep::kQueueSubmit: return "queue_submit";
        case ProfileStep::kFrameTotal: return "frame_total";
        case ProfileStep::kNoUpdateMask: return "no_update_mask";
        case ProfileStep::kMotionGate: return "motion_gate";
        case ProfileStep::kBinaryMask: return "binary_mask";
        case ProfileStep::kBlobExtract: return "blob_extract";
        case ProfileStep::kGraphMatch: return "graph_match";
        case ProfileStep::kCameraUpdate: return "camera_update";
        case ProfileStep::kGraphUpdate: return "graph_update";
        case ProfileStep::kMatchedRefresh: return "matched_refresh";
        case ProfileStep::kCandidateProcess: return "candidate_process";
        case ProfileStep::kMergeNodes: return "merge_nodes";
        case ProfileStep::kAbsenceTracking: return "absence_tracking";
        case ProfileStep::kNeighborRebuild: return "neighbor_rebuild";
        case ProfileStep::kBoundsUpdate: return "bounds_update";
        case ProfileStep::kSeedGroup: return "seed_group";
        case ProfileStep::kCreateSubCanvas: return "create_subcanvas";
        case ProfileStep::kEnsureRenderCache: return "ensure_render_cache";
        case ProfileStep::kRenderStrokeCache: return "render_stroke_cache";
        case ProfileStep::kRenderRawCache: return "render_raw_cache";
        case ProfileStep::kViewport: return "viewport";
        case ProfileStep::kOverview: return "overview";
        case ProfileStep::kRgbaCopy: return "rgba_copy";
        case ProfileStep::kCount: break;
    }

    return "unknown";
}

void WhiteboardCanvas::RecordProfileSample(ProfileStep step, double duration_ms) {
    std::lock_guard<std::mutex> lock(perf_stats_mutex_);
    perf_stats_.steps[static_cast<size_t>(step)].Add(duration_ms);
}

void WhiteboardCanvas::RecordFrameProfile(bool motion_gated,
                                          int best_votes,
                                          int blob_count,
                                          int graph_node_count) {
    std::lock_guard<std::mutex> lock(perf_stats_mutex_);
    perf_stats_.frame_count++;
    if (motion_gated) {
        perf_stats_.gated_frames++;
    }
    if (best_votes > 0) {
        perf_stats_.matched_frames++;
    }
    perf_stats_.total_best_votes += static_cast<double>(best_votes);
    perf_stats_.total_frame_contours += static_cast<double>(blob_count);
    perf_stats_.total_canvas_contours += static_cast<double>(graph_node_count);
    perf_stats_.last_votes = best_votes;

    const auto now = SteadyClock::now();
    const double elapsed_sec = std::chrono::duration<double>(
        now - perf_stats_.window_start_time).count();
    if (elapsed_sec > 0.0) {
        perf_stats_.last_fps = static_cast<double>(perf_stats_.frame_count) / elapsed_sec;
    }
}

void WhiteboardCanvas::MaybeLogProfileSummary(bool force) {
    struct StepRow {
        std::string name;
        double average_ms = 0.0;
        double variance_ms2 = 0.0;
        uint64_t count = 0;
    };

    std::vector<std::pair<std::string, std::string>> summary_rows;
    std::vector<StepRow> step_rows;

    {
        std::lock_guard<std::mutex> lock(perf_stats_mutex_);
        const auto now = SteadyClock::now();
        const auto elapsed = now - perf_stats_.window_start_time;
        if (!force && elapsed < std::chrono::seconds(10)) {
            return;
        }

        bool has_samples = perf_stats_.frame_count > 0;
        if (!has_samples) {
            for (const auto& step : perf_stats_.steps) {
                if (step.count > 0) {
                    has_samples = true;
                    break;
                }
            }
        }

        if (!has_samples) {
            perf_stats_.window_start_time = now;
            return;
        }

        const double elapsed_sec = std::max(0.001,
            std::chrono::duration<double>(elapsed).count());
        const double frames = static_cast<double>(std::max(1, perf_stats_.frame_count));
        const double matched_frames = static_cast<double>(std::max(1, perf_stats_.matched_frames));
        perf_stats_.last_fps = static_cast<double>(perf_stats_.frame_count) / elapsed_sec;

        auto format_value = [](double value) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(3) << value;
            return stream.str();
        };

        auto format_count = [](uint64_t value) {
            std::ostringstream stream;
            stream << value;
            return stream.str();
        };

        summary_rows.push_back({"windowSec", format_value(elapsed_sec)});
        summary_rows.push_back({"frames", format_count(static_cast<uint64_t>(perf_stats_.frame_count))});
        summary_rows.push_back({"gated", format_count(static_cast<uint64_t>(perf_stats_.gated_frames))});
        summary_rows.push_back({"matched", format_count(static_cast<uint64_t>(perf_stats_.matched_frames))});
        summary_rows.push_back({"fps", format_value(perf_stats_.last_fps)});
        summary_rows.push_back({"avgBlobs", format_value(perf_stats_.total_frame_contours / frames)});
        summary_rows.push_back({"avgNodes", format_value(perf_stats_.total_canvas_contours / frames)});
        summary_rows.push_back({"avgVotesAll", format_value(perf_stats_.total_best_votes / frames)});
        summary_rows.push_back({
            "avgVotesMatched",
            format_value(perf_stats_.matched_frames > 0
                ? (perf_stats_.total_best_votes / matched_frames)
                : 0.0)
        });
        summary_rows.push_back({
            "avgCandidatePairs",
            format_value(static_cast<double>(perf_stats_.total_candidate_pairs) / frames)
        });
        summary_rows.push_back({
            "avgAcceptedPairs",
            format_value(static_cast<double>(perf_stats_.total_accepted_pairs) / frames)
        });

        step_rows.reserve(perf_stats_.steps.size());
        for (size_t index = 0; index < perf_stats_.steps.size(); index++) {
            const auto& stat = perf_stats_.steps[index];
            if (stat.count == 0) {
                continue;
            }

            const double average_ms = stat.total_ms / static_cast<double>(stat.count);
            const double variance_ms2 = std::max(
                0.0,
                stat.total_sq_ms / static_cast<double>(stat.count) - average_ms * average_ms);

            StepRow row;
            row.name = ProfileStepName(static_cast<ProfileStep>(index));
            row.average_ms = average_ms;
            row.variance_ms2 = variance_ms2;
            row.count = stat.count;
            step_rows.push_back(std::move(row));
        }

        const double last_fps = perf_stats_.last_fps;
        const int last_votes = perf_stats_.last_votes;
        perf_stats_ = PerformanceStats();
        perf_stats_.window_start_time = now;
        perf_stats_.last_fps = last_fps;
        perf_stats_.last_votes = last_votes;
    }

    std::ostringstream block;
    block << "=========\n\n\n";
    block << "[WhiteboardProfile]\n";
    block << "Summary\n";
    block << std::left << std::setw(20) << "Metric"
          << std::right << std::setw(16) << "Value" << '\n';
    block << std::string(36, '-') << '\n';
    for (const auto& row : summary_rows) {
        block << std::left << std::setw(20) << row.first
              << std::right << std::setw(16) << row.second << '\n';
    }

    block << '\n';
    block << "Steps\n";
    block << std::left << std::setw(24) << "Step"
          << std::right << std::setw(14) << "AvgMs"
          << std::setw(14) << "VarMs2"
          << std::setw(10) << "N" << '\n';
    block << std::string(62, '-') << '\n';
    for (const auto& row : step_rows) {
        block << std::left << std::setw(24) << row.name
              << std::right << std::fixed << std::setprecision(3)
              << std::setw(14) << row.average_ms
              << std::setw(14) << row.variance_ms2
              << std::setw(10) << row.count << '\n';
    }

    block << "\n\n\n=========";
    WhiteboardLog(block.str());
}

void WhiteboardCanvas::RecordRgbaCopyProfile(double duration_ms) {
    RecordProfileSample(ProfileStep::kRgbaCopy, duration_ms);
    MaybeLogProfileSummary();
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
    for (auto& group : groups_) {
        group->stroke_cache_dirty = true;
        group->raw_cache_dirty = true;
    }
    BumpCanvasVersion();
}

bool WhiteboardCanvas::EnsureRenderCacheReady(WhiteboardGroup& group,
                                              CanvasRenderMode render_mode) {
    const auto profile_start = SteadyClock::now();

    const auto cache_pm = GetPipelineMode();
    if (render_mode == CanvasRenderMode::kRaw) {
        if (group.raw_cache_dirty) {
            if (cache_pm == CanvasPipelineMode::kChunk) RebuildRawRenderCacheChunk(group);
            else RebuildRawRenderCache(group);
            group.raw_cache_dirty = false;
        }
    } else {
        if (group.stroke_cache_dirty) {
            if (cache_pm == CanvasPipelineMode::kChunk) RebuildStrokeRenderCacheChunk(group);
            else RebuildStrokeRenderCache(group);
            group.stroke_cache_dirty = false;
        }
    }

    const bool ready = !GetRenderCacheForMode(group, render_mode).empty();
    RecordProfileSample(ProfileStep::kEnsureRenderCache,
                        ElapsedMs(profile_start, SteadyClock::now()));
    return ready;
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
        person_mask.type() != CV_8UC1) {
        WhiteboardLog(
            "[WhiteboardCanvas] Dropping frame: person mask is required and must match frame size",
            true);
        return;
    }

    const auto profile_start = SteadyClock::now();

    CanvasWorkItem item;
    frame.copyTo(item.frame);
    person_mask.copyTo(item.person_mask);

    std::unique_lock<std::mutex> lock(queue_mutex_);
    while ((int)work_queue_.size() >= kQueueDepth) {
        work_queue_.pop();
    }
    work_queue_.push(std::move(item));
    lock.unlock();
    queue_cv_.notify_one();

    RecordProfileSample(ProfileStep::kQueueSubmit,
                        ElapsedMs(profile_start, SteadyClock::now()));
    MaybeLogProfileSummary();
}

bool WhiteboardCanvas::GetViewport(float panX, float panY, float zoom,
                                    cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetViewport(panX, panY, zoom, viewSize, out_frame);
    }

    if (viewSize.width <= 0 || viewSize.height <= 0) {
        return false;
    }

    const auto profile_start = SteadyClock::now();
    bool success = false;

    {
        std::unique_lock<std::mutex> lock(state_mutex_);

        int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
        if (idx < 0 || idx >= (int)groups_.size()) {
            RecordProfileSample(ProfileStep::kViewport,
                                ElapsedMs(profile_start, SteadyClock::now()));
            MaybeLogProfileSummary();
            return false;
        }
        auto& group = *groups_[idx];

        const CanvasRenderMode render_mode = GetRenderMode();
        if (!EnsureRenderCacheReady(group, render_mode)) {
            RecordProfileSample(ProfileStep::kViewport,
                                ElapsedMs(profile_start, SteadyClock::now()));
            MaybeLogProfileSummary();
            return false;
        }

        const cv::Mat& render_cache = GetRenderCacheForMode(group, render_mode);

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
        success = true;
    }

    RecordProfileSample(ProfileStep::kViewport,
                        ElapsedMs(profile_start, SteadyClock::now()));
    MaybeLogProfileSummary();
    return success;
}

bool WhiteboardCanvas::GetOverview(cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetOverview(viewSize, out_frame);
    }

    const auto profile_start = SteadyClock::now();
    bool success = false;

    {
        std::unique_lock<std::mutex> lock(state_mutex_);

        int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
        if (idx < 0 || idx >= (int)groups_.size()) {
            RecordProfileSample(ProfileStep::kOverview,
                                ElapsedMs(profile_start, SteadyClock::now()));
            MaybeLogProfileSummary();
            return false;
        }
        auto& group = *groups_[idx];

        const CanvasRenderMode render_mode = GetRenderMode();
        if (!EnsureRenderCacheReady(group, render_mode)) {
            RecordProfileSample(ProfileStep::kOverview,
                                ElapsedMs(profile_start, SteadyClock::now()));
            MaybeLogProfileSummary();
            return false;
        }

        success = RenderOverviewToFrame(
            GetRenderCacheForMode(group, render_mode), viewSize, out_frame);
    }

    RecordProfileSample(ProfileStep::kOverview,
                        ElapsedMs(profile_start, SteadyClock::now()));
    MaybeLogProfileSummary();
    return success;
}

bool WhiteboardCanvas::GetOverviewBlocking(cv::Size viewSize, cv::Mat& out_frame) {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetOverview(viewSize, out_frame);
    }

    if (viewSize.width <= 0 || viewSize.height <= 0) return false;

    const auto profile_start = SteadyClock::now();
    bool success = false;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);

        int idx = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
        if (idx < 0 || idx >= (int)groups_.size()) {
            RecordProfileSample(ProfileStep::kOverview,
                                ElapsedMs(profile_start, SteadyClock::now()));
            MaybeLogProfileSummary();
            return false;
        }
        auto& group = *groups_[idx];

        const CanvasRenderMode render_mode = GetRenderMode();
        if (!EnsureRenderCacheReady(group, render_mode)) {
            RecordProfileSample(ProfileStep::kOverview,
                                ElapsedMs(profile_start, SteadyClock::now()));
            MaybeLogProfileSummary();
            return false;
        }

        success = RenderOverviewToFrame(
            GetRenderCacheForMode(group, render_mode), viewSize, out_frame);
    }

    RecordProfileSample(ProfileStep::kOverview,
                        ElapsedMs(profile_start, SteadyClock::now()));
    MaybeLogProfileSummary();
    return success;
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
    camera_velocity_ = cv::Point2f(0, 0);
    last_vote_count_ = 0;
    global_frame_bootstrap_consumed_ = false;

    prev_gray_ = cv::Mat();
    prev_frame_gray_ = cv::Mat();
    canvas_contours_.clear();
    canvas_contours_dirty_ = true;
    consecutive_failed_matches_ = 0;
    last_match_accuracy_ = 0.0f;
    chunk_height_ = 0;
    has_content_ = false;
    BumpCanvasVersion();
    next_debug_id_ = 0;
    frame_w_ = 0;
    frame_h_ = 0;
    matched_frame_counter_ = 0;
    processed_frame_id_ = 0;

    perf_stats_ = PerformanceStats();
    perf_stats_.window_start_time = SteadyClock::now();
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

bool WhiteboardCanvas::ApplyMotionGate(const cv::Mat& gray,
                                       float& motion_fraction,
                                       bool& motion_too_high) {
    motion_fraction = 0.0f;
    motion_too_high = false;

    if (!kEnableMotionGate) {
        return false;
    }

    cv::Mat motion_gray;
    const float motion_scale = ComputeScaleForLongEdge(gray.size(), kMotionLongEdge);
    if (motion_scale < 0.999f) {
        cv::resize(gray, motion_gray, cv::Size(), motion_scale, motion_scale,
                   cv::INTER_AREA);
    } else {
        motion_gray = gray;
    }

    if (!prev_gray_.empty() && prev_gray_.size() == motion_gray.size()) {
        cv::Mat diff;
        cv::absdiff(motion_gray, prev_gray_, diff);
        cv::threshold(diff, diff, 10, 255, cv::THRESH_BINARY);
        motion_fraction = static_cast<float>(cv::countNonZero(diff)) /
                          static_cast<float>(std::max<size_t>(1, diff.total()));
        motion_too_high = motion_fraction > kMaxMotionFraction;
    }

    motion_gray.copyTo(prev_gray_);

    return motion_too_high;
}

void WhiteboardCanvas::ProcessFrameInternal(const cv::Mat& uncut_frame, const cv::Mat& person_mask) {
    const int current_frame = processed_frame_id_++;
    const auto frame_start = SteadyClock::now();

    // cut the edges of the frame out. keep only the center. 90% top and bottom 5% each side
    // its to avoid edge noise. and elements in screens like sidebars, timestamps, etc
    const cv::Rect roi = ComputeProcessingRoi(uncut_frame.size());
    if (roi.width <= 0 || roi.height <= 0) {
        return;
    }

    cv::Mat frame = uncut_frame(roi);
    const cv::Mat cropped_person_mask = CropPersonMaskForProcessing(person_mask, roi);

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    const auto no_update_start = SteadyClock::now();
    cv::Mat no_update_mask = BuildNoUpdateMask(gray, cropped_person_mask);
    RecordProfileSample(ProfileStep::kNoUpdateMask,
                        ElapsedMs(no_update_start, SteadyClock::now()));
    const cv::Rect lecturer_frame_rect = ComputeMaskBoundingRect(cropped_person_mask);
    cv::Mat stroke_reject_mask;
    if (kEnableFrameStrokeRejectFilter) {
        stroke_reject_mask = BuildFrameStrokeRejectMask(
            frame.size(), lecturer_frame_rect);
    }

    // -------------------------------------------------------------------
    // STAGE 1: Motion gate
    // -------------------------------------------------------------------
    float motion_fraction = 0.0f;
    bool motion_too_high = false;
    const auto motion_start = SteadyClock::now();
    const bool motion_gated = ApplyMotionGate(
        gray, motion_fraction, motion_too_high);
    RecordProfileSample(ProfileStep::kMotionGate,
                        ElapsedMs(motion_start, SteadyClock::now()));

    if (motion_gated) {
        RecordFrameProfile(true, 0, 0, 0);
        RecordProfileSample(ProfileStep::kFrameTotal,
                            ElapsedMs(frame_start, SteadyClock::now()));
        MaybeLogProfileSummary();
        return;
    }

    // -------------------------------------------------------------------
    // STAGE 3: Enhance + Binarize (graph pipeline)
    // -------------------------------------------------------------------
    int stroke_pixel_count = 0;
    const auto binary_start = SteadyClock::now();
    cv::Mat binary = BuildBinaryMask(gray, no_update_mask, stroke_pixel_count);
    RecordProfileSample(ProfileStep::kBinaryMask,
                        ElapsedMs(binary_start, SteadyClock::now()));

    // -------------------------------------------------------------------
    // STAGE 4: Graph-based blob extraction and matching
    // -------------------------------------------------------------------
    const auto blob_extract_start = SteadyClock::now();
    std::vector<FrameBlob> blobs = ExtractFrameBlobs(binary, frame);
    RecordProfileSample(ProfileStep::kBlobExtract,
                        ElapsedMs(blob_extract_start, SteadyClock::now()));

    // Enhance blob color pixels for cleaner raw render cache
    EnhanceFrameBlobs(blobs, frame, g_canvas_enhance_threshold.load());

    std::lock_guard<std::mutex> state_lock(state_mutex_);

    // Helper to recompute has_content_ after graph updates or new group creation
    auto recompute_has_content = [&]() {
        bool any_nodes = false;
        for (const auto& group_ptr : groups_) {
            if (group_ptr && !group_ptr->nodes.empty()) {
                any_nodes = true;
                break;
            }
        }
        has_content_ = any_nodes;
    };

    if (frame_w_ == 0) { frame_w_ = frame.cols; frame_h_ = frame.rows; chunk_height_ = frame_h_; }

    int         best_votes = 0;
    cv::Point2f matched_pos(0, 0);
    std::string match_status;
    bool created_new_sc = false;
    bool graph_updated = false;

    const bool has_active_group =
        active_group_idx_ >= 0 && active_group_idx_ < static_cast<int>(groups_.size());
    const int active_group_nodes = has_active_group
        ? static_cast<int>(groups_[active_group_idx_]->nodes.size())
        : 0;
    const bool graph_ready_for_matching =
        has_active_group && active_group_nodes >= kStableGraphNodeThreshold;

    if (graph_ready_for_matching) {
        const auto match_start = SteadyClock::now();
        if (!blobs.empty()) {
            auto& group = *groups_[active_group_idx_];
            matched_pos = MatchBlobsToGraph(group, blobs, best_votes);
        }
        RecordProfileSample(ProfileStep::kGraphMatch,
                            ElapsedMs(match_start, SteadyClock::now()));
    }

    if (kEnableFrameStrokeRejectFilter) {
        FilterFrameBlobsForCanvas(blobs, stroke_reject_mask, kKNeighbors);
    }

    // -------------------------------------------------------------------
    // STAGE 5: Graph update or create new group
    // -------------------------------------------------------------------
    const auto graph_update_start = SteadyClock::now();
    if (!has_active_group) {
        if (stroke_pixel_count >= kMinStrokePixelsForNewSC && !blobs.empty()) {
            CreateSubCanvas(frame, binary, blobs, current_frame);
            created_new_sc = true;
            if (active_group_idx_ >= 0 && active_group_idx_ < static_cast<int>(groups_.size())) {
                groups_[active_group_idx_]->last_lecturer_rect = cv::Rect();
            }
            recompute_has_content();
            match_status = "NEW GROUP (total=" + std::to_string(groups_.size()) + ")";
        } else {
            match_status = "bootstrap waiting (strokes=" + std::to_string(stroke_pixel_count)
                         + " blobs=" + std::to_string(blobs.size()) + ")";
        }

    } else if (!graph_ready_for_matching) {
        auto& group = *groups_[active_group_idx_];
        if (!blobs.empty()) {
            SeedGroupFromFrameBlobs(group, blobs, current_frame);
            global_camera_pos_ = cv::Point2f(0, 0);
            camera_velocity_ = cv::Point2f(0, 0);
            last_vote_count_ = 0;
            global_frame_bootstrap_consumed_ = false;
            matched_frame_counter_ = 0;
            group.last_lecturer_rect = cv::Rect();
            recompute_has_content();
            match_status = "RESET GRAPH GR" + std::to_string(active_group_idx_)
                         + " nodes=" + std::to_string(group.nodes.size())
                         + " target=" + std::to_string(kStableGraphNodeThreshold);
        } else {
            group.last_lecturer_rect = cv::Rect();
            match_status = "bootstrap waiting (graph=" + std::to_string(active_group_nodes)
                         + " target=" + std::to_string(kStableGraphNodeThreshold) + ")";
        }

    } else {
        auto& group = *groups_[active_group_idx_];
        const auto camera_update_start = SteadyClock::now();

        if (best_votes >= kMinVotesForCameraUpdate) {
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
                    if (std::abs(subpx.x) < kVoteBinSize && std::abs(subpx.y) < kVoteBinSize) {
                        matched_pos.x += (float)subpx.x;
                        matched_pos.y += (float)subpx.y;
                    }
                }
            }

            // Velocity-based adaptive jump rejection:
            // Allow jumps proportional to recent camera velocity instead
            // of relying on a fixed pixel threshold.
            if (kEnableJumpRejection && matched_frame_counter_ > 0) {
                const cv::Point2f predicted_pos = global_camera_pos_ + camera_velocity_;
                const float speed = static_cast<float>(cv::norm(camera_velocity_));
                // Allow jump = max(kMaxJumpPx, 3× recent speed)
                const float adaptive_limit = std::min(kMaxPredictedJumpPx,
                    std::max(kMaxJumpPx, speed * 3.0f));
                const float jump = static_cast<float>(cv::norm(matched_pos - predicted_pos));
                if (jump > adaptive_limit) {
                    matched_pos = predicted_pos;
                }
            }

            // Update velocity with exponential moving average
            const cv::Point2f frame_velocity = matched_pos - global_camera_pos_;
            camera_velocity_ = kVelocitySmoothingAlpha * frame_velocity
                             + (1.0f - kVelocitySmoothingAlpha) * camera_velocity_;

            global_camera_pos_ = matched_pos;
            global_frame_bootstrap_consumed_ = true;
            matched_frame_counter_++;
            last_vote_count_ = best_votes;
        } else if (best_votes > 0) {
            // Low-confidence match: apply velocity prediction but do not
            // trust the noisy matched position.
            global_camera_pos_ += camera_velocity_;
            // Decay velocity when matches are weak
            camera_velocity_ *= 0.7f;
        }
        RecordProfileSample(ProfileStep::kCameraUpdate,
                            ElapsedMs(camera_update_start, SteadyClock::now()));

        // Compute lecturer rect in canvas coords BEFORE UpdateGraph
        // so absence tracking uses the current frame's lecturer position.
        const cv::Rect current_lecturer_canvas =
            TranslateFrameRectToCanvas(lecturer_frame_rect, global_camera_pos_);

        graph_updated = UpdateGraph(group, blobs, current_frame, current_lecturer_canvas);
        group.last_lecturer_rect = current_lecturer_canvas;
        recompute_has_content();

        // When graph overlay is on, always dirty caches so debug overlays
        // track the latest camera placement every frame.
        if (kShowGraphOverlay) {
            group.stroke_cache_dirty = true;
            group.raw_cache_dirty = true;
            BumpCanvasVersion();
        }

        if (best_votes >= kMinVotesForCameraUpdate) {
            match_status = (graph_updated ? "MATCHED GR" : "MATCHED SKIP GR")
                         + std::to_string(active_group_idx_)
                         + " votes=" + std::to_string(best_votes)
                         + " pos=(" + std::to_string((int)global_camera_pos_.x)
                         + ","     + std::to_string((int)global_camera_pos_.y) + ")"
                         + " nodes=" + std::to_string(group.nodes.size());
        } else if (best_votes > 0) {
            match_status = "LOW CONFIDENCE GR"
                         + std::to_string(active_group_idx_)
                         + " votes=" + std::to_string(best_votes)
                         + " (need " + std::to_string(kMinVotesForCameraUpdate) + ")"
                         + " nodes=" + std::to_string(group.nodes.size());
        } else {
            match_status = (graph_updated ? "NO MATCH PRUNE GR" : "NO MATCH GR")
                         + std::to_string(active_group_idx_)
                         + " blobs=" + std::to_string(blobs.size())
                         + " nodes=" + std::to_string(group.nodes.size());
        }
    }

    RecordProfileSample(ProfileStep::kGraphUpdate,
                        ElapsedMs(graph_update_start, SteadyClock::now()));

    if (groups_.empty() && stroke_pixel_count >= kMinStrokePixelsForNewSC &&
        !created_new_sc && !blobs.empty()) {
        CreateSubCanvas(frame, binary, blobs, current_frame);
        created_new_sc = true;
        if (active_group_idx_ >= 0 && active_group_idx_ < static_cast<int>(groups_.size())) {
            groups_[active_group_idx_]->last_lecturer_rect = cv::Rect();
        }
        recompute_has_content();
        match_status = "NEW GROUP (total=" + std::to_string(groups_.size()) + ")";
    }

    // -------------------------------------------------------------------
    // STAGE 5b: Also paint to chunks (for chunk/hybrid display modes)
    // -------------------------------------------------------------------
    if (active_group_idx_ >= 0 && active_group_idx_ < static_cast<int>(groups_.size()) &&
        best_votes >= kMinVotesForCameraUpdate && chunk_height_ > 0) {
        auto& chunk_group = *groups_[active_group_idx_];
        cv::Mat stroke_paint(frame.size(), CV_8UC3, cv::Scalar(0, 0, 0));
        PaintStrokesToChunks(chunk_group, binary, stroke_paint, global_camera_pos_, no_update_mask);
        PaintRawFrameToChunks(chunk_group, frame, global_camera_pos_, no_update_mask);
    }

    int graph_node_count = 0;
    if (active_group_idx_ >= 0 && active_group_idx_ < static_cast<int>(groups_.size())) {
        graph_node_count = static_cast<int>(groups_[active_group_idx_]->nodes.size());
    }

    RecordFrameProfile(false, best_votes,
                       static_cast<int>(blobs.size()),
                       graph_node_count);
    RecordProfileSample(ProfileStep::kFrameTotal,
                        ElapsedMs(frame_start, SteadyClock::now()));
    MaybeLogProfileSummary();

}

// ============================================================================
//  SECTION 7: Frame Blob Extraction
// ============================================================================

// Extracts connected components from the binary mask, computes their
// contours and Hu moments, and returns them as FrameBlobs.  Also builds
// a neighbor graph among the blobs for use in seeding new groups and
// graph updates.

std::vector<FrameBlob>
WhiteboardCanvas::ExtractFrameBlobs(const cv::Mat& binary,
                                     const cv::Mat& frame_bgr) const {
    std::vector<FrameBlob> result;
    if (binary.empty()) return result;

    // 1. Find connected components on raw binary mask
    cv::Mat labels, stats, centroids;
    int num_labels = cv::connectedComponentsWithStats(binary, labels, stats, centroids);
    if (num_labels <= 1) return result;

    struct StrokeComponent {
        int label;
        cv::Rect bbox;
        cv::Point2f centroid;
        double area;
        std::vector<cv::Point> contour;
    };

    std::vector<StrokeComponent> components;
    components.reserve(num_labels - 1);

    for (int i = 1; i < num_labels; i++) {
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area < kMinContourArea) continue;

        int bx = stats.at<int>(i, cv::CC_STAT_LEFT);
        int by = stats.at<int>(i, cv::CC_STAT_TOP);
        int bw = stats.at<int>(i, cv::CC_STAT_WIDTH);
        int bh = stats.at<int>(i, cv::CC_STAT_HEIGHT);

        bx = std::max(0, bx);
        by = std::max(0, by);
        bw = std::min(bw, binary.cols - bx);
        bh = std::min(bh, binary.rows - by);
        if (bw <= 0 || bh <= 0) continue;

        cv::Rect bbox(bx, by, bw, bh);
        cv::Mat cc_mask = (labels(bbox) == i);
        
        cv::Point2f local_centroid = ComputeGravityCenter(cc_mask);
        cv::Point2f global_centroid(local_centroid.x + (float)bx, local_centroid.y + (float)by);

        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(cc_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) continue;

        int largest_idx = 0;
        double largest_area = 0;
        for (int ci = 0; ci < (int)contours.size(); ci++) {
            double ca = cv::contourArea(contours[ci]);
            if (ca > largest_area) { largest_area = ca; largest_idx = ci; }
        }

        StrokeComponent sc;
        sc.label = i;
        sc.bbox = bbox;
        sc.centroid = global_centroid;
        sc.area = largest_area;
        sc.contour = contours[largest_idx];
        components.push_back(std::move(sc));
    }

    if (components.empty()) return result;

    // 2. Group components by centroid distance using Union-Find
    std::vector<int> parent(components.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](auto& self, int i) -> int {
        return (parent[i] == i) ? i : (parent[i] = self(self, parent[i]));
    };
    auto unite = [&](int i, int j) {
        int r_i = find(find, i);
        int r_j = find(find, j);
        if (r_i != r_j) parent[r_i] = r_j;
    };

    for (size_t i = 0; i < components.size(); i++) {
        for (size_t j = i + 1; j < components.size(); j++) {
            float d = static_cast<float>(cv::norm(components[i].centroid - components[j].centroid));
            if (d < kStrokeClusterRadius) {
                unite((int)i, (int)j);
            }
        }
    }

    // 2b. Dilation-based clustering: unite components that touch after dilation
    if (kDilationClusterKernel > 0) {
        cv::Mat dilated_binary, dilated_labels;
        cv::Mat dilation_kernel = cv::getStructuringElement(
            cv::MORPH_RECT, cv::Size(kDilationClusterKernel, kDilationClusterKernel));
        cv::dilate(binary, dilated_binary, dilation_kernel);
        cv::connectedComponents(dilated_binary, dilated_labels);

        // Look up each component's dilated-CC label via its centroid
        std::unordered_map<int, int> label_to_first;
        for (size_t i = 0; i < components.size(); i++) {
            int cx = std::clamp((int)std::round(components[i].centroid.x),
                                0, dilated_labels.cols - 1);
            int cy = std::clamp((int)std::round(components[i].centroid.y),
                                0, dilated_labels.rows - 1);
            int dl = dilated_labels.at<int>(cy, cx);
            if (dl == 0) continue;  // background
            auto it = label_to_first.find(dl);
            if (it == label_to_first.end())
                label_to_first[dl] = (int)i;
            else
                unite((int)i, it->second);
        }
    }

    // 3. Merge clusters into final FrameBlobs
    std::unordered_map<int, std::vector<int>> clusters;
    for (int i = 0; i < (int)components.size(); i++) {
        clusters[find(find, i)].push_back(i);
    }

    for (auto& cp : clusters) {
        const auto& indices = cp.second;
        
        cv::Rect g_bbox = components[indices[0]].bbox;
        for (size_t k = 1; k < indices.size(); k++) {
            g_bbox |= components[indices[k]].bbox;
        }

        // Skip very elongated blobs — filters whiteboard edge lines
        {
            float long_edge = static_cast<float>(std::max(g_bbox.width, g_bbox.height));
            float short_edge = static_cast<float>(std::min(g_bbox.width, g_bbox.height));
            if (short_edge > 0 && long_edge / short_edge > kMaxAllowedRectangle) continue;
        }

        cv::Mat g_mask = cv::Mat::zeros(g_bbox.size(), CV_8UC1);
        double max_area = -1.0;
        int area_best_idx = -1;
        float best_squareness = -1.0f;
        int square_best_idx = -1;

        for (int idx : indices) {
            const auto& sc = components[idx];
            cv::Rect rel_roi(sc.bbox.x - g_bbox.x, sc.bbox.y - g_bbox.y, sc.bbox.width, sc.bbox.height);
            cv::Mat label_roi = labels(sc.bbox);
            cv::Mat cc_mask = (label_roi == sc.label);
            cc_mask.copyTo(g_mask(rel_roi), cc_mask);

            // Track largest area for fallback
            if (sc.area > max_area) {
                max_area = sc.area;
                area_best_idx = idx;
            }

            // Track squarest stroke with radius > threshold
            float w = static_cast<float>(sc.bbox.width);
            float h = static_cast<float>(sc.bbox.height);
            float radius = std::max(w, h) * 0.5f;
            if (radius > kSquareSelectionRadiusThreshold) {
                float squareness = std::min(w, h) / std::max(w, h);
                if (squareness > best_squareness) {
                    best_squareness = squareness;
                    square_best_idx = idx;
                }
            }
        }

        const int final_best_idx = (square_best_idx != -1) ? square_best_idx : area_best_idx;
        const auto& best_sc = components[final_best_idx];

        FrameBlob blob;
        blob.bbox = g_bbox;
        blob.binary_mask = g_mask;
        cv::Point2f local_g = ComputeGravityCenter(g_mask);
        blob.centroid = local_g + cv::Point2f((float)g_bbox.x, (float)g_bbox.y);
        blob.contour = best_sc.contour;
        blob.area = best_sc.area;

        cv::Moments m = cv::moments(blob.contour);
        cv::HuMoments(m, blob.hu);

        if (!frame_bgr.empty()) {
            blob.color_pixels = frame_bgr(g_bbox).clone();
        }
        result.push_back(std::move(blob));
    }

    BuildFrameBlobNeighborGraph(result, kKNeighbors);
    return result;
}

// ============================================================================
//  SECTION 8: Graph-Based Matching
// ============================================================================

cv::Point2f WhiteboardCanvas::MatchBlobsToGraph(WhiteboardGroup& group,
                                                std::vector<FrameBlob>& blobs,
                                                int& out_best_votes) {
    return MatchBlobsToGraphWithSeed(group, blobs, out_best_votes, global_camera_pos_);
}

cv::Point2f WhiteboardCanvas::MatchBlobsToGraphWithSeed(
        const WhiteboardGroup& group,
        std::vector<FrameBlob>& blobs,
        int& out_best_votes,
    const cv::Point2f& prior_offset) {
    out_best_votes = 0;
    if (blobs.empty() || group.nodes.empty()) return cv::Point2f(0, 0);

    // Reset blob match state
    for (auto& blob : blobs) {
        blob.matched_node_id = -1;
        blob.matched_shape_dist = 1e9;
        blob.matched_offset = cv::Point2f(0, 0);
        blob.matched_long_ratio = 0.0;
        blob.matched_short_ratio = 0.0;
        blob.matched_edge_error = 1e9;
    }

    // Build source features from blobs
    const int N = static_cast<int>(blobs.size());
    cv::Mat source_features(N, 7, CV_32F);
    std::vector<cv::Point2f> source_centroids(N);
    std::vector<cv::Rect> source_bboxes(N);
    std::vector<int> source_ids(N);

    for (int i = 0; i < N; i++) {
        const auto& blob = blobs[i];
        auto log_hu = ComputeLogHuFeatures(blob.hu);
        for (int j = 0; j < 7; j++) {
            source_features.at<float>(i, j) = log_hu[j];
        }
        source_centroids[i] = blob.centroid;
        source_bboxes[i] = blob.bbox;
        source_ids[i] = i;  // blob index
    }

    // Build target features from graph nodes
    std::vector<int> node_id_list;
    node_id_list.reserve(group.nodes.size());
    for (const auto& pair : group.nodes) {
        node_id_list.push_back(pair.first);
    }

    const int M = static_cast<int>(node_id_list.size());
    cv::Mat target_features(M, 7, CV_32F);
    std::vector<cv::Point2f> target_centroids(M);
    std::vector<cv::Rect> target_bboxes(M);
    std::vector<int> target_ids(M);

    for (int i = 0; i < M; i++) {
        const auto& node = *group.nodes.at(node_id_list[i]);
        auto log_hu = ComputeLogHuFeatures(node.hu);
        for (int j = 0; j < 7; j++) {
            target_features.at<float>(i, j) = log_hu[j];
        }
        target_centroids[i] = node.centroid_canvas;
        target_bboxes[i] = node.bbox_canvas;
        target_ids[i] = node.id;
    }

    // Run KD-Tree + RANSAC matching
    GraphMatchResult match = MatchWithKdTreeRansac(
        source_features, source_centroids, source_bboxes, source_ids,
        target_features, target_centroids, target_bboxes, target_ids,
        kKdTreeHuDistanceThreshold,
        kKdTreeMinBboxSimilarity,
        kRansacInlierTolerancePx,
        kRansacMaxIterations,
        kMinRansacInliers,
        kKdTreeKnnNeighbors,
        prior_offset);

    if (!match.valid) {
        return cv::Point2f(0, 0);
    }

    // Post-process: populate matched blob fields from inlier pairs
    for (const auto& [blob_index, node_id] : match.inlier_pairs) {
        auto node_it = group.nodes.find(node_id);
        if (node_it == group.nodes.end()) continue;
        const auto& node = *node_it->second;

        auto& blob = blobs[blob_index];
        blob.matched_node_id = node_id;
        blob.matched_offset = node.centroid_canvas - blob.centroid;
        blob.matched_long_ratio = LongEdgeSimilarity(blob.bbox, node.bbox_canvas);
        blob.matched_short_ratio = ShortEdgeSimilarity(blob.bbox, node.bbox_canvas);
        blob.matched_shape_dist = 1.0 - static_cast<double>(
            (blob.matched_long_ratio + blob.matched_short_ratio) * 0.5f);
        blob.matched_edge_error = 0.0;
    }

    out_best_votes = match.vote_count;
    return match.offset;
}

// ============================================================================
//  SECTION 9: Graph Update (Battle Logic)
// ============================================================================

bool WhiteboardCanvas::UpdateGraph(WhiteboardGroup& group,
                                    std::vector<FrameBlob>& blobs,
                                    int current_frame,
                                    const cv::Rect& lecturer_canvas_rect) {
    bool graph_changed = false;

    const cv::Rect cropped_frame = BuildCroppedFrameRect(frame_w_, frame_h_);

    static constexpr float kBattleReplaceAreaGain = 1.10f;
    static constexpr float kBattleShiftThresholdPx = 3.0f;

    // ---------------------------------------------------------------
    // 5a. Process matched blobs — refresh existing nodes
    // ---------------------------------------------------------------
    std::vector<int> matched_blob_indices;
    matched_blob_indices.reserve(blobs.size());
    for (int i = 0; i < static_cast<int>(blobs.size()); i++) {
        const auto& blob = blobs[i];
        if (blob.matched_node_id < 0) continue;
        if (blob.matched_long_ratio < kMinNodeLongEdgeSimilarity ||
            blob.matched_short_ratio < kMinNodeShortEdgeSimilarity) continue;
        if (!group.nodes.count(blob.matched_node_id)) continue;
        matched_blob_indices.push_back(i);
    }

    // Find new (unmatched) blobs reachable via the frame neighbour graph
    // from already-matched blobs. Track BFS hop distance per blob.
    std::unordered_set<int> reachable_new_blob_indices;
    std::unordered_map<int, int> blob_match_distance; // blob index -> hop distance
    if (!matched_blob_indices.empty()) {
        std::queue<int> pending;
        std::unordered_set<int> visited;
        for (int idx : matched_blob_indices) {
            pending.push(idx);
            visited.insert(idx);
            blob_match_distance[idx] = 0;
        }
        while (!pending.empty()) {
            const int bi = pending.front(); pending.pop();
            const int parent_dist = blob_match_distance[bi];
            if (parent_dist >= kMaxNewBlobHopDepth) continue;
            for (int ni : blobs[bi].neighbor_blob_indices) {
                if (ni < 0 || ni >= static_cast<int>(blobs.size()) || visited.count(ni)) continue;
                visited.insert(ni);
                pending.push(ni);
                blob_match_distance[ni] = parent_dist + 1;
                if (blobs[ni].matched_node_id < 0) reachable_new_blob_indices.insert(ni);
            }
        }
    }

    // Update match_distance on matched nodes
    for (int bi : matched_blob_indices) {
        auto nit = group.nodes.find(blobs[bi].matched_node_id);
        if (nit != group.nodes.end()) {
            nit->second->match_distance = 0;
        }
    }

    const bool has_reachable_new_nodes = !reachable_new_blob_indices.empty();
    std::unordered_map<int, int> blob_to_graph_node_id;
    blob_to_graph_node_id.reserve(blobs.size());

    // ---------------------------------------------------------------
    // 5b. Battle logic for matched blobs
    // ---------------------------------------------------------------
    const auto matched_refresh_start = SteadyClock::now();
    for (int blob_index : matched_blob_indices) {
        auto& blob = blobs[blob_index];
        auto node_it = group.nodes.find(blob.matched_node_id);
        if (node_it == group.nodes.end()) continue;
        auto& node = *node_it->second;

        const cv::Point2f canvas_centroid = blob.centroid + blob.matched_offset;
        const cv::Rect canvas_bbox(
            blob.bbox.x + static_cast<int>(std::round(blob.matched_offset.x)),
            blob.bbox.y + static_cast<int>(std::round(blob.matched_offset.y)),
            blob.bbox.width, blob.bbox.height);

        const MaskRelation relation = ComputeMaskRelation(
            canvas_bbox, blob.binary_mask, canvas_centroid,
            node.bbox_canvas, node.binary_mask, node.centroid_canvas);

        node.last_seen_frame = current_frame;
        node.seen_count++;
        node.max_area_seen = std::max(std::max(node.max_area_seen, node.area), blob.area);

        const float centroid_shift = static_cast<float>(
            cv::norm(canvas_centroid - node.centroid_canvas));
        const bool bbox_changed = canvas_bbox != node.bbox_canvas;
        const bool area_grew = blob.area > std::max(1.0, node.area) * kBattleReplaceAreaGain;
        const bool overlap_refresh = relation.valid &&
                                     relation.overlap_over_min >= kBattleRefreshOverlap;
        const bool overlap_replace = relation.valid &&
                                     relation.overlap_over_min >= kBattleReplaceOverlap;
        const bool frame_wins = has_reachable_new_nodes &&
            ((overlap_refresh && (bbox_changed || centroid_shift > kBattleShiftThresholdPx)) ||
             (overlap_replace && area_grew) ||
             (bbox_changed && area_grew));

        if (frame_wins) {
            RefreshNodeFromBlob(group, node, blob, canvas_centroid, canvas_bbox);
            graph_changed = true;
        }

        blob_to_graph_node_id[blob_index] = node.id;
    }
    RecordProfileSample(ProfileStep::kMatchedRefresh,
                        ElapsedMs(matched_refresh_start, SteadyClock::now()));

    // ---------------------------------------------------------------
    // 5b2. Absence tracking with asymmetric EMA
    // ---------------------------------------------------------------
    {
        const auto absence_start = SteadyClock::now();

        // Viewport in canvas coordinates
        const cv::Rect viewport_canvas(
            static_cast<int>(std::round(global_camera_pos_.x)),
            static_cast<int>(std::round(global_camera_pos_.y)),
            frame_w_, frame_h_);

        // Lecturer rect in canvas coordinates (current frame)
        const cv::Rect& lecturer_canvas = lecturer_canvas_rect;

        // Collect IDs of all matched (seen) nodes this frame
        std::unordered_set<int> seen_node_ids;
        seen_node_ids.reserve(matched_blob_indices.size());
        for (int bi : matched_blob_indices) {
            seen_node_ids.insert(blobs[bi].matched_node_id);
        }

        // Find absent nodes: near a matched node, in viewport, not behind lecturer
        static constexpr float kAbsenceNearbyRadius = 100.0f;
        std::unordered_set<int> absent_node_ids;

        for (int bi : matched_blob_indices) {
            const auto& blob = blobs[bi];
            auto node_it = group.nodes.find(blob.matched_node_id);
            if (node_it == group.nodes.end()) continue;
            const auto& matched_node = *node_it->second;

            auto nearby_ids = group.spatial_index.QueryRadius(
                matched_node.centroid_canvas, kAbsenceNearbyRadius);

            for (int nearby_id : nearby_ids) {
                if (seen_node_ids.count(nearby_id)) continue;
                if (absent_node_ids.count(nearby_id)) continue;

                auto nit = group.nodes.find(nearby_id);
                if (nit == group.nodes.end()) continue;

                const cv::Point2i centroid_px(
                    static_cast<int>(std::round(nit->second->centroid_canvas.x)),
                    static_cast<int>(std::round(nit->second->centroid_canvas.y)));
                if (!viewport_canvas.contains(centroid_px)) continue;

                if (lecturer_canvas.width > 0 && lecturer_canvas.height > 0 &&
                    lecturer_canvas.contains(centroid_px)) continue;

                absent_node_ids.insert(nearby_id);
            }
        }

        // Apply asymmetric EMA: slow build when seen, fast decay when absent
        static constexpr float kAbsenceAlphaSeen  = 0.9f;  // fast build
        static constexpr float kAbsenceAlphaAbsent = 0.1f;  // slow decay

        // Seen nodes: delta = +1
        for (int nid : seen_node_ids) {
            auto nit = group.nodes.find(nid);
            if (nit == group.nodes.end()) continue;
            // If also absent (near another matched node), delta cancels to 0
            if (absent_node_ids.count(nid)) continue;
            auto& node = *nit->second;
            node.absence_score = node.absence_score * (1.0f - kAbsenceAlphaSeen)
                               + 1.0f * kAbsenceAlphaSeen;
        }

        // Absent nodes: delta = -1
        std::vector<int> nodes_to_remove;
        for (int nid : absent_node_ids) {
            // If also seen, delta cancels to 0 — skip
            if (seen_node_ids.count(nid)) continue;
            auto nit = group.nodes.find(nid);
            if (nit == group.nodes.end()) continue;
            auto& node = *nit->second;
            node.absence_score = node.absence_score * (1.0f - kAbsenceAlphaAbsent)
                               + (-1.0f) * kAbsenceAlphaAbsent;
            if (node.absence_score < 0.0f) {
                nodes_to_remove.push_back(nid);
            }
        }

        for (int nid : nodes_to_remove) {
            RemoveNodeFromGraph(group, nid);
            graph_changed = true;
        }

        RecordProfileSample(ProfileStep::kAbsenceTracking,
                            ElapsedMs(absence_start, SteadyClock::now()));
    }

    // ---------------------------------------------------------------
    // 5c. Candidate-based new node placement
    // ---------------------------------------------------------------
    const auto candidate_process_start = SteadyClock::now();
    ProcessCandidateBlobs(group, blobs, reachable_new_blob_indices,
                              blob_to_graph_node_id,
                              cropped_frame,
                              current_frame, graph_changed);
    RecordProfileSample(ProfileStep::kCandidateProcess,
                        ElapsedMs(candidate_process_start, SteadyClock::now()));

    // Set match_distance on newly placed nodes from blob hop distance
    for (const auto& bm : blob_to_graph_node_id) {
        auto dist_it = blob_match_distance.find(bm.first);
        if (dist_it == blob_match_distance.end()) continue;
        auto nit = group.nodes.find(bm.second);
        if (nit != group.nodes.end()) {
            nit->second->match_distance = dist_it->second;
        }
    }

    // ---------------------------------------------------------------
    // 5d. Merge overlapping duplicate nodes
    // ---------------------------------------------------------------
    const auto merge_start = SteadyClock::now();
    MergeOverlappingNodes(group, current_frame, graph_changed);
    RecordProfileSample(ProfileStep::kMergeNodes,
                        ElapsedMs(merge_start, SteadyClock::now()));

    // ---------------------------------------------------------------
    // Finalize
    // ---------------------------------------------------------------
    if (graph_changed) {
        const auto neighbor_start = SteadyClock::now();
        RebuildGroupNeighborGraph(group, kKNeighbors);
        RecordProfileSample(ProfileStep::kNeighborRebuild,
                            ElapsedMs(neighbor_start, SteadyClock::now()));
        UpdateGroupBounds(group);
        group.stroke_cache_dirty = true;
        group.raw_cache_dirty = true;
        BumpCanvasVersion();
    }

    return graph_changed;
}

// ---------------------------------------------------------------------------
// RemoveNodeFromGraph — safely remove a node and patch neighbour lists
// ---------------------------------------------------------------------------
void WhiteboardCanvas::RemoveNodeFromGraph(WhiteboardGroup& group, int node_id) {
    auto it = group.nodes.find(node_id);
    if (it == group.nodes.end()) return;
    group.spatial_index.Remove(node_id, it->second->centroid_canvas);
    for (int neighbor_id : it->second->neighbor_ids) {
        auto nit = group.nodes.find(neighbor_id);
        if (nit != group.nodes.end()) {
            auto& nvec = nit->second->neighbor_ids;
            nvec.erase(std::remove(nvec.begin(), nvec.end(), node_id), nvec.end());
        }
    }
    group.nodes.erase(it);
}

// ---------------------------------------------------------------------------
// ProcessCandidateBlobs — route unmatched blobs through the candidate pool
// ---------------------------------------------------------------------------
void WhiteboardCanvas::ProcessCandidateBlobs(
        WhiteboardGroup& group,
        std::vector<FrameBlob>& blobs,
        const std::unordered_set<int>& reachable_new_blob_indices,
        std::unordered_map<int, int>& blob_to_graph_node_id,
        const cv::Rect& cropped_frame,
        int current_frame,
        bool& graph_changed) {
    if (!kEnableCandidateStaging) {
        group.candidates.clear();
        group.next_candidate_id = 0;

        if (reachable_new_blob_indices.empty()) {
            return;
        }

        std::unordered_set<int> pending_new(reachable_new_blob_indices);
        while (!pending_new.empty()) {
            bool placed_any = false;
            std::vector<int> placed;

            for (int blob_index : pending_new) {
                auto& blob = blobs[blob_index];

                std::vector<int> anchor_node_ids;
                std::vector<float> offset_xs, offset_ys;
                for (int ni : blob.neighbor_blob_indices) {
                    auto mapping_it = blob_to_graph_node_id.find(ni);
                    if (mapping_it == blob_to_graph_node_id.end()) continue;
                    auto anchor_it = group.nodes.find(mapping_it->second);
                    if (anchor_it == group.nodes.end()) continue;
                    AppendUniqueId(anchor_node_ids, anchor_it->second->id);
                    const cv::Point2f predicted =
                        anchor_it->second->centroid_canvas - blobs[ni].centroid;
                    offset_xs.push_back(predicted.x);
                    offset_ys.push_back(predicted.y);
                }
                if (offset_xs.empty()) continue;

                if (blob.bbox.x <= cropped_frame.x ||
                    blob.bbox.y <= cropped_frame.y ||
                    blob.bbox.x + blob.bbox.width >= cropped_frame.x + cropped_frame.width ||
                    blob.bbox.y + blob.bbox.height >= cropped_frame.y + cropped_frame.height) {
                    continue;
                }

                std::sort(offset_xs.begin(), offset_xs.end());
                std::sort(offset_ys.begin(), offset_ys.end());
                const int mid = static_cast<int>(offset_xs.size() / 2);
                const cv::Point2f final_offset(offset_xs[mid], offset_ys[mid]);

                const int node_id = AppendFrameBlobToGroup(
                    group, blob, final_offset, anchor_node_ids, current_frame);

                for (int anchor_id : anchor_node_ids) {
                    auto anchor_it = group.nodes.find(anchor_id);
                    if (anchor_it != group.nodes.end()) {
                        AppendUniqueId(anchor_it->second->neighbor_ids, node_id);
                    }
                }

                blob_to_graph_node_id[blob_index] = node_id;
                graph_changed = true;
                placed.push_back(blob_index);
                placed_any = true;
            }

            for (int blob_index : placed) {
                pending_new.erase(blob_index);
            }
            if (!placed_any) break;
        }

        return;
    }

    auto expire_absent_candidates = [&](const std::unordered_set<int>& candidate_visited) {
        std::vector<int> expired;
        for (auto& cp : group.candidates) {
            auto& cand = *cp.second;
            if (candidate_visited.count(cp.first)) {
                cand.absence_count = 0;
                continue;
            }

            cand.absence_count++;
            if (cand.absence_count >= kCandidateExpireFrames) {
                expired.push_back(cp.first);
            }
        }

        for (int cid : expired) {
            group.candidates.erase(cid);
        }
    };

    if (reachable_new_blob_indices.empty()) {
        // Still need to age out candidates that were not seen again.
        expire_absent_candidates({});
        return;
    }

    std::unordered_set<int> candidate_visited;
    std::unordered_set<int> pending_new(reachable_new_blob_indices);

    while (!pending_new.empty()) {
        bool placed_any = false;
        std::vector<int> placed;

        for (int blob_index : pending_new) {
            auto& blob = blobs[blob_index];

            // Compute placement offset from nearby matched anchors
            std::vector<int> anchor_node_ids;
            std::vector<float> offset_xs, offset_ys;
            for (int ni : blob.neighbor_blob_indices) {
                auto mapping_it = blob_to_graph_node_id.find(ni);
                if (mapping_it == blob_to_graph_node_id.end()) continue;
                auto anchor_it = group.nodes.find(mapping_it->second);
                if (anchor_it == group.nodes.end()) continue;
                AppendUniqueId(anchor_node_ids, anchor_it->second->id);
                const cv::Point2f predicted = anchor_it->second->centroid_canvas - blobs[ni].centroid;
                offset_xs.push_back(predicted.x);
                offset_ys.push_back(predicted.y);
            }
            if (offset_xs.empty()) continue;

            // Skip blobs touching the cropped frame border (partial shapes)
            if (blob.bbox.x <= cropped_frame.x ||
                blob.bbox.y <= cropped_frame.y ||
                blob.bbox.x + blob.bbox.width >= cropped_frame.x + cropped_frame.width ||
                blob.bbox.y + blob.bbox.height >= cropped_frame.y + cropped_frame.height) {
                continue;
            }

            std::sort(offset_xs.begin(), offset_xs.end());
            std::sort(offset_ys.begin(), offset_ys.end());
            const int mid = static_cast<int>(offset_xs.size() / 2);
            const cv::Point2f final_offset(offset_xs[mid], offset_ys[mid]);
            const cv::Point2f canvas_centroid = blob.centroid + final_offset;
            const cv::Rect canvas_bbox(
                blob.bbox.x + static_cast<int>(std::round(final_offset.x)),
                blob.bbox.y + static_cast<int>(std::round(final_offset.y)),
                blob.bbox.width, blob.bbox.height);

            // Try to match to an existing candidate
            int best_cand_id = -1;
            double best_cand_dist = 1e9;
            for (const auto& cp : group.candidates) {
                const auto& cand = *cp.second;
                const float cdist = static_cast<float>(cv::norm(canvas_centroid - cand.centroid_canvas));
                if (cdist > kCandidateMatchRadiusPx) continue;
                if (cand.contour.empty() || blob.contour.empty()) continue;
                const double shape_dist = cv::matchShapes(
                    blob.contour, cand.contour, cv::CONTOURS_MATCH_I2, 0);
                if (shape_dist > kCandidateMatchShapeDist) continue;
                const double combined = static_cast<double>(cdist) + shape_dist * 100.0;
                if (combined < best_cand_dist) {
                    best_cand_dist = combined;
                    best_cand_id = cp.first;
                }
            }

            if (best_cand_id >= 0) {
                auto& cand = *group.candidates[best_cand_id];
                cand.binary_mask = blob.binary_mask.clone();
                if (!blob.color_pixels.empty()) cand.color_pixels = blob.color_pixels.clone();
                cand.bbox_canvas = canvas_bbox;
                cand.centroid_canvas = canvas_centroid;
                cand.contour = blob.contour;
                std::copy(blob.hu, blob.hu + 7, cand.hu);
                cand.area = blob.area;
                cand.absence_count = 0;
                cand.last_seen_frame = current_frame;
                cand.seen_count++;
                for (int aid : anchor_node_ids) AppendUniqueId(cand.anchor_node_ids, aid);
                candidate_visited.insert(best_cand_id);

                // Promote to real node if confirmed enough times
                if (cand.seen_count >= kCandidateConfirmFrames) {
                    auto nn = std::make_unique<DrawingNode>();
                    nn->id = group.next_node_id++;
                    nn->binary_mask = cand.binary_mask.clone();
                    if (!cand.color_pixels.empty()) nn->color_pixels = cand.color_pixels.clone();
                    nn->bbox_canvas = cand.bbox_canvas;
                    nn->centroid_canvas = cand.centroid_canvas;
                    nn->contour = cand.contour;
                    std::copy(cand.hu, cand.hu + 7, nn->hu);
                    nn->area = cand.area;
                    nn->max_area_seen = cand.area;
                    nn->absence_score = kAbsenceScoreInitial;
                    nn->last_seen_frame = current_frame;
                    nn->created_frame = cand.created_frame;
                    nn->seen_count = cand.seen_count;
                    for (int aid : cand.anchor_node_ids) AppendUniqueId(nn->neighbor_ids, aid);

                    const int nid = nn->id;
                    group.spatial_index.Insert(nid, cand.centroid_canvas);
                    group.nodes[nid] = std::move(nn);

                    for (int aid : cand.anchor_node_ids) {
                        auto ait = group.nodes.find(aid);
                        if (ait != group.nodes.end()) {
                            AppendUniqueId(ait->second->neighbor_ids, nid);
                        }
                    }
                    blob_to_graph_node_id[blob_index] = nid;
                    graph_changed = true;

                    group.candidates.erase(best_cand_id);
                }
            } else {
                // Create a new candidate
                auto nc = std::make_unique<CandidateNode>();
                nc->id = group.next_candidate_id++;
                nc->binary_mask = blob.binary_mask.clone();
                if (!blob.color_pixels.empty()) nc->color_pixels = blob.color_pixels.clone();
                nc->bbox_canvas = canvas_bbox;
                nc->centroid_canvas = canvas_centroid;
                nc->contour = blob.contour;
                std::copy(blob.hu, blob.hu + 7, nc->hu);
                nc->area = blob.area;
                nc->seen_count = 1;
                nc->absence_count = 0;
                nc->last_seen_frame = current_frame;
                nc->created_frame = current_frame;
                nc->anchor_node_ids = anchor_node_ids;

                const int cid = nc->id;
                group.candidates[cid] = std::move(nc);
                candidate_visited.insert(cid);
            }

            placed.push_back(blob_index);
            placed_any = true;
        }

        for (int bi : placed) pending_new.erase(bi);
        if (!placed_any) break;
    }

    expire_absent_candidates(candidate_visited);
}

// ---------------------------------------------------------------------------
// MergeOverlappingNodes — collapse duplicate graph nodes without erasing notes
// ---------------------------------------------------------------------------
void WhiteboardCanvas::MergeOverlappingNodes(WhiteboardGroup& group,
                                             int current_frame,
                                             bool& graph_changed) {
                                                static constexpr float kMergeBboxOverlap = 0.70f;
    static constexpr double kMergeShapeDist = 0.5;
    static constexpr double kMergeAlignedShapeDist = 0.22;
    static constexpr float kMergeAlignedMaskOverlap = 0.72f;
    static constexpr float kMergeSmallerMaskOverlapDuplicate = 0.10f;
    static constexpr float kMergeAlignedAreaRatioMin = 0.45f;
    static constexpr float kMergeContainmentMin = 0.35f;
    static constexpr float kMergeBboxAreaRatio = 0.70f;
    static constexpr float kMergeBboxIoU = 0.70f;

    // Close-proximity duplicate: centroids within this distance + similar bbox
    static constexpr float kMergeProximityDistPx = 30.0f;
    static constexpr float kMergeProximityBboxSimilarity = 0.80f;

    // Center-aligned and side-aligned duplicate thresholds
    static constexpr float kMergeCenterAlignedOverlap = 0.60f;   // AND/min_pixels when centroids aligned
    static constexpr float kMergeSideAlignedOverlap   = 0.60f;   // AND/min_pixels when snapped to same edge

    auto compute_aligned_mask_overlap = [&](const DrawingNode& a,
                                            const DrawingNode& b) -> float {
        if (a.binary_mask.empty() || b.binary_mask.empty()) return 0.0f;
        if (a.binary_mask.cols <= 0 || a.binary_mask.rows <= 0 ||
            b.binary_mask.cols <= 0 || b.binary_mask.rows <= 0) {
            return 0.0f;
        }

        const int left = std::min(a.bbox_canvas.x, b.bbox_canvas.x);
        const int top = std::min(a.bbox_canvas.y, b.bbox_canvas.y);
        const int right = std::max(a.bbox_canvas.x + a.bbox_canvas.width,
                                   b.bbox_canvas.x + b.bbox_canvas.width);
        const int bottom = std::max(a.bbox_canvas.y + a.bbox_canvas.height,
                                    b.bbox_canvas.y + b.bbox_canvas.height);
        const int uw = std::max(1, right - left);
        const int uh = std::max(1, bottom - top);

        cv::Mat a_aligned = cv::Mat::zeros(uh, uw, CV_8UC1);
        cv::Mat b_aligned = cv::Mat::zeros(uh, uw, CV_8UC1);

        const cv::Rect a_roi(
            a.bbox_canvas.x - left,
            a.bbox_canvas.y - top,
            a.binary_mask.cols,
            a.binary_mask.rows);
        const cv::Rect b_roi(
            b.bbox_canvas.x - left,
            b.bbox_canvas.y - top,
            b.binary_mask.cols,
            b.binary_mask.rows);

        if (a_roi.x < 0 || a_roi.y < 0 ||
            a_roi.x + a_roi.width > uw || a_roi.y + a_roi.height > uh ||
            b_roi.x < 0 || b_roi.y < 0 ||
            b_roi.x + b_roi.width > uw || b_roi.y + b_roi.height > uh) {
            return 0.0f;
        }

        a.binary_mask.copyTo(a_aligned(a_roi));
        b.binary_mask.copyTo(b_aligned(b_roi));

        cv::Mat overlap_mask;
        cv::bitwise_and(a_aligned, b_aligned, overlap_mask);
        const int overlap_px = cv::countNonZero(overlap_mask);
        const int min_px = std::min(cv::countNonZero(a.binary_mask),
                                    cv::countNonZero(b.binary_mask));
        if (min_px <= 0) return 0.0f;
        return static_cast<float>(overlap_px) / static_cast<float>(min_px);
    };

    auto canonical_score = [&](const DrawingNode& node) -> double {
        const double stable_area = std::max(node.max_area_seen, node.area);
        const double seen_bonus = static_cast<double>(std::max(1, node.seen_count)) * 250.0;
        const double age_bonus = static_cast<double>(
            std::max(0, current_frame - node.created_frame)) * 10.0;
        const double neighbor_bonus = static_cast<double>(node.neighbor_ids.size()) * 15.0;
        const double absence_penalty = (node.absence_score < 0.0f)
            ? static_cast<double>(-node.absence_score) * 100.0 : 0.0;
        return stable_area * 2.0 + seen_bonus + age_bonus + neighbor_bonus - absence_penalty;
    };

    // Compute bbox width/height similarity (min/max for each dimension)
    auto bbox_dimension_similarity = [](const cv::Rect& a, const cv::Rect& b) -> float {
        const float w_sim = static_cast<float>(std::min(a.width, b.width)) /
                            static_cast<float>(std::max(1, std::max(a.width, b.width)));
        const float h_sim = static_cast<float>(std::min(a.height, b.height)) /
                            static_cast<float>(std::max(1, std::max(a.height, b.height)));
        return std::min(w_sim, h_sim);
    };

    // Center-aligned overlap: translate both masks so their centroids coincide,
    // then compute bitwise AND / min_pixels.
    auto compute_center_aligned_overlap = [](const DrawingNode& a,
                                             const DrawingNode& b) -> float {
        if (a.binary_mask.empty() || b.binary_mask.empty()) return 0.0f;
        const float centroid_dist = static_cast<float>(cv::norm(a.centroid_canvas - b.centroid_canvas));
        if (centroid_dist > WhiteboardCanvas::kToFarToBeSame) return 0.0f;
        const int a_px = cv::countNonZero(a.binary_mask);
        const int b_px = cv::countNonZero(b.binary_mask);
        if (a_px <= 0 || b_px <= 0) return 0.0f;

        // Place both masks in a common canvas centered on (0,0) at each centroid
        const int a_cx = a.binary_mask.cols / 2;
        const int a_cy = a.binary_mask.rows / 2;
        const int b_cx = b.binary_mask.cols / 2;
        const int b_cy = b.binary_mask.rows / 2;

        const int half_w = std::max(std::max(a_cx, a.binary_mask.cols - a_cx),
                                    std::max(b_cx, b.binary_mask.cols - b_cx));
        const int half_h = std::max(std::max(a_cy, a.binary_mask.rows - a_cy),
                                    std::max(b_cy, b.binary_mask.rows - b_cy));
        const int uw = half_w * 2;
        const int uh = half_h * 2;
        if (uw <= 0 || uh <= 0) return 0.0f;

        cv::Mat a_aligned = cv::Mat::zeros(uh, uw, CV_8UC1);
        cv::Mat b_aligned = cv::Mat::zeros(uh, uw, CV_8UC1);

        const int a_ox = half_w - a_cx;
        const int a_oy = half_h - a_cy;
        const int b_ox = half_w - b_cx;
        const int b_oy = half_h - b_cy;

        cv::Rect a_roi(a_ox, a_oy, a.binary_mask.cols, a.binary_mask.rows);
        cv::Rect b_roi(b_ox, b_oy, b.binary_mask.cols, b.binary_mask.rows);
        a_roi &= cv::Rect(0, 0, uw, uh);
        b_roi &= cv::Rect(0, 0, uw, uh);
        if (a_roi.empty() || b_roi.empty()) return 0.0f;

        // Increase thickness by dilating masks
        cv::Mat thick_a, thick_b;
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
        cv::dilate(a.binary_mask(cv::Rect(0, 0, a_roi.width, a_roi.height)), thick_a, kernel);
        cv::dilate(b.binary_mask(cv::Rect(0, 0, b_roi.width, b_roi.height)), thick_b, kernel);
        thick_a.copyTo(a_aligned(a_roi));
        thick_b.copyTo(b_aligned(b_roi));

        cv::Mat overlap;
        cv::bitwise_and(a_aligned, b_aligned, overlap);
        const int overlap_px = cv::countNonZero(overlap);

        // IoU calculation
        cv::Mat union_mask;
        cv::bitwise_or(a_aligned, b_aligned, union_mask);
        const int union_px = cv::countNonZero(union_mask);
        float iou = union_px > 0 ? static_cast<float>(overlap_px) / static_cast<float>(union_px) : 0.0f;
        float and_ratio = static_cast<float>(overlap_px) / static_cast<float>(std::min(a_px, b_px));
        if (iou > 0.4f) return 1.0f;
        if (std::max(a_px, b_px) > 100 && and_ratio > 0.6f) return 1.0f;

        return 0.0f;
    };

    // Side-aligned overlap: snap both masks to each edge (top, bottom, left, right)
    // of a common bounding box, compute AND/min for each, return the max.
    auto compute_side_aligned_overlap = [](const DrawingNode& a,
                                           const DrawingNode& b) -> float {
        if (a.binary_mask.empty() || b.binary_mask.empty()) return 0.0f;
        const float centroid_dist = static_cast<float>(cv::norm(a.centroid_canvas - b.centroid_canvas));
        if (centroid_dist > WhiteboardCanvas::kToFarToBeSame) return 0.0f;
        const int a_px = cv::countNonZero(a.binary_mask);
        const int b_px = cv::countNonZero(b.binary_mask);
        if (a_px <= 0 || b_px <= 0) return 0.0f;

        const int uw = std::max(a.binary_mask.cols, b.binary_mask.cols);
        const int uh = std::max(a.binary_mask.rows, b.binary_mask.rows);
        if (uw <= 0 || uh <= 0) return 0.0f;

        struct Snap { int a_ox, a_oy, b_ox, b_oy; };
        const Snap snaps[4] = {
            {0, 0, 0, 0},
            {uw - a.binary_mask.cols, 0, uw - b.binary_mask.cols, 0},
            {0, uh - a.binary_mask.rows, 0, uh - b.binary_mask.rows},
            {uw - a.binary_mask.cols, uh - a.binary_mask.rows,
             uw - b.binary_mask.cols, uh - b.binary_mask.rows},
        };

        for (const auto& s : snaps) {
            cv::Mat a_aligned = cv::Mat::zeros(uh, uw, CV_8UC1);
            cv::Mat b_aligned = cv::Mat::zeros(uh, uw, CV_8UC1);

            cv::Rect a_roi(s.a_ox, s.a_oy, a.binary_mask.cols, a.binary_mask.rows);
            cv::Rect b_roi(s.b_ox, s.b_oy, b.binary_mask.cols, b.binary_mask.rows);
            a_roi &= cv::Rect(0, 0, uw, uh);
            b_roi &= cv::Rect(0, 0, uw, uh);
            if (a_roi.empty() || b_roi.empty()) continue;

            // Increase thickness by dilating masks
            cv::Mat thick_a, thick_b;
            cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
            cv::dilate(a.binary_mask(cv::Rect(0, 0, a_roi.width, a_roi.height)), thick_a, kernel);
            cv::dilate(b.binary_mask(cv::Rect(0, 0, b_roi.width, b_roi.height)), thick_b, kernel);
            thick_a.copyTo(a_aligned(a_roi));
            thick_b.copyTo(b_aligned(b_roi));

            cv::Mat overlap;
            cv::bitwise_and(a_aligned, b_aligned, overlap);
            const int overlap_px = cv::countNonZero(overlap);

            cv::Mat union_mask;
            cv::bitwise_or(a_aligned, b_aligned, union_mask);
            const int union_px = cv::countNonZero(union_mask);
            float iou = union_px > 0 ? static_cast<float>(overlap_px) / static_cast<float>(union_px) : 0.0f;
            float and_ratio = static_cast<float>(overlap_px) / static_cast<float>(std::min(a_px, b_px));
            if (iou > 0.4f) return 1.0f;
            // if the its a big shape use also and ratio to detect containment duplicates
            if (std::max(a_px, b_px) > 100 && and_ratio > 0.6f) return 1.0f;
        }
        return 0.0f;
    };

    std::vector<std::pair<int, int>> merge_pairs;
    std::unordered_set<int> marked_for_removal;

    // Neighbor-based merge: for each node, only check its neighbors + nearby nodes
    for (const auto& pair : group.nodes) {
        const int id_a = pair.first;
        if (marked_for_removal.count(id_a)) continue;
        const auto& a = *pair.second;

        // Build candidate set: graph neighbors + spatial proximity query
        std::unordered_set<int> candidate_ids;
        for (int nid : a.neighbor_ids) {
            candidate_ids.insert(nid);
        }
        // Also query spatial index for close-proximity nodes that may not
        // be graph neighbors yet (e.g. freshly added duplicates)
        const float search_radius = std::max(kMergeProximityDistPx,
            std::max(static_cast<float>(a.bbox_canvas.width),
                     static_cast<float>(a.bbox_canvas.height)) * 0.5f);
        const auto nearby = group.spatial_index.QueryRadius(
            a.centroid_canvas, search_radius);
        for (int nid : nearby) {
            candidate_ids.insert(nid);
        }
        candidate_ids.erase(id_a);

        for (int id_b : candidate_ids) {
            if (marked_for_removal.count(id_b)) continue;
            // Avoid duplicate pair evaluation: only process A < B
            if (id_a > id_b) continue;
            auto it_b = group.nodes.find(id_b);
            if (it_b == group.nodes.end()) continue;
            const auto& b = *it_b->second;

            const float centroid_dist =
                static_cast<float>(cv::norm(a.centroid_canvas - b.centroid_canvas));

            // --- Close-proximity duplicate: close centroids + similar bbox dimensions ---
            const bool proximity_duplicate =
                centroid_dist <= kMergeProximityDistPx &&
                bbox_dimension_similarity(a.bbox_canvas, b.bbox_canvas)
                    >= kMergeProximityBboxSimilarity;

            // --- Original merge criteria ---
            const cv::Rect isect = a.bbox_canvas & b.bbox_canvas;
            const int isect_area = isect.empty() ? 0 : isect.width * isect.height;
            const int area_a = std::max(1, a.bbox_canvas.width * a.bbox_canvas.height);
            const int area_b = std::max(1, b.bbox_canvas.width * b.bbox_canvas.height);
            const int min_area = std::min(area_a, area_b);

            const float bbox_overlap = (min_area > 0)
                ? static_cast<float>(isect_area) / static_cast<float>(min_area) : 0.0f;
            const int bbox_union_area = area_a + area_b - isect_area;
            const float bbox_area_ratio = static_cast<float>(min_area) /
                static_cast<float>(std::max(area_a, area_b));
            const float bbox_iou = static_cast<float>(isect_area) /
                static_cast<float>(std::max(1, bbox_union_area));

            const bool near_identical_bbox =
                bbox_area_ratio >= kMergeBboxAreaRatio && bbox_iou >= kMergeBboxIoU;
            const float max_dim = static_cast<float>(std::max(
                std::max(a.bbox_canvas.width, a.bbox_canvas.height),
                std::max(b.bbox_canvas.width, b.bbox_canvas.height)));
            const float centroid_limit = std::max(60.0f, max_dim * 0.45f);
            const float area_ratio = static_cast<float>(
                std::min(a.area, b.area) / std::max(1.0, std::max(a.area, b.area)));

            const bool maybe_shifted = bbox_overlap < kMergeBboxOverlap &&
                centroid_dist <= centroid_limit && area_ratio >= kMergeAlignedAreaRatioMin;

            const bool allow_bbox = kEnableMergeNearIdenticalBbox && near_identical_bbox;
            const bool allow_overlap_shape = kEnableMergeOverlapShapeContainment &&
                                             bbox_overlap >= kMergeBboxOverlap;
            const bool allow_shifted = kEnableMergeShiftedDuplicate && maybe_shifted;

            const MaskRelation raw_rel = ComputeMaskRelation(
                a.bbox_canvas, a.binary_mask, a.centroid_canvas,
                b.bbox_canvas, b.binary_mask, b.centroid_canvas);
            const bool smaller_mask_overlap_duplicate = raw_rel.valid &&
                raw_rel.overlap_over_min >= kMergeSmallerMaskOverlapDuplicate;

            // Center-aligned duplicate: align centroids, check AND/min
            const bool allow_center_aligned = kEnableMergeCenterAlignedDuplicate &&
                centroid_dist <= centroid_limit;
            // Side-aligned duplicate: snap to each corner, check AND/min
            const bool allow_side_aligned = kEnableMergeSideAlignedDuplicate &&
                centroid_dist <= centroid_limit;

            if (!proximity_duplicate && !allow_bbox && !allow_overlap_shape &&
                !allow_shifted && !smaller_mask_overlap_duplicate &&
                !allow_center_aligned && !allow_side_aligned) {
                continue;
            }

            bool should_merge = proximity_duplicate || allow_bbox;
            bool merged_by_alignment = false;

            if (!should_merge) {
                const double shape_dist = cv::matchShapes(a.contour, b.contour,
                                                          cv::CONTOURS_MATCH_I2, 0);
                const float containment_min = raw_rel.valid
                    ? std::min(raw_rel.overlap_over_first, raw_rel.overlap_over_second)
                    : 0.0f;

                should_merge = allow_overlap_shape && shape_dist < kMergeShapeDist &&
                    containment_min >= kMergeContainmentMin;

                if (!should_merge && allow_shifted && shape_dist < kMergeAlignedShapeDist) {
                    should_merge = compute_aligned_mask_overlap(a, b) >= kMergeAlignedMaskOverlap;
                }

                if (!should_merge && smaller_mask_overlap_duplicate) {
                    should_merge = true;
                }

                // Center-aligned duplicate check
                if (!should_merge && allow_center_aligned) {
                    const float center_overlap = compute_center_aligned_overlap(a, b);
                    if (center_overlap >= kMergeCenterAlignedOverlap) {
                        should_merge = true;
                        merged_by_alignment = true;
                    }
                }

                // Side-aligned duplicate check
                if (!should_merge && allow_side_aligned) {
                    const float side_overlap = compute_side_aligned_overlap(a, b);
                    if (side_overlap >= kMergeSideAlignedOverlap) {
                        should_merge = true;
                        merged_by_alignment = true;
                    }
                }
            }

            if (should_merge) {
                int keep_id = id_a;
                int remove_id = id_b;

                // Always keep the node with the larger mask area, or if equal, the higher canonical score
                if (raw_rel.second_px > raw_rel.first_px) {
                    std::swap(keep_id, remove_id);
                } else if (raw_rel.second_px == raw_rel.first_px) {
                    if (canonical_score(b) > canonical_score(a)) {
                        std::swap(keep_id, remove_id);
                    }
                }
                merge_pairs.emplace_back(keep_id, remove_id);
                marked_for_removal.insert(remove_id);
            }
        }
    }

    // --- Brute-force containment check ---
    // For pairs with intersecting bboxes or centroids within kContainCentroidDist,
    // slide the smaller mask over a 2x search area. If the smaller
    // stroke is >kContainThreshold contained in any position, delete it.
    if (kEnableContainmentFilter) {
        for (const auto& pair_a : group.nodes) {
            const int id_a = pair_a.first;
            if (marked_for_removal.count(id_a)) continue;
            const auto& a = *pair_a.second;
            if (a.binary_mask.empty()) continue;

            const auto nearby = group.spatial_index.QueryRadius(
                a.centroid_canvas,
                std::max(static_cast<float>(a.bbox_canvas.width),
                         static_cast<float>(a.bbox_canvas.height)) * 1.5f);

            for (int id_b : nearby) {
                if (id_b <= id_a) continue; // avoid duplicates
                if (marked_for_removal.count(id_b)) continue;
                auto it_b = group.nodes.find(id_b);
                if (it_b == group.nodes.end()) continue;
                const auto& b = *it_b->second;
                if (b.binary_mask.empty()) continue;

                const float cdist = static_cast<float>(
                    cv::norm(a.centroid_canvas - b.centroid_canvas));
                const cv::Rect isect = a.bbox_canvas & b.bbox_canvas;
                const bool bboxes_intersect = !isect.empty();

                if (!bboxes_intersect && cdist > kContainCentroidDist) continue;

                // Determine smaller / larger by pixel area
                const int a_px = cv::countNonZero(a.binary_mask);
                const int b_px = cv::countNonZero(b.binary_mask);
                const auto& smaller_node = (a_px <= b_px) ? a : b;
                const auto& bigger_node  = (a_px <= b_px) ? b : a;
                const int smaller_id = (a_px <= b_px) ? id_a : id_b;
                const int bigger_id  = (a_px <= b_px) ? id_b : id_a;
                const int smaller_px = std::min(a_px, b_px);
                if (smaller_px <= 0) continue;

                // Search area: 2x the smaller mask dimensions, centered on its canvas position
                const int sw = smaller_node.binary_mask.cols;
                const int sh = smaller_node.binary_mask.rows;
                const int search_w = sw * 2;
                const int search_h = sh * 2;

                // Canvas region of the bigger node
                const int big_left   = bigger_node.bbox_canvas.x;
                const int big_top    = bigger_node.bbox_canvas.y;

                // Search center = smaller node canvas position
                const int cx = smaller_node.bbox_canvas.x + sw / 2;
                const int cy = smaller_node.bbox_canvas.y + sh / 2;
                const int search_left = cx - search_w / 2;
                const int search_top  = cy - search_h / 2;

                bool found_containment = false;
                for (int dy = 0; dy <= search_h - sh && !found_containment; dy += kContainStepPx) {
                    for (int dx = 0; dx <= search_w - sw && !found_containment; dx += kContainStepPx) {
                        // Small mask placed at canvas (sx, sy)
                        const int sx = search_left + dx;
                        const int sy = search_top  + dy;

                        // Intersection with big mask in canvas coords
                        const cv::Rect small_rect(sx, sy, sw, sh);
                        const cv::Rect big_rect(big_left, big_top,
                                                bigger_node.binary_mask.cols, bigger_node.binary_mask.rows);
                        const cv::Rect overlap_rect = small_rect & big_rect;
                        if (overlap_rect.empty()) continue;

                        // Local coords in each mask
                        const cv::Rect s_local(overlap_rect.x - sx,
                                               overlap_rect.y - sy,
                                               overlap_rect.width, overlap_rect.height);
                        const cv::Rect b_local(overlap_rect.x - big_left,
                                               overlap_rect.y - big_top,
                                               overlap_rect.width, overlap_rect.height);

                        if (s_local.x < 0 || s_local.y < 0 ||
                            s_local.x + s_local.width > sw ||
                            s_local.y + s_local.height > sh) continue;
                        if (b_local.x < 0 || b_local.y < 0 ||
                            b_local.x + b_local.width > bigger_node.binary_mask.cols ||
                            b_local.y + b_local.height > bigger_node.binary_mask.rows) continue;

                        cv::Mat contain_overlap;
                        cv::bitwise_and(smaller_node.binary_mask(s_local),
                                        bigger_node.binary_mask(b_local), contain_overlap);
                        const int overlap_px = cv::countNonZero(contain_overlap);
                        const float ratio = static_cast<float>(overlap_px) /
                                            static_cast<float>(smaller_px);
                        if (ratio >= kContainThreshold) {
                            found_containment = true;
                        }
                    }
                }

                if (found_containment) {
                    merge_pairs.emplace_back(bigger_id, smaller_id);
                    marked_for_removal.insert(smaller_id);
                }
            }
        }
    }

    for (const auto& mp : merge_pairs) {
        auto keep_it = group.nodes.find(mp.first);
        auto remove_it = group.nodes.find(mp.second);
        if (keep_it == group.nodes.end() || remove_it == group.nodes.end()) continue;

        auto& keep = *keep_it->second;
        auto& removed = *remove_it->second;
        keep.created_frame = std::min(keep.created_frame, removed.created_frame);
        keep.last_seen_frame = std::max(keep.last_seen_frame, removed.last_seen_frame);
        keep.absence_score = std::max(keep.absence_score, removed.absence_score);
        keep.seen_count = std::max(1, keep.seen_count) + std::max(1, removed.seen_count);
        keep.in_view_count = std::max(keep.in_view_count, removed.in_view_count);
        keep.max_area_seen = std::max(
            std::max(keep.max_area_seen, keep.area),
            std::max(removed.max_area_seen, removed.area));

        for (int nid : removed.neighbor_ids) {
            if (nid == mp.first) continue;
            AppendUniqueId(keep.neighbor_ids, nid);
            auto nit = group.nodes.find(nid);
            if (nit != group.nodes.end()) {
                auto& nvec = nit->second->neighbor_ids;
                std::replace(nvec.begin(), nvec.end(), mp.second, mp.first);
                nvec.erase(std::remove(nvec.begin(), nvec.end(), nit->second->id), nvec.end());
                DeduplicateIds(nvec);
            }
        }
        keep.neighbor_ids.erase(
            std::remove(keep.neighbor_ids.begin(), keep.neighbor_ids.end(), mp.second),
            keep.neighbor_ids.end());
        keep.neighbor_ids.erase(
            std::remove(keep.neighbor_ids.begin(), keep.neighbor_ids.end(), mp.first),
            keep.neighbor_ids.end());
        DeduplicateIds(keep.neighbor_ids);

        group.spatial_index.Remove(mp.second, removed.centroid_canvas);
        group.nodes.erase(remove_it);
        graph_changed = true;
    }
}

// ============================================================================
//  SECTION 10: Phase Correlation Helper
// ============================================================================

cv::Mat WhiteboardCanvas::GetCanvasGrayRegion(WhiteboardGroup& group,
                                               int global_x, int global_y,
                                               int width, int height) {
    // --- Chunk pipeline path: read from chunk tiles ---
    if (GetPipelineMode() == CanvasPipelineMode::kChunk && chunk_height_ > 0) {
        int gx0 = global_x, gy0 = global_y;
        int gx1 = gx0 + width, gy1 = gy0 + height;

        int sc_x = (int)std::floor((float)gx0 / kChunkWidth);
        int sc_y = (int)std::floor((float)gy0 / chunk_height_);
        int ec_x = (int)std::floor((float)(gx1 - 1) / kChunkWidth);
        int ec_y = (int)std::floor((float)(gy1 - 1) / chunk_height_);

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
                int cpx = cx * kChunkWidth, cpy = cy * chunk_height_;
                int ix0 = std::max(cpx, gx0), iy0 = std::max(cpy, gy0);
                int ix1 = std::min(cpx + kChunkWidth, gx1);
                int iy1 = std::min(cpy + chunk_height_, gy1);
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

    // --- Graph pipeline path: compose from node masks ---
    cv::Rect viewport(global_x, global_y, width, height);

    int coverage_pixels = 0;
    int total_pixels = width * height;

    cv::Mat region(height, width, CV_8UC1, cv::Scalar(255));

    for (const auto& pair : group.nodes) {
        const auto& node = *pair.second;
        cv::Rect intersection = viewport & node.bbox_canvas;
        if (intersection.empty()) continue;

        cv::Rect src_roi(
            intersection.x - node.bbox_canvas.x,
            intersection.y - node.bbox_canvas.y,
            intersection.width, intersection.height);

        cv::Rect dst_roi(
            intersection.x - global_x,
            intersection.y - global_y,
            intersection.width, intersection.height);

        // Validate ROIs
        if (src_roi.x < 0 || src_roi.y < 0 ||
            src_roi.x + src_roi.width > node.binary_mask.cols ||
            src_roi.y + src_roi.height > node.binary_mask.rows) continue;
        if (dst_roi.x < 0 || dst_roi.y < 0 ||
            dst_roi.x + dst_roi.width > region.cols ||
            dst_roi.y + dst_roi.height > region.rows) continue;

        cv::Mat mask_roi = node.binary_mask(src_roi);
        region(dst_roi).setTo(0, mask_roi);
        coverage_pixels += cv::countNonZero(mask_roi);
    }

    // If coverage is too sparse, skip phase correlation
    if (coverage_pixels < total_pixels * 0.02) {
        return cv::Mat();
    }

    return region;
}

// ============================================================================
//  SECTION 11: Rendering from Graph Nodes
// ============================================================================

void WhiteboardCanvas::UpdateGroupBounds(WhiteboardGroup& group) {
    const auto profile_start = SteadyClock::now();
    if (group.nodes.empty()) {
        // No nodes: keep existing bounds so render doesn't collapse.
        RecordProfileSample(ProfileStep::kBoundsUpdate,
                            ElapsedMs(profile_start, SteadyClock::now()));
        return;
    }

    // Step 1: Compute tight bounds from current nodes (same as original code).
    int smin_x = INT_MAX, smin_y = INT_MAX, smax_x = INT_MIN, smax_y = INT_MIN;
    int rmin_x = INT_MAX, rmin_y = INT_MAX, rmax_x = INT_MIN, rmax_y = INT_MIN;

    for (const auto& pair : group.nodes) {
        const auto& node = *pair.second;
        const cv::Rect& bb = node.bbox_canvas;

        smin_x = std::min(smin_x, bb.x);
        smin_y = std::min(smin_y, bb.y);
        smax_x = std::max(smax_x, bb.x + bb.width);
        smax_y = std::max(smax_y, bb.y + bb.height);

        if (!node.color_pixels.empty()) {
            rmin_x = std::min(rmin_x, bb.x);
            rmin_y = std::min(rmin_y, bb.y);
            rmax_x = std::max(rmax_x, bb.x + bb.width);
            rmax_y = std::max(rmax_y, bb.y + bb.height);
        }
    }

    // Step 2: Apply tight bounds, then clamp so render only grows.
    // min values can only decrease; max values can only increase.
    if (smin_x < smax_x && smin_y < smax_y) {
        group.stroke_min_px_x = std::min(group.stroke_min_px_x, smin_x);
        group.stroke_min_px_y = std::min(group.stroke_min_px_y, smin_y);
        group.stroke_max_px_x = std::max(group.stroke_max_px_x, smax_x);
        group.stroke_max_px_y = std::max(group.stroke_max_px_y, smax_y);
    }

    if (rmin_x < rmax_x && rmin_y < rmax_y) {
        group.raw_min_px_x = std::min(group.raw_min_px_x, rmin_x);
        group.raw_min_px_y = std::min(group.raw_min_px_y, rmin_y);
        group.raw_max_px_x = std::max(group.raw_max_px_x, rmax_x);
        group.raw_max_px_y = std::max(group.raw_max_px_y, rmax_y);
    } else {
        group.raw_min_px_x = std::min(group.raw_min_px_x, group.stroke_min_px_x);
        group.raw_min_px_y = std::min(group.raw_min_px_y, group.stroke_min_px_y);
        group.raw_max_px_x = std::max(group.raw_max_px_x, group.stroke_max_px_x);
        group.raw_max_px_y = std::max(group.raw_max_px_y, group.stroke_max_px_y);
    }

    // Step 3: Enforce minimum height = first frame height.
    if (group.fixed_render_height > 0) {
        int stroke_h = group.stroke_max_px_y - group.stroke_min_px_y;
        if (stroke_h < group.fixed_render_height) {
            group.stroke_max_px_y = group.stroke_min_px_y + group.fixed_render_height;
        }
        int raw_h = group.raw_max_px_y - group.raw_min_px_y;
        if (raw_h < group.fixed_render_height) {
            group.raw_max_px_y = group.raw_min_px_y + group.fixed_render_height;
        }
    }

    RecordProfileSample(ProfileStep::kBoundsUpdate,
                        ElapsedMs(profile_start, SteadyClock::now()));
}

void WhiteboardCanvas::RebuildStrokeRenderCache(WhiteboardGroup& group) {
    const auto profile_start = SteadyClock::now();
    if (group.nodes.empty()) {
        group.stroke_render_cache = cv::Mat();
        RecordProfileSample(ProfileStep::kRenderStrokeCache,
                            ElapsedMs(profile_start, SteadyClock::now()));
        return;
    }

    int width = std::max(1, group.stroke_max_px_x - group.stroke_min_px_x);
    int height = std::max(1, group.stroke_max_px_y - group.stroke_min_px_y);

    // White canvas, strokes painted dark, then inverted
    cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    // Sort nodes by created_frame for z-order
    std::vector<const DrawingNode*> sorted_nodes;
    sorted_nodes.reserve(group.nodes.size());
    for (const auto& pair : group.nodes) {
        sorted_nodes.push_back(pair.second.get());
    }
    std::sort(sorted_nodes.begin(), sorted_nodes.end(),
              [](const DrawingNode* a, const DrawingNode* b) {
                  return a->created_frame < b->created_frame;
              });

    for (const auto* node : sorted_nodes) {
        cv::Rect dst_roi(
            node->bbox_canvas.x - group.stroke_min_px_x,
            node->bbox_canvas.y - group.stroke_min_px_y,
            node->bbox_canvas.width, node->bbox_canvas.height);

        // Clamp to canvas bounds
        int cx0 = std::max(0, dst_roi.x);
        int cy0 = std::max(0, dst_roi.y);
        int cx1 = std::min(width, dst_roi.x + dst_roi.width);
        int cy1 = std::min(height, dst_roi.y + dst_roi.height);
        if (cx0 >= cx1 || cy0 >= cy1) continue;

        int sx0 = cx0 - dst_roi.x;
        int sy0 = cy0 - dst_roi.y;
        int sw = cx1 - cx0;
        int sh = cy1 - cy0;

        if (sx0 + sw > node->binary_mask.cols || sy0 + sh > node->binary_mask.rows) continue;

        cv::Rect src_rect(sx0, sy0, sw, sh);
        cv::Rect dst_rect(cx0, cy0, sw, sh);

        // Paint dark where mask is set
        canvas(dst_rect).setTo(cv::Scalar(0, 0, 0), node->binary_mask(src_rect));
    }

    // Invert: black background with white strokes
    cv::bitwise_not(canvas, canvas);

    // Graph overlay: node bboxes, centroids, neighbor edges, FPS, counters
    if (kShowGraphOverlay) {
        DrawGraphOverlay(canvas,
                         group,
                         group.stroke_min_px_x,
                         group.stroke_min_px_y,
                         perf_stats_.last_fps,
                         perf_stats_.last_votes);
    }

    group.stroke_render_cache = canvas;
    RecordProfileSample(ProfileStep::kRenderStrokeCache,
                        ElapsedMs(profile_start, SteadyClock::now()));
}

void WhiteboardCanvas::RebuildRawRenderCache(WhiteboardGroup& group) {
    const auto profile_start = SteadyClock::now();
    if (group.nodes.empty()) {
        group.raw_render_cache = cv::Mat();
        RecordProfileSample(ProfileStep::kRenderRawCache,
                            ElapsedMs(profile_start, SteadyClock::now()));
        return;
    }

    int width = std::max(1, group.raw_max_px_x - group.raw_min_px_x);
    int height = std::max(1, group.raw_max_px_y - group.raw_min_px_y);
    cv::Mat canvas(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    // Sort nodes by created_frame for z-order
    std::vector<const DrawingNode*> sorted_nodes;
    sorted_nodes.reserve(group.nodes.size());
    for (const auto& pair : group.nodes) {
        if (pair.second->color_pixels.empty()) continue;
        sorted_nodes.push_back(pair.second.get());
    }
    std::sort(sorted_nodes.begin(), sorted_nodes.end(),
              [](const DrawingNode* a, const DrawingNode* b) {
                  return a->created_frame < b->created_frame;
              });

    for (const auto* node : sorted_nodes) {
        cv::Rect dst_roi(
            node->bbox_canvas.x - group.raw_min_px_x,
            node->bbox_canvas.y - group.raw_min_px_y,
            node->bbox_canvas.width, node->bbox_canvas.height);

        int cx0 = std::max(0, dst_roi.x);
        int cy0 = std::max(0, dst_roi.y);
        int cx1 = std::min(width, dst_roi.x + dst_roi.width);
        int cy1 = std::min(height, dst_roi.y + dst_roi.height);
        if (cx0 >= cx1 || cy0 >= cy1) continue;

        int sx0 = cx0 - dst_roi.x;
        int sy0 = cy0 - dst_roi.y;
        int sw = cx1 - cx0;
        int sh = cy1 - cy0;

        if (sx0 + sw > node->color_pixels.cols || sy0 + sh > node->color_pixels.rows) continue;
        if (sx0 + sw > node->binary_mask.cols || sy0 + sh > node->binary_mask.rows) continue;

        cv::Rect src_rect(sx0, sy0, sw, sh);
        cv::Rect dst_rect(cx0, cy0, sw, sh);

        // Paint color pixels where mask is set
        node->color_pixels(src_rect).copyTo(canvas(dst_rect), node->binary_mask(src_rect));
    }

    // Draw graph overlay: FPS, candidates, nodes, camera frame (when graph overlay is on)
    if (kShowGraphOverlay) {
        DrawGraphOverlay(canvas,
                         group,
                         group.raw_min_px_x,
                         group.raw_min_px_y,
                         perf_stats_.last_fps,
                         perf_stats_.last_votes);
    }

    group.raw_render_cache = canvas;
    RecordProfileSample(ProfileStep::kRenderRawCache,
                        ElapsedMs(profile_start, SteadyClock::now()));
}

// ============================================================================
//  SECTION 12: Create new Whiteboard Group
// ============================================================================

void WhiteboardCanvas::SeedGroupFromFrameBlobs(WhiteboardGroup& group,
                                                const std::vector<FrameBlob>& blobs,
                                                int current_frame) {
    const auto profile_start = SteadyClock::now();
    group.nodes.clear();
    group.spatial_index.Clear();
    group.next_node_id = 0;
    group.candidates.clear();
    group.next_candidate_id = 0;

    for (const auto& blob : blobs) {
        auto node = std::make_unique<DrawingNode>();
        node->id = group.next_node_id++;
        node->binary_mask = blob.binary_mask.clone();
        if (!blob.color_pixels.empty()) {
            node->color_pixels = blob.color_pixels.clone();
        }
        node->bbox_canvas = blob.bbox;
        node->centroid_canvas = blob.centroid;
        node->contour = blob.contour;
        std::copy(blob.hu, blob.hu + 7, node->hu);
        node->area = blob.area;
        node->max_area_seen = blob.area;
        node->absence_score = kAbsenceScoreInitial;
        node->last_seen_frame = current_frame;
        node->created_frame = current_frame;
        node->seen_count = 1;
        node->match_distance = 0;

        const int nid = node->id;
        group.spatial_index.Insert(nid, blob.centroid);
        group.nodes[nid] = std::move(node);
    }

    const auto neighbor_start = SteadyClock::now();
    RebuildGroupNeighborGraph(group, kKNeighbors);
    RecordProfileSample(ProfileStep::kNeighborRebuild,
                        ElapsedMs(neighbor_start, SteadyClock::now()));
    UpdateGroupBounds(group);
    group.stroke_cache_dirty = true;
    group.raw_cache_dirty = true;
    BumpCanvasVersion();
    RecordProfileSample(ProfileStep::kSeedGroup,
                        ElapsedMs(profile_start, SteadyClock::now()));
}

void WhiteboardCanvas::CreateSubCanvas(const cv::Mat& frame_bgr,
                                        const cv::Mat& binary,
                                        std::vector<FrameBlob>& blobs,
                                        int current_frame) {
    const auto profile_start = SteadyClock::now();
    auto group = std::make_unique<WhiteboardGroup>();
    group->debug_id = next_debug_id_++;

    // Lock render height to the first frame's height (only grows, never shrinks)
    group->fixed_render_height = std::max(frame_h_, frame_bgr.rows);

    // Seed at (0,0) global position
    global_camera_pos_ = cv::Point2f(0, 0);
    camera_velocity_ = cv::Point2f(0, 0);
    last_vote_count_ = 0;
    global_frame_bootstrap_consumed_ = false;
    matched_frame_counter_ = 0;

    // Initialize bounds to frame size
    group->stroke_min_px_x = 0;
    group->stroke_min_px_y = 0;
    group->stroke_max_px_x = std::max(1, frame_bgr.cols);
    group->stroke_max_px_y = std::max(1, frame_bgr.rows);
    group->raw_min_px_x = 0;
    group->raw_min_px_y = 0;
    group->raw_max_px_x = std::max(frame_w_, frame_bgr.cols);
    group->raw_max_px_y = std::max(frame_h_, frame_bgr.rows);

    // If no blobs were extracted, try extracting them now
    if (blobs.empty()) {
        blobs = ExtractFrameBlobs(binary, frame_bgr);
        EnhanceFrameBlobs(blobs, frame_bgr, g_canvas_enhance_threshold.load());
    }

    SeedGroupFromFrameBlobs(*group, blobs, current_frame);

    int idx = (int)groups_.size();
    groups_.push_back(std::move(group));
    active_group_idx_ = idx;

    if (canvas_view_mode_.load() && view_group_idx_ == -1) {
        view_group_idx_ = idx;
    }

    has_content_ = true;
    RecordProfileSample(ProfileStep::kCreateSubCanvas,
                        ElapsedMs(profile_start, SteadyClock::now()));
}

// ============================================================================
//  SECTION 14: Pipeline mode switching
// ============================================================================

void WhiteboardCanvas::SetPipelineMode(CanvasPipelineMode mode) {
    const int old_mode = pipeline_mode_.load(std::memory_order_relaxed);
    const int new_mode = static_cast<int>(mode);
    if (old_mode == new_mode) return;
    pipeline_mode_.store(new_mode, std::memory_order_relaxed);

    if (remote_process_ && helper_client_) {
        helper_client_->SetPipelineMode(new_mode);
    }

    InvalidateRenderCaches();
    const char* name = (new_mode == static_cast<int>(CanvasPipelineMode::kChunk))
                            ? "CHUNK" : "GRAPH";
    WhiteboardLog(std::string("[WhiteboardCanvas] Pipeline mode changed to ") + name);
}

CanvasPipelineMode WhiteboardCanvas::GetPipelineMode() const {
    const int m = pipeline_mode_.load(std::memory_order_relaxed);
    if (m == static_cast<int>(CanvasPipelineMode::kChunk))
        return CanvasPipelineMode::kChunk;
    return CanvasPipelineMode::kGraph;
}

// ============================================================================
//  SECTION 15: Chunk Pipeline -- Contour helpers
// ============================================================================

std::vector<WhiteboardCanvas::ContourShape>
WhiteboardCanvas::ExtractContourShapes(const cv::Mat& binary,
                                        cv::Point2f roi_offset) const {
    std::vector<ContourShape> result;
    if (binary.empty()) return result;

    std::vector<std::vector<cv::Point>> raw;
    cv::findContours(binary, raw, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

    for (const auto& c : raw) {
        const double area = cv::contourArea(c);
        if (area < ScaleArea(kMinContourArea)) continue;

        cv::Rect bbox = cv::boundingRect(c);
        if (bbox.width > 0 && bbox.height > 0) {
            float aspect_ratio = (bbox.width > bbox.height)
                               ? (float)bbox.width / (float)bbox.height
                               : (float)bbox.height / (float)bbox.width;
            if (aspect_ratio >= kChunkRectangleThreshold) continue;
        }

        cv::Moments m = cv::moments(c);
        if (std::abs(m.m00) < 1e-6) continue;
        cv::Point2f centroid((float)(m.m10 / m.m00) + roi_offset.x,
                             (float)(m.m01 / m.m00) + roi_offset.y);

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

bool WhiteboardCanvas::MatchContours(const std::vector<ContourShape>& frame_contours,
                                     const std::vector<ContourShape>& canvas_contours,
                                     cv::Point2f& out_pos, int& out_votes, int binSize) {
    if (canvas_contours.empty() || frame_contours.empty()) return false;

    std::unordered_map<int64_t, std::vector<cv::Point2f>> votes;

    for (const auto& cc : canvas_contours) {
        for (const auto& fc : frame_contours) {
            double dist = cv::matchShapes(cc.contour, fc.contour,
                                          cv::CONTOURS_MATCH_I2, 0);
            if (dist > kChunkMaxShapeDist) continue;

            float dx = cc.centroid.x - fc.centroid.x;
            float dy = cc.centroid.y - fc.centroid.y;

            int bin_x = (int)std::round(dx / (float)binSize) * binSize;
            int bin_y = (int)std::round(dy / (float)binSize) * binSize;
            int64_t key = (static_cast<int64_t>(static_cast<uint32_t>(bin_x)) << 32)
                        | static_cast<uint32_t>(bin_y);
            votes[key].push_back(cv::Point2f(dx, dy));
        }
    }

    int best_votes = 0;
    int64_t best_key = 0;
    for (const auto& vote : votes) {
        const int vote_count = (int)vote.second.size();
        if (vote_count > best_votes) {
            best_votes = vote_count;
            best_key = vote.first;
        }
    }

    if (best_votes < kChunkMinShapeVotes) return false;

    std::vector<cv::Point2f> merged;
    if (kChunkEnableNeighborBinMerge) {
        int center_bx = (int)(int32_t)(uint32_t)(best_key >> 32);
        int center_by = (int)(int32_t)(uint32_t)(best_key & 0xFFFFFFFF);
        for (int nx = -1; nx <= 1; nx++) {
            for (int ny = -1; ny <= 1; ny++) {
                int nbx = center_bx + nx * binSize;
                int nby = center_by + ny * binSize;
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
    out_pos = cv::Point2f(dxs[mid], dys[mid]);
    out_votes = best_votes;
    return true;
}

void WhiteboardCanvas::RebuildCanvasContours(WhiteboardGroup& group) {
    canvas_contours_.clear();
    canvas_contours_dirty_ = false;

    if (group.stroke_cache_dirty) {
        RebuildStrokeRenderCacheChunk(group);
        group.stroke_cache_dirty = false;
    }
    if (group.stroke_render_cache.empty()) return;

    cv::Mat gray, canvas_bin;
    cv::cvtColor(group.stroke_render_cache, gray, cv::COLOR_BGR2GRAY);
    cv::threshold(gray, canvas_bin, 50, 255, cv::THRESH_BINARY);

    cv::Point2f offset((float)group.stroke_min_px_x,
                       (float)group.stroke_min_px_y);
    canvas_contours_ = ExtractContourShapes(canvas_bin, offset);
}

// ============================================================================
//  SECTION 16: Chunk Pipeline -- Sub-pixel Refinement
// ============================================================================

bool WhiteboardCanvas::RefineWithTemplate(WhiteboardGroup& group,
                                           const cv::Mat& binary,
                                           const cv::Mat& gray,
                                           cv::Point2f& pos) {
    const int patch_size = ScalePx(kChunkTemplateMatchPatchSize);
    const int search_radius = ScalePx(kChunkTemplateMatchSearchRadius);

    cv::Rect best_patch;
    int best_density = 0;
    for (int y = 0; y <= binary.rows - patch_size; y += patch_size / 2) {
        for (int x = 0; x <= binary.cols - patch_size; x += patch_size / 2) {
            cv::Rect r(x, y, patch_size, patch_size);
            int density = cv::countNonZero(binary(r));
            if (density > best_density) {
                best_density = density;
                best_patch = r;
            }
        }
    }

    if (best_density < patch_size) return false;

    cv::Mat templ = gray(best_patch);

    int search_x = (int)std::round(pos.x) + best_patch.x - search_radius;
    int search_y = (int)std::round(pos.y) + best_patch.y - search_radius;
    int search_w = patch_size + 2 * search_radius;
    int search_h = patch_size + 2 * search_radius;

    cv::Mat canvas_roi = GetCanvasGrayRegion(group, search_x, search_y,
                                             search_w, search_h);
    if (canvas_roi.empty() || canvas_roi.cols < templ.cols || canvas_roi.rows < templ.rows) {
        return false;
    }

    cv::Mat result;
    cv::matchTemplate(canvas_roi, templ, result, cv::TM_CCOEFF_NORMED);
    if (result.empty()) return false;

    double minVal, maxVal;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);

    if (maxVal < 0.5) return false;

    float sub_x = (float)maxLoc.x;
    float sub_y = (float)maxLoc.y;

    if (maxLoc.x > 0 && maxLoc.x < result.cols - 1) {
        float left  = result.at<float>(maxLoc.y, maxLoc.x - 1);
        float center = result.at<float>(maxLoc.y, maxLoc.x);
        float right = result.at<float>(maxLoc.y, maxLoc.x + 1);
        float denom = 2.0f * (2.0f * center - left - right);
        if (std::abs(denom) > 1e-6f) {
            sub_x += (left - right) / denom;
        }
    }
    if (maxLoc.y > 0 && maxLoc.y < result.rows - 1) {
        float top    = result.at<float>(maxLoc.y - 1, maxLoc.x);
        float center = result.at<float>(maxLoc.y, maxLoc.x);
        float bottom = result.at<float>(maxLoc.y + 1, maxLoc.x);
        float denom = 2.0f * (2.0f * center - top - bottom);
        if (std::abs(denom) > 1e-6f) {
            sub_y += (top - bottom) / denom;
        }
    }

    float dx = sub_x - (float)search_radius;
    float dy = sub_y - (float)search_radius;

    if (std::abs(dx) < kChunkTemplateMaxCorrection && std::abs(dy) < kChunkTemplateMaxCorrection) {
        pos.x += dx;
        pos.y += dy;
        return true;
    }
    return false;
}

// ============================================================================
//  SECTION 17: Chunk Grid Management
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
        chunk->stroke_canvas = cv::Mat(chunk_height_, kChunkWidth, CV_8UC3, cv::Scalar(255, 255, 255));
        chunk->absence_counter = cv::Mat(chunk_height_, kChunkWidth, CV_8U, cv::Scalar(0));
        chunk->raw_canvas = cv::Mat(chunk_height_, kChunkWidth, CV_8UC3, cv::Scalar(255, 255, 255));
        chunk->grid_x = grid_x;
        chunk->grid_y = grid_y;
        group.chunks[hash] = std::move(chunk);
        group.stroke_cache_dirty = true;
        group.raw_cache_dirty = true;
        BumpCanvasVersion();
    }
}

// ============================================================================
//  SECTION 18: Chunk Pipeline -- Painting to Chunks
// ============================================================================

void WhiteboardCanvas::PaintStrokesToChunks(WhiteboardGroup& group, const cv::Mat& binary,
                                             const cv::Mat& enhanced_bgr,
                                             cv::Point2f camera_pos,
                                             const cv::Mat& no_update_mask) {
    cv::Rect bbox = cv::boundingRect(binary);
    if (bbox.empty()) return;

    int global_start_x = (int)std::round(camera_pos.x) + bbox.x;
    int global_start_y = (int)std::round(camera_pos.y) + bbox.y;
    int global_end_x   = global_start_x + bbox.width;
    int global_end_y   = global_start_y + bbox.height;

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

    int start_chunk_x = (int)std::floor((float)global_start_x / kChunkWidth);
    int start_chunk_y = (int)std::floor((float)global_start_y / chunk_height_);
    int end_chunk_x   = (int)std::floor((float)global_end_x   / kChunkWidth);
    int end_chunk_y   = (int)std::floor((float)global_end_y   / chunk_height_);

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++)
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++)
            EnsureChunkAllocated(group, cx, cy);

    cv::Mat footprint(paint_bbox.height, paint_bbox.width, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::Mat absence_foot(paint_bbox.height, paint_bbox.width, CV_8U, cv::Scalar(0));

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            int chunk_px_x = cx * kChunkWidth;
            int chunk_px_y = cy * chunk_height_;
            int ix0 = std::max(chunk_px_x, global_start_x);
            int iy0 = std::max(chunk_px_y, global_start_y);
            int ix1 = std::min(chunk_px_x + kChunkWidth, global_end_x);
            int iy1 = std::min(chunk_px_y + chunk_height_, global_end_y);
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

    cv::Mat foot_gray, existing;
    cv::cvtColor(footprint, foot_gray, cv::COLOR_BGR2GRAY);
    cv::threshold(foot_gray, existing, 200, 255, cv::THRESH_BINARY_INV);

    cv::Mat prox_allow_mask(footprint.size(), CV_8U, cv::Scalar(255));
    if (kEnableProximitySuppression) {
        cv::Mat canvas_stroke_zone;
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE,
            cv::Size(2 * ScalePx(kProximityRadius) + 1, 2 * ScalePx(kProximityRadius) + 1));
        cv::dilate(existing, canvas_stroke_zone, kernel);
        cv::bitwise_not(canvas_stroke_zone, prox_allow_mask);
    }

    cv::Mat update_allowed_mask;
    cv::bitwise_not(no_update_bbox, update_allowed_mask);

    cv::Mat cell_replace_mask(footprint.size(), CV_8U, cv::Scalar(0));
    cv::Mat cell_erase_mask(footprint.size(), CV_8U, cv::Scalar(0));
    cv::Mat ghost_block(footprint.size(), CV_8U, cv::Scalar(0));

    if (kEnableGridReplace || kEnableGhostBlock || kEnableAbsenceErasure) {
        const int grid_cell_size = ScalePx(kGridCellSize);
        for (int gy = 0; gy < footprint.rows; gy += grid_cell_size) {
            for (int gx = 0; gx < footprint.cols; gx += grid_cell_size) {
                int cw = std::min(grid_cell_size, footprint.cols - gx);
                int ch = std::min(grid_cell_size, footprint.rows - gy);
                cv::Rect cell(gx, gy, cw, ch);

                if (cv::countNonZero(no_update_bbox(cell)) > 0) continue;

                int new_count   = cv::countNonZero(new_strokes(cell));
                int exist_count = cv::countNonZero(existing(cell));

                if (kEnableAbsenceErasure
                    && exist_count >= ScalePx(kMinCellStrokePixels) && new_count < kAbsenceEraseThr) {
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

                if (exist_count < ScalePx(kMinCellStrokePixels) || new_count == 0) continue;

                cv::Mat inter;
                cv::bitwise_and(new_strokes(cell), existing(cell), inter);
                int inter_count = cv::countNonZero(inter);
                int union_count = new_count + exist_count - inter_count;
                float iou = (union_count > 0) ? (float)inter_count / (float)union_count : 0.0f;
                float overlap = (float)inter_count / (float)new_count;

                if (kEnableGridReplace && iou < kCellReplaceIoU) {
                    cell_replace_mask(cell).setTo(255);
                    prox_allow_mask(cell).setTo(255);
                } else if (kEnableGhostBlock && overlap > kCellGhostOverlap) {
                    ghost_block(cell).setTo(255);
                }
            }
        }
    }

    if (kEnableGhostBlock) {
        cv::Mat ghost_allow;
        cv::bitwise_not(ghost_block, ghost_allow);
        cv::bitwise_and(prox_allow_mask, ghost_allow, prox_allow_mask);
    }
    cv::bitwise_and(prox_allow_mask, update_allowed_mask, prox_allow_mask);

    cv::Mat new_only;
    cv::bitwise_and(new_strokes, prox_allow_mask, new_only);

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            int chunk_px_x = cx * kChunkWidth;
            int chunk_px_y = cy * chunk_height_;
            int ix0 = std::max(chunk_px_x, global_start_x);
            int iy0 = std::max(chunk_px_y, global_start_y);
            int ix1 = std::min(chunk_px_x + kChunkWidth, global_end_x);
            int iy1 = std::min(chunk_px_y + chunk_height_, global_end_y);
            if (ix0 >= ix1 || iy0 >= iy1) continue;

            cv::Rect chunk_roi(ix0 - chunk_px_x, iy0 - chunk_px_y, ix1 - ix0, iy1 - iy0);
            cv::Rect foot_roi (ix0 - global_start_x, iy0 - global_start_y, ix1 - ix0, iy1 - iy0);

            uint64_t hash = GetChunkHash(cx, cy);
            auto& chunk = group.chunks[hash];
            bool chunk_dirty = false;

            absence_foot(foot_roi).copyTo(chunk->absence_counter(chunk_roi));

            cv::Mat erase_roi = cell_erase_mask(foot_roi) | cell_replace_mask(foot_roi);
            if (cv::countNonZero(erase_roi) > 0) {
                chunk->stroke_canvas(chunk_roi).setTo(cv::Scalar(255, 255, 255), erase_roi);
                chunk_dirty = true;
            }

            cv::Mat sub_mask = new_only(foot_roi);
            if (cv::countNonZero(sub_mask) > 0) {
                colors_bbox(foot_roi).copyTo(chunk->stroke_canvas(chunk_roi), sub_mask);
                chunk_dirty = true;
            }

            if (chunk_dirty) {
                group.stroke_cache_dirty = true;
                canvas_contours_dirty_ = true;
                BumpCanvasVersion();
            }
        }
    }
}

void WhiteboardCanvas::PaintRawFrameToChunks(WhiteboardGroup& group,
                                             const cv::Mat& frame_bgr,
                                             cv::Point2f camera_pos,
                                             const cv::Mat& no_update_mask) {
    if (frame_bgr.empty()) return;

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
        if (variance < kBlurThreshold) return;
    }

    const int margin = ScalePx(kRawEdgeMargin);
    const int fw = frame_bgr.cols, fh = frame_bgr.rows;
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

    cv::Mat inner_frame = frame_bgr(inner);
    cv::Mat inner_no_update;
    if (!no_update_mask.empty() && no_update_mask.size() == frame_bgr.size()) {
        inner_no_update = no_update_mask(inner);
    }

    int start_chunk_x = (int)std::floor((float)global_start_x / kChunkWidth);
    int start_chunk_y = (int)std::floor((float)global_start_y / chunk_height_);
    int end_chunk_x   = (int)std::floor((float)global_end_x   / kChunkWidth);
    int end_chunk_y   = (int)std::floor((float)global_end_y   / chunk_height_);

    for (int cy = start_chunk_y; cy <= end_chunk_y; cy++) {
        for (int cx = start_chunk_x; cx <= end_chunk_x; cx++) {
            EnsureChunkAllocated(group, cx, cy);

            int chunk_px_x = cx * kChunkWidth;
            int chunk_px_y = cy * chunk_height_;
            int ix0 = std::max(chunk_px_x, global_start_x);
            int iy0 = std::max(chunk_px_y, global_start_y);
            int ix1 = std::min(chunk_px_x + kChunkWidth, global_end_x);
            int iy1 = std::min(chunk_px_y + chunk_height_, global_end_y);
            if (ix0 >= ix1 || iy0 >= iy1) continue;

            cv::Rect chunk_roi(ix0 - chunk_px_x, iy0 - chunk_px_y, ix1 - ix0, iy1 - iy0);
            cv::Rect src_roi(ix0 - global_start_x, iy0 - global_start_y, ix1 - ix0, iy1 - iy0);

            cv::Mat valid_mask;
            if (!inner_no_update.empty()) {
                cv::bitwise_not(inner_no_update(src_roi), valid_mask);
            } else {
                valid_mask = cv::Mat(src_roi.height, src_roi.width, CV_8U, cv::Scalar(255));
            }
            if (cv::countNonZero(valid_mask) == 0) continue;

            uint64_t hash = GetChunkHash(cx, cy);
            auto& chunk = group.chunks[hash];
            inner_frame(src_roi).copyTo(chunk->raw_canvas(chunk_roi), valid_mask);
            group.raw_cache_dirty = true;
            BumpCanvasVersion();
        }
    }
}

// ============================================================================
//  SECTION 19: Chunk Pipeline -- Render Caches
// ============================================================================

void WhiteboardCanvas::RebuildStrokeRenderCacheChunk(WhiteboardGroup& group) {
    if (group.chunks.empty()) {
        group.stroke_render_cache = cv::Mat();
        return;
    }

    int width = std::max(1, group.stroke_max_px_x - group.stroke_min_px_x);
    int height = std::max(1, group.stroke_max_px_y - group.stroke_min_px_y);
    group.stroke_render_cache = cv::Mat(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    for (const auto& pair : group.chunks) {
        const auto& chunk = pair.second;
        int chunk_left = chunk->grid_x * kChunkWidth;
        int chunk_top = chunk->grid_y * chunk_height_;
        int chunk_right = chunk_left + kChunkWidth;
        int chunk_bottom = chunk_top + chunk_height_;

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

    cv::bitwise_not(group.stroke_render_cache, group.stroke_render_cache);
}

void WhiteboardCanvas::RebuildRawRenderCacheChunk(WhiteboardGroup& group) {
    if (group.chunks.empty()) {
        group.raw_render_cache = cv::Mat();
        return;
    }

    int width = std::max(1, group.raw_max_px_x - group.raw_min_px_x);
    int height = std::max(1, group.raw_max_px_y - group.raw_min_px_y);
    group.raw_render_cache = cv::Mat(height, width, CV_8UC3, cv::Scalar(255, 255, 255));

    for (const auto& pair : group.chunks) {
        const auto& chunk = pair.second;
        int chunk_left = chunk->grid_x * kChunkWidth;
        int chunk_top = chunk->grid_y * chunk_height_;
        int chunk_right = chunk_left + kChunkWidth;
        int chunk_bottom = chunk_top + chunk_height_;

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
//  SECTION 20: Chunk Pipeline -- CreateSubCanvas
// ============================================================================

void WhiteboardCanvas::CreateSubCanvasChunk(const cv::Mat& frame_bgr,
                                             const cv::Mat& binary,
                                             const cv::Mat& enhanced_bgr,
                                             const std::vector<ContourShape>& seed_contours) {
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

    global_camera_pos_ = cv::Point2f(0, 0);

    int idx = (int)groups_.size();
    groups_.push_back(std::move(group));
    active_group_idx_ = idx;

    if (canvas_view_mode_.load() && view_group_idx_ == -1) {
        view_group_idx_ = idx;
    }

    has_content_ = true;
    canvas_contours_ = seed_contours;
    canvas_contours_dirty_ = false;
}

// ============================================================================
//  SECTION 21: Chunk Pipeline -- ProcessFrameChunk
// ============================================================================

void WhiteboardCanvas::ProcessFrameChunk(const cv::Mat& frame, const cv::Mat& gray,
                                          const cv::Mat& person_mask, float motion_fraction) {
    // --- Person mask ---
    bool has_person_mask = !person_mask.empty() && person_mask.size() == gray.size();
    if (!has_person_mask) {
        WhiteboardLog("[ChunkPipeline] No person mask -- skipping frame");
        return;
    }

    cv::Mat person_mask_dilated;
    {
        cv::Mat small_mask;
        cv::resize(person_mask, small_mask, cv::Size(), 0.25, 0.25, cv::INTER_NEAREST);
        int pad = std::max(1, (int)(std::max(small_mask.cols, small_mask.rows) * 0.05));
        pad |= 1;
        cv::Mat k_dilate = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(pad, pad));
        cv::dilate(small_mask, small_mask, k_dilate);
        cv::resize(small_mask, person_mask_dilated, frame.size(), 0, 0, cv::INTER_NEAREST);
    }

    cv::Rect person_bbox = cv::boundingRect(person_mask_dilated);
    struct FrameStrip { int x; int width; };
    std::vector<FrameStrip> strips;
    static constexpr int kMinStripWidth = 512;
    int left_width = person_bbox.x;
    if (left_width >= kMinStripWidth) strips.push_back({0, left_width});
    int right_x = person_bbox.x + person_bbox.width;
    int right_width = frame.cols - right_x;
    if (right_width >= kMinStripWidth) strips.push_back({right_x, right_width});
    if (strips.empty()) return;

    // --- Binarize ---
    cv::Mat binary;
    cv::adaptiveThreshold(gray, binary, 255,
                          cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, 51, 5);

    // Remove salt-and-pepper noise: discard tiny connected components
    {
        cv::Mat labels, stats, centroids;
        int n = cv::connectedComponentsWithStats(binary, labels, stats, centroids);
        for (int i = 1; i < n; i++) {
            if (stats.at<int>(i, cv::CC_STAT_AREA) < 100) {
                binary.setTo(0, labels == i);
            }
        }
    }

    if (has_person_mask && !person_mask_dilated.empty()) {
        binary.setTo(0, person_mask_dilated);
    }
    const int top_cut    = (int)(gray.rows * 0.15);
    const int bottom_cut = (int)(gray.rows * 0.90);
    if (top_cut > 0) binary(cv::Rect(0, 0, binary.cols, top_cut)).setTo(0);
    if (bottom_cut < binary.rows)
        binary(cv::Rect(0, bottom_cut, binary.cols, binary.rows - bottom_cut)).setTo(0);

    int stroke_pixel_count = cv::countNonZero(binary);
    cv::Mat stroke_paint_bgr(frame.size(), CV_8UC3, cv::Scalar(0, 0, 0));

    // --- Contour matching ---
    std::vector<ContourShape> frame_contours =
        ExtractContourShapes(binary, cv::Point2f(0, 0));

    std::lock_guard<std::mutex> state_lock(state_mutex_);

    if (frame_w_ == 0) { frame_w_ = frame.cols; frame_h_ = frame.rows; chunk_height_ = frame_h_; }

    int         best_group_idx = -1;
    int         best_votes = 0;
    cv::Point2f matched_pos(0, 0);
    std::string match_status;
    bool created_new_sc = false;
    bool tier_matched = false;
    int final_bin_size = 10;

    if (canvas_contours_dirty_ && active_group_idx_ >= 0
        && active_group_idx_ < (int)groups_.size()) {
        RebuildCanvasContours(*groups_[active_group_idx_]);
    }

    if (active_group_idx_ >= 0 && active_group_idx_ < (int)groups_.size()
        && !canvas_contours_.empty() && !frame_contours.empty()) {

        int bin_sizes[] = {2, 5, 10};
        for (int bin_size : bin_sizes) {
            int tier_votes = 0;
            cv::Point2f tier_pos;
            if (MatchContours(frame_contours, canvas_contours_, tier_pos, tier_votes, bin_size)) {
                if (tier_votes > kChunkMinShapeVotes && bin_size > 2) continue;
                best_votes = tier_votes;
                matched_pos = tier_pos;
                final_bin_size = bin_size;
                tier_matched = true;
                break;
            }
        }

        if (tier_matched) {
            best_group_idx = active_group_idx_;
            last_match_accuracy_ = (float)best_votes;
        }
    }

    // --- Paint or create ---
    if (best_group_idx >= 0) {
        auto& group = *groups_[best_group_idx];

        if (kChunkEnableTemplateRefinement) {
            RefineWithTemplate(group, binary, gray, matched_pos);
        }

        if (kChunkEnableJumpRejection && matched_frame_counter_ > 0) {
            float jump = (float)cv::norm(matched_pos - global_camera_pos_);
            const float max_jump_px = (float)ScalePx((int)kChunkMaxJumpPx);
            if (jump > max_jump_px) {
                matched_pos = global_camera_pos_;
            }
        }

        global_camera_pos_ = matched_pos;
        consecutive_failed_matches_ = 0;

        for (const auto& strip : strips) {
            cv::Rect strip_roi(strip.x, 0, strip.width, frame.rows);
            cv::Mat strip_binary = binary(strip_roi);
            cv::Mat strip_paint  = stroke_paint_bgr(strip_roi);
            cv::Mat strip_frame  = frame(strip_roi);
            cv::Point2f strip_pos(global_camera_pos_.x + strip.x, global_camera_pos_.y);
            PaintStrokesToChunks(group, strip_binary, strip_paint, strip_pos);
            PaintRawFrameToChunks(group, strip_frame, strip_pos);
        }

        active_group_idx_ = best_group_idx;
        has_content_ = true;
        matched_frame_counter_++;
        match_status = "CHUNK MATCHED GR" + std::to_string(best_group_idx)
                     + " votes=" + std::to_string(best_votes)
                     + " chunks=" + std::to_string(group.chunks.size());

    } else if (groups_.empty() && stroke_pixel_count >= ScaleArea(kMinStrokePixelsForNewSC)) {
        CreateSubCanvasChunk(frame, binary, stroke_paint_bgr, frame_contours);
        {
            auto& new_group = *groups_.back();
            for (const auto& strip : strips) {
                cv::Rect strip_roi(strip.x, 0, strip.width, frame.rows);
                cv::Mat strip_binary = binary(strip_roi);
                cv::Mat strip_paint  = stroke_paint_bgr(strip_roi);
                cv::Mat strip_frame  = frame(strip_roi);
                cv::Point2f strip_pos(global_camera_pos_.x + strip.x, global_camera_pos_.y);
                PaintStrokesToChunks(new_group, strip_binary, strip_paint, strip_pos);
                PaintRawFrameToChunks(new_group, strip_frame, strip_pos);
            }
        }
        created_new_sc = true;
        match_status = "CHUNK NEW GROUP (total=" + std::to_string(groups_.size()) + ")";

    } else {
        consecutive_failed_matches_++;
        match_status = "CHUNK no match (strokes=" + std::to_string(stroke_pixel_count)
                     + " contours=" + std::to_string(frame_contours.size())
                     + " votes=" + std::to_string(best_votes) + ")";
    }

    WhiteboardLog("[ChunkPipeline] " + match_status);
}

// ============================================================================
//  SECTION 22: FFI exports
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

    const auto profile_start = SteadyClock::now();
    const bool copied = CopyBgrFrameToRgbaBuffer(overview_bgr, buffer, width, height);
    g_whiteboard_canvas->RecordRgbaCopyProfile(
        ElapsedMs(profile_start, SteadyClock::now()));
    return copied;
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

    const auto profile_start = SteadyClock::now();
    const bool copied = CopyBgrFrameToRgbaBuffer(viewport_bgr, buffer, width, height);
    g_whiteboard_canvas->RecordRgbaCopyProfile(
        ElapsedMs(profile_start, SteadyClock::now()));
    return copied;
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


void SetCanvasPipelineMode(int mode) {
    if (g_whiteboard_canvas) {
        CanvasPipelineMode pm = CanvasPipelineMode::kGraph;
        if (mode == static_cast<int>(CanvasPipelineMode::kChunk))
            pm = CanvasPipelineMode::kChunk;
        g_whiteboard_canvas->SetPipelineMode(pm);
    }
}

int GetCanvasPipelineMode() {
    if (!g_whiteboard_canvas) return static_cast<int>(CanvasPipelineMode::kGraph);
    return static_cast<int>(g_whiteboard_canvas->GetPipelineMode());
}

// ============================================================================
//  Graph Debug — WhiteboardCanvas methods
// ============================================================================

int WhiteboardCanvas::GetGraphNodeCount() const {
    if (remote_process_ && helper_client_) {
        return helper_client_->GetGraphNodeCount();
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;
    return (int)groups_[gi]->nodes.size();
}

int WhiteboardCanvas::GetGraphNodes(float* buffer, int max_nodes) const {
    if (!buffer || max_nodes <= 0) return 0;
    if (remote_process_ && helper_client_) {
        return helper_client_->GetGraphNodes(buffer, max_nodes);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;

    return CopyGraphNodesToBuffer(*groups_[gi], buffer, max_nodes);
}

int WhiteboardCanvas::GetGraphNodeNeighbors(int node_id, int* neighbors, int max_neighbors) const {
    if (!neighbors || max_neighbors <= 0) return 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_; // fallback for remote-process mode
    if (gi < 0 || gi >= (int)groups_.size()) return 0;

    const auto& group = *groups_[gi];
    auto it = group.nodes.find(node_id);
    if (it == group.nodes.end()) return 0;

    const auto& ids = it->second->neighbor_ids;
    int count = std::min((int)ids.size(), max_neighbors);
    for (int i = 0; i < count; i++) {
        neighbors[i] = ids[i];
    }
    return count;
}

bool WhiteboardCanvas::CompareGraphNodes(int id_a, int id_b, float* result) const {
    if (!result) return false;
    if (remote_process_ && helper_client_) {
        return helper_client_->CompareGraphNodes(id_a, id_b, result);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_; // fallback for remote-process mode
    if (gi < 0 || gi >= (int)groups_.size()) return false;

    const auto& group = *groups_[gi];
    auto itA = group.nodes.find(id_a);
    auto itB = group.nodes.find(id_b);
    if (itA == group.nodes.end() || itB == group.nodes.end()) return false;

    const DrawingNode& a = *itA->second;
    const DrawingNode& b = *itB->second;

    // [0] shape_distance (matchShapes I2)
    double shape_dist = 1e9;
    if (!a.contour.empty() && !b.contour.empty()) {
        shape_dist = cv::matchShapes(a.contour, b.contour, cv::CONTOURS_MATCH_I2, 0);
    }
    result[0] = (float)shape_dist;

    // [1] centroid_distance
    cv::Point2f diff = a.centroid_canvas - b.centroid_canvas;
    result[1] = std::sqrt(diff.x * diff.x + diff.y * diff.y);

    // [2] bbox_intersection_area
    cv::Rect intersection = a.bbox_canvas & b.bbox_canvas;
    result[2] = (float)intersection.area();

    // [3] and_overlap_pixel_count, [4] mask_overlap_ratio
    float and_overlap = 0.0f;
    float overlap_ratio = 0.0f;
    if (intersection.area() > 0) {
        // Compute the overlap region in each node's local mask coordinates
        cv::Rect a_local(intersection.x - a.bbox_canvas.x,
                         intersection.y - a.bbox_canvas.y,
                         intersection.width, intersection.height);
        cv::Rect b_local(intersection.x - b.bbox_canvas.x,
                         intersection.y - b.bbox_canvas.y,
                         intersection.width, intersection.height);

        // Clamp to mask bounds
        a_local &= cv::Rect(0, 0, a.binary_mask.cols, a.binary_mask.rows);
        b_local &= cv::Rect(0, 0, b.binary_mask.cols, b.binary_mask.rows);

        if (a_local.area() > 0 && b_local.area() > 0 &&
            a_local.size() == b_local.size()) {
            cv::Mat a_roi = a.binary_mask(a_local);
            cv::Mat b_roi = b.binary_mask(b_local);
            cv::Mat and_mask;
            cv::bitwise_and(a_roi, b_roi, and_mask);
            and_overlap = (float)cv::countNonZero(and_mask);

            float min_area = (float)std::min(cv::countNonZero(a.binary_mask),
                                              cv::countNonZero(b.binary_mask));
            if (min_area > 0) {
                overlap_ratio = and_overlap / min_area;
            }
        }
    }
    result[3] = and_overlap;
    result[4] = overlap_ratio;

    return true;
}

bool WhiteboardCanvas::MoveGraphNode(int node_id, float new_cx, float new_cy) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_; // fallback for remote-process mode
    if (gi < 0 || gi >= (int)groups_.size()) return false;

    auto& group = *groups_[gi];
    auto it = group.nodes.find(node_id);
    if (it == group.nodes.end()) return false;

    DrawingNode& node = *it->second;
    float dx = new_cx - node.centroid_canvas.x;
    float dy = new_cy - node.centroid_canvas.y;

    // Remove from spatial index at old position
    group.spatial_index.Remove(node.id, node.centroid_canvas);

    // Update position
    node.centroid_canvas.x = new_cx;
    node.centroid_canvas.y = new_cy;
    node.bbox_canvas.x += (int)std::round(dx);
    node.bbox_canvas.y += (int)std::round(dy);

    // Re-insert into spatial index
    group.spatial_index.Insert(node.id, node.centroid_canvas);

    // Mark caches dirty
    group.stroke_cache_dirty = true;
    group.raw_cache_dirty = true;
    BumpCanvasVersion();

    return true;
}

bool WhiteboardCanvas::GetGraphCanvasBounds(int* bounds) const {
    if (!bounds) return false;
    if (remote_process_ && helper_client_) {
        return helper_client_->GetGraphCanvasBounds(bounds);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return false;

    return CopyGraphBoundsToBuffer(*groups_[gi], bounds);
}

int WhiteboardCanvas::GetGraphNodeContours(float* buffer, int max_floats) const {
    if (!buffer || max_floats <= 0) return 0;
    if (remote_process_ && helper_client_) {
        return helper_client_->GetGraphNodeContours(buffer, max_floats);
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    int gi = canvas_view_mode_.load() ? view_group_idx_ : active_group_idx_;
    if (gi < 0) gi = active_group_idx_;
    if (gi < 0 || gi >= (int)groups_.size()) return 0;

    return CopyGraphContoursToBuffer(*groups_[gi], buffer, max_floats);
}

bool WhiteboardCanvas::CaptureGraphDebugSnapshot(int slot,
                                                 const cv::Mat& frame,
                                                 const cv::Mat& person_mask) {
    if (slot < 0 || slot >= kGraphDebugCompareSnapshotCount) return false;
    if (frame.empty() || person_mask.empty()) return false;
    if (person_mask.size() != frame.size() || person_mask.type() != CV_8UC1) {
        return false;
    }

    const cv::Rect roi = ComputeProcessingRoi(frame.size());
    if (roi.width <= 0 || roi.height <= 0) {
        return false;
    }

    cv::Mat frame_roi = frame(roi);
    cv::Mat cropped_person_mask = CropPersonMaskForProcessing(person_mask, roi);

    cv::Mat gray;
    cv::cvtColor(frame_roi, gray, cv::COLOR_BGR2GRAY);
    const cv::Rect lecturer_frame_rect = ComputeMaskBoundingRect(cropped_person_mask);
    cv::Mat stroke_reject_mask;
    if (kEnableFrameStrokeRejectFilter) {
        stroke_reject_mask = BuildFrameStrokeRejectMask(
            frame_roi.size(), lecturer_frame_rect);
    }

    int stroke_pixel_count = 0;
    cv::Mat no_update_mask = BuildNoUpdateMask(gray, cropped_person_mask);
    cv::Mat binary = BuildBinaryMask(gray, no_update_mask, stroke_pixel_count);
    std::vector<FrameBlob> blobs = ExtractFrameBlobs(binary, frame_roi);
    EnhanceFrameBlobs(blobs, frame_roi, g_canvas_enhance_threshold.load());
    if (kEnableFrameStrokeRejectFilter) {
        FilterFrameBlobsForCanvas(blobs, stroke_reject_mask, kKNeighbors);
    }

    auto snapshot = std::make_unique<WhiteboardGroup>();
    snapshot->debug_id = slot;
    snapshot->fixed_render_height = std::max(1, frame_roi.rows);
    snapshot->stroke_min_px_x = 0;
    snapshot->stroke_min_px_y = 0;
    snapshot->stroke_max_px_x = std::max(1, frame_roi.cols);
    snapshot->stroke_max_px_y = std::max(1, frame_roi.rows);
    snapshot->raw_min_px_x = 0;
    snapshot->raw_min_px_y = 0;
    snapshot->raw_max_px_x = std::max(1, frame_roi.cols);
    snapshot->raw_max_px_y = std::max(1, frame_roi.rows);

    if (!blobs.empty()) {
        SeedGroupFromFrameBlobs(*snapshot, blobs, ++graph_debug_snapshot_frame_id_);
    }

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    graph_debug_snapshots_[slot] = std::move(snapshot);
    graph_debug_snapshots_[kGraphDebugResultSnapshotSlot].reset();
    return true;
}

int WhiteboardCanvas::GetGraphSnapshotNodeCount(int slot) const {
    if (slot < 0 || slot >= kGraphDebugSnapshotCount) return 0;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& snapshot = graph_debug_snapshots_[slot];
    return snapshot ? static_cast<int>(snapshot->nodes.size()) : 0;
}

int WhiteboardCanvas::GetGraphSnapshotNodes(int slot,
                                            float* buffer,
                                            int max_nodes) const {
    if (slot < 0 || slot >= kGraphDebugSnapshotCount) return 0;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& snapshot = graph_debug_snapshots_[slot];
    if (!snapshot) return 0;
    return CopyGraphNodesToBuffer(*snapshot, buffer, max_nodes);
}

bool WhiteboardCanvas::GetGraphSnapshotCanvasBounds(int slot, int* bounds) const {
    if (slot < 0 || slot >= kGraphDebugSnapshotCount || !bounds) return false;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& snapshot = graph_debug_snapshots_[slot];
    if (!snapshot) return false;
    return CopyGraphBoundsToBuffer(*snapshot, bounds);
}

int WhiteboardCanvas::GetGraphSnapshotNodeContours(int slot,
                                                   float* buffer,
                                                   int max_floats) const {
    if (slot < 0 || slot >= kGraphDebugSnapshotCount) return 0;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& snapshot = graph_debug_snapshots_[slot];
    if (!snapshot) return 0;
    return CopyGraphContoursToBuffer(*snapshot, buffer, max_floats);
}

bool WhiteboardCanvas::CompareGraphSnapshotNodes(int slot_a,
                                                 int id_a,
                                                 int slot_b,
                                                 int id_b,
                                                 float* result) const {
    if (!result) return false;
    if (slot_a < 0 || slot_a >= kGraphDebugSnapshotCount) return false;
    if (slot_b < 0 || slot_b >= kGraphDebugSnapshotCount) return false;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& snapshot_a = graph_debug_snapshots_[slot_a];
    const auto& snapshot_b = graph_debug_snapshots_[slot_b];
    if (!snapshot_a || !snapshot_b) return false;

    auto it_a = snapshot_a->nodes.find(id_a);
    auto it_b = snapshot_b->nodes.find(id_b);
    if (it_a == snapshot_a->nodes.end() || it_b == snapshot_b->nodes.end()) {
        return false;
    }

    return FillSnapshotNodeComparisonResult(*it_a->second, *it_b->second, result);
}

bool WhiteboardCanvas::CombineGraphDebugSnapshots(int slot_a,
                                                  int anchor_id_a,
                                                  int slot_b,
                                                  int anchor_id_b) {
    if (slot_a < 0 || slot_a >= kGraphDebugCompareSnapshotCount) return false;
    if (slot_b < 0 || slot_b >= kGraphDebugCompareSnapshotCount) return false;
    if (slot_a == slot_b) return false;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& snapshot_a = graph_debug_snapshots_[slot_a];
    const auto& snapshot_b = graph_debug_snapshots_[slot_b];
    if (!snapshot_a || !snapshot_b) return false;
    if (snapshot_a->nodes.empty() || snapshot_b->nodes.empty()) return false;

    // Build source features from snapshot_b nodes (source = right)
    std::vector<int> source_node_ids;
    source_node_ids.reserve(snapshot_b->nodes.size());
    for (const auto& pair : snapshot_b->nodes) {
        source_node_ids.push_back(pair.first);
    }

    const int N = static_cast<int>(source_node_ids.size());
    cv::Mat source_features(N, 7, CV_32F);
    std::vector<cv::Point2f> source_centroids(N);
    std::vector<cv::Rect> source_bboxes(N);
    std::vector<int> source_ids(N);

    for (int i = 0; i < N; i++) {
        const auto& node = *snapshot_b->nodes.at(source_node_ids[i]);
        auto log_hu = ComputeLogHuFeatures(node.hu);
        for (int j = 0; j < 7; j++) {
            source_features.at<float>(i, j) = log_hu[j];
        }
        source_centroids[i] = node.centroid_canvas;
        source_bboxes[i] = node.bbox_canvas;
        source_ids[i] = node.id;
    }

    // Build target features from snapshot_a nodes (target = left)
    std::vector<int> target_node_ids;
    target_node_ids.reserve(snapshot_a->nodes.size());
    for (const auto& pair : snapshot_a->nodes) {
        target_node_ids.push_back(pair.first);
    }

    const int M = static_cast<int>(target_node_ids.size());
    cv::Mat target_features(M, 7, CV_32F);
    std::vector<cv::Point2f> target_centroids(M);
    std::vector<cv::Rect> target_bboxes(M);
    std::vector<int> target_ids(M);

    for (int i = 0; i < M; i++) {
        const auto& node = *snapshot_a->nodes.at(target_node_ids[i]);
        auto log_hu = ComputeLogHuFeatures(node.hu);
        for (int j = 0; j < 7; j++) {
            target_features.at<float>(i, j) = log_hu[j];
        }
        target_centroids[i] = node.centroid_canvas;
        target_bboxes[i] = node.bbox_canvas;
        target_ids[i] = node.id;
    }

    // Compute prior offset: preferred anchor pair or bounds center fallback
    cv::Point2f prior_offset =
        GetGroupBoundsCenter(*snapshot_a) - GetGroupBoundsCenter(*snapshot_b);
    auto preferred_anchor_a = snapshot_a->nodes.find(anchor_id_a);
    auto preferred_anchor_b = snapshot_b->nodes.find(anchor_id_b);
    if (preferred_anchor_a != snapshot_a->nodes.end() &&
        preferred_anchor_b != snapshot_b->nodes.end()) {
        prior_offset = preferred_anchor_a->second->centroid_canvas -
                       preferred_anchor_b->second->centroid_canvas;
    }

    // Run KD-Tree + RANSAC matching (source=B, target=A → offset is right_to_left)
    GraphMatchResult match = MatchWithKdTreeRansac(
        source_features, source_centroids, source_bboxes, source_ids,
        target_features, target_centroids, target_bboxes, target_ids,
        kKdTreeHuDistanceThreshold,
        kKdTreeMinBboxSimilarity,
        kRansacInlierTolerancePx,
        kRansacMaxIterations,
        kMinRansacInliers,
        kKdTreeKnnNeighbors,
        prior_offset);

    cv::Point2f best_merge_offset = match.valid ? match.offset : prior_offset;

    auto result = CloneGraphGroup(*snapshot_a);
    result->debug_id = kGraphDebugResultSnapshotSlot;
    result->fixed_render_height = std::max(
        snapshot_a->fixed_render_height,
        snapshot_b->fixed_render_height);
    result->last_lecturer_rect = cv::Rect();

    const int current_frame = ++graph_debug_snapshot_frame_id_;
    for (const auto& pair : snapshot_b->nodes) {
        AppendShiftedNodeToGroup(
            *result,
            *pair.second,
            best_merge_offset,
            result->next_node_id++,
            current_frame);
    }

    bool graph_changed = false;
    MergeOverlappingNodes(*result, current_frame, graph_changed);
    RebuildGroupNeighborGraph(*result, kKNeighbors);
    UpdateGroupBounds(*result);
    result->stroke_cache_dirty = true;
    result->raw_cache_dirty = true;

    graph_debug_snapshots_[kGraphDebugResultSnapshotSlot] = std::move(result);
    return true;
}

bool WhiteboardCanvas::CopyGraphDebugSnapshot(int source_slot, int target_slot) {
    if (source_slot < 0 || source_slot >= kGraphDebugSnapshotCount) return false;
    if (target_slot < 0 || target_slot >= kGraphDebugCompareSnapshotCount) return false;

    std::lock_guard<std::mutex> lock(graph_snapshot_mutex_);
    const auto& source = graph_debug_snapshots_[source_slot];
    if (!source) return false;

    auto clone = CloneGraphGroup(*source);
    clone->debug_id = target_slot;
    graph_debug_snapshots_[target_slot] = std::move(clone);
    return true;
}

// ============================================================================
//  Graph Debug FFI exports
// ============================================================================

int GetGraphNodeCount() {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphNodeCount();
}

int GetGraphNodes(float* buffer, int max_nodes) {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphNodes(buffer, max_nodes);
}

int GetGraphNodeNeighbors(int node_id, int* neighbors, int max_neighbors) {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphNodeNeighbors(node_id, neighbors, max_neighbors);
}

bool CompareGraphNodes(int id_a, int id_b, float* result) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->CompareGraphNodes(id_a, id_b, result);
}

bool MoveGraphNode(int node_id, float new_cx, float new_cy) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->MoveGraphNode(node_id, new_cx, new_cy);
}

bool GetGraphCanvasBounds(int* bounds) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->GetGraphCanvasBounds(bounds);
}

int GetGraphNodeContours(float* buffer, int max_floats) {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphNodeContours(buffer, max_floats);
}

bool CaptureGraphDebugSnapshot(int slot) {
    if (!g_whiteboard_canvas || !g_native_camera) return false;

    cv::Mat frame;
    cv::Mat person_mask;
    if (!g_native_camera->CopyLatestWhiteboardInput(frame, person_mask)) {
        return false;
    }

    return g_whiteboard_canvas->CaptureGraphDebugSnapshot(slot, frame, person_mask);
}

int GetGraphSnapshotNodeCount(int slot) {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphSnapshotNodeCount(slot);
}

int GetGraphSnapshotNodes(int slot, float* buffer, int max_nodes) {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphSnapshotNodes(slot, buffer, max_nodes);
}

bool GetGraphSnapshotCanvasBounds(int slot, int* bounds) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->GetGraphSnapshotCanvasBounds(slot, bounds);
}

int GetGraphSnapshotNodeContours(int slot, float* buffer, int max_floats) {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetGraphSnapshotNodeContours(slot, buffer, max_floats);
}

bool CompareGraphSnapshotNodes(int slot_a,
                               int id_a,
                               int slot_b,
                               int id_b,
                               float* result) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->CompareGraphSnapshotNodes(
        slot_a, id_a, slot_b, id_b, result);
}

bool CombineGraphDebugSnapshots(int slot_a,
                                int anchor_id_a,
                                int slot_b,
                                int anchor_id_b) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->CombineGraphDebugSnapshots(
        slot_a, anchor_id_a, slot_b, anchor_id_b);
}

bool CopyGraphDebugSnapshot(int source_slot, int target_slot) {
    if (!g_whiteboard_canvas) return false;
    return g_whiteboard_canvas->CopyGraphDebugSnapshot(source_slot, target_slot);
}

uint64_t GetCanvasVersion() {
    if (!g_whiteboard_canvas) return 0;
    return g_whiteboard_canvas->GetCanvasVersion();
}

bool GetCanvasFullResRgba(uint8_t* buffer, int max_w, int max_h,
                          int* out_w, int* out_h) {
    if (!g_whiteboard_canvas || !buffer || max_w <= 0 || max_h <= 0 ||
        !out_w || !out_h) {
        return false;
    }

    const cv::Size canvas_size = g_whiteboard_canvas->GetCanvasSize();
    if (canvas_size.width <= 0 || canvas_size.height <= 0) return false;

    // Scale to fit within max_w x max_h, preserving aspect ratio (no hard cap)
    const double scale = std::min(
        (double)max_w / canvas_size.width,
        (double)max_h / canvas_size.height);
    const int scaled_w = std::max(1, (int)std::round(canvas_size.width * scale));
    const int scaled_h = std::max(1, (int)std::round(canvas_size.height * scale));

    cv::Mat overview_bgr;
    if (!g_whiteboard_canvas->GetOverviewBlocking(
            cv::Size(scaled_w, scaled_h), overview_bgr)) {
        return false;
    }

    // The overview might be letterboxed to the requested size; use actual dims
    const int actual_w = overview_bgr.cols;
    const int actual_h = overview_bgr.rows;
    if (actual_w > max_w || actual_h > max_h) return false;

    *out_w = actual_w;
    *out_h = actual_h;

    const auto profile_start = SteadyClock::now();
    const bool copied = CopyBgrFrameToRgbaBuffer(overview_bgr, buffer,
                                                  actual_w, actual_h);
    g_whiteboard_canvas->RecordRgbaCopyProfile(
        ElapsedMs(profile_start, SteadyClock::now()));
    return copied;
}

