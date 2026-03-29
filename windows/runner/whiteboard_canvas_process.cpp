#include "whiteboard_canvas_process.h"

#include "native_camera.h"
#include "whiteboard_canvas.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

namespace {

constexpr uint32_t kSharedMagic = 0x57424950;  // 'WBIP'
constexpr int kMaxFrameWidth = 3840;
constexpr int kMaxFrameHeight = 2160;
constexpr size_t kMaxFrameBytes =
    static_cast<size_t>(kMaxFrameWidth) * static_cast<size_t>(kMaxFrameHeight) * 3;
constexpr size_t kMaxMaskBytes =
    static_cast<size_t>(kMaxFrameWidth) * static_cast<size_t>(kMaxFrameHeight);
constexpr int kMaxOverviewWidth = 4096;
constexpr int kMaxOverviewHeight = 4096;
constexpr size_t kMaxOverviewBytes =
    static_cast<size_t>(kMaxOverviewWidth) * static_cast<size_t>(kMaxOverviewHeight) * 3;
constexpr DWORD kFrameSubmitLockTimeoutMs = 8;
constexpr DWORD kImageReadLockTimeoutMs = 25;
constexpr DWORD kStateReadLockTimeoutMs = 8;
constexpr DWORD kHelperStartTimeoutMs = 5000;
constexpr DWORD kHelperLoopWaitMs = 15;
constexpr int kNoSubCanvasRequest = -1;
constexpr int kDefaultCanvasWidth = 1920;
constexpr int kDefaultCanvasHeight = 1080;
constexpr int kMaxGraphNodes = 512;
constexpr int kGraphNodeStride = 16;
constexpr int kMaxGraphNodeFloats = kMaxGraphNodes * kGraphNodeStride;
constexpr int kMaxGraphContourFloats = 500000;
constexpr DWORD kGraphCompareTimeoutMs = 2000;
constexpr int kMaxEditDeletes = 256;
constexpr int kMaxEditMoves = 256;
constexpr DWORD kEditCommandTimeoutMs = 2000;
constexpr int kMaxMaskDataBytes = 20 * 1024 * 1024;  // 20 MB for node RGBA masks
constexpr DWORD kMaskRequestTimeoutMs = 3000;
bool g_is_helper_process = false;
std::string g_helper_session_id;

#pragma pack(push, 1)
struct SharedState {
    uint32_t magic = kSharedMagic;
    LONG shutdown = 0;
    LONG helper_alive = 0;
    LONG whiteboard_enabled = 0;
    LONG canvas_view_mode = 0;
    LONG render_mode = static_cast<LONG>(CanvasRenderMode::kStroke);
    LONG whiteboard_debug = 0;
    LONG reset_requested = 0;
    LONG pending_active_subcanvas = kNoSubCanvasRequest;
    float pan_x = 0.5f;
    float pan_y = 0.5f;
    float zoom = 1.0f;
    float enhance_threshold = 5.0f;
    float yolo_fps = 2.0f;
    LONG viewport_req_width = 0;
    LONG viewport_req_height = 0;
    LONG overview_req_width = 0;
    LONG overview_req_height = 0;
    LONG has_content = 0;
    LONG canvas_width = kDefaultCanvasWidth;
    LONG canvas_height = kDefaultCanvasHeight;
    LONG subcanvas_count = 0;
    LONG active_subcanvas = -1;
    LONG frame_available = 0;
    LONG frame_width = 0;
    LONG frame_height = 0;
    LONG mask_width = 0;
    LONG mask_height = 0;
    LONG viewport_width = 0;
    LONG viewport_height = 0;
    LONG overview_width = 0;
    LONG overview_height = 0;
    LONG graph_node_count = 0;
    LONG graph_node_floats_written = 0;
    LONG graph_contour_floats_written = 0;
    LONG graph_bounds_x = 0;
    LONG graph_bounds_y = 0;
    LONG graph_bounds_w = 0;
    LONG graph_bounds_h = 0;
    LONG graph_bounds_valid = 0;
    LONG graph_compare_request_id = 0;
    LONG graph_compare_node_a = -1;
    LONG graph_compare_node_b = -1;
    LONG graph_compare_result_ready = 0;
    LONG graph_compare_result_ok = 0;
    LONG graph_compare_result_id = 0;
    unsigned char frame_bgr[kMaxFrameBytes];
    unsigned char person_mask[kMaxMaskBytes];
    unsigned char viewport_bgr[kMaxFrameBytes];
    unsigned char overview_bgr[kMaxOverviewBytes];
    float graph_nodes[kMaxGraphNodeFloats];
    float graph_contours[kMaxGraphContourFloats];
    float graph_compare_result[10];

    // User edit commands (client -> helper)
    LONG edit_request_id = 0;
    LONG edit_lock_all = 0;          // 1 = lock all nodes
    LONG edit_delete_count = 0;
    LONG edit_move_count = 0;
    int  edit_delete_ids[kMaxEditDeletes];
    float edit_moves[kMaxEditMoves * 3]; // [id, cx, cy] triples
    LONG edit_result_ready = 0;
    LONG edit_result_ok = 0;
    LONG edit_result_id = 0;

    // Mask data request/response (client -> helper -> client)
    LONG mask_request_id = 0;
    LONG mask_result_ready = 0;
    LONG mask_result_id = 0;
    LONG mask_result_bytes = 0;
    unsigned char mask_data[kMaxMaskDataBytes];
};
#pragma pack(pop)

struct HelperStateSnapshot {
    bool shutdown = false;
    bool enabled = false;
    bool canvas_view_mode = false;
    CanvasRenderMode render_mode = CanvasRenderMode::kStroke;
    bool debug_enabled = false;
    bool reset_requested = false;
    int requested_active_subcanvas = kNoSubCanvasRequest;
    float pan_x = 0.5f;
    float pan_y = 0.5f;
    float zoom = 1.0f;
    float enhance_threshold = 5.0f;
    float yolo_fps = 2.0f;
    cv::Size viewport_size;
    cv::Size overview_size;
    cv::Mat frame;
    cv::Mat person_mask;
    bool has_new_frame = false;
    int graph_compare_request_id = 0;
    int graph_compare_node_a = -1;
    int graph_compare_node_b = -1;
    int edit_request_id = 0;
    bool edit_lock_all = false;
    int edit_delete_count = 0;
    int edit_move_count = 0;
    int edit_delete_ids[kMaxEditDeletes] = {};
    float edit_moves[kMaxEditMoves * 3] = {};
    int mask_request_id = 0;
};

std::wstring Utf16FromUtf8(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, wide.data(), required);
    wide.resize(static_cast<size_t>(required - 1));
    return wide;
}

std::wstring MakeObjectName(const std::wstring& prefix, const std::wstring& session_id) {
    return prefix + session_id;
}

bool IsValidFrameSize(int width, int height) {
    return width > 0 && height > 0 && width <= kMaxFrameWidth && height <= kMaxFrameHeight;
}

bool IsValidOverviewSize(int width, int height) {
    return width > 0 && height > 0 && width <= kMaxOverviewWidth && height <= kMaxOverviewHeight;
}

size_t FrameBytesForSize(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
}

size_t MaskBytesForSize(int width, int height) {
    return static_cast<size_t>(width) * static_cast<size_t>(height);
}

bool WaitAndLock(HANDLE mutex_handle, DWORD timeout_ms) {
    if (!mutex_handle) return false;
    const DWORD wait_result = WaitForSingleObject(mutex_handle, timeout_ms);
    return wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED;
}

void Unlock(HANDLE mutex_handle) {
    if (mutex_handle) {
        ReleaseMutex(mutex_handle);
    }
}

class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return handle_; }
    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }
    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class WhiteboardHelperServer {
public:
    explicit WhiteboardHelperServer(std::string session_id)
        : session_id_utf8_(std::move(session_id)),
          session_id_utf16_(Utf16FromUtf8(session_id_utf8_)) {}

    int Run() {
        if (!OpenSharedObjects()) {
            return EXIT_FAILURE;
        }

        WhiteboardCanvas canvas;
        cv::Mat last_viewport;
        cv::Mat last_overview;
        cv::Size latest_output_size(kDefaultCanvasWidth, kDefaultCanvasHeight);
        int last_graph_compare_request_id = 0;
        int graph_compare_result_id = 0;
        bool graph_compare_result_ready = false;
        bool graph_compare_result_ok = false;
        float graph_compare_result[10] = {};

        int last_edit_request_id = 0;
        int last_mask_request_id = 0;
        int edit_result_id = 0;
        bool edit_result_ready = false;
        bool edit_result_ok = false;

        while (true) {
            WaitForSingleObject(wake_event_.get(), kHelperLoopWaitMs);

            HelperStateSnapshot snapshot;
            if (!ReadSnapshot(snapshot)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (snapshot.shutdown) {
                break;
            }

            g_whiteboard_debug.store(snapshot.debug_enabled);
            g_canvas_enhance_threshold.store(snapshot.enhance_threshold);
            g_yolo_fps.store(snapshot.yolo_fps);

            canvas.SetCanvasViewMode(snapshot.canvas_view_mode);
            canvas.SetRenderMode(snapshot.render_mode);

            if (snapshot.reset_requested) {
                canvas.Reset();
                last_viewport.release();
                last_overview.release();
            }
            if (snapshot.requested_active_subcanvas >= 0) {
                canvas.SetActiveSubCanvas(snapshot.requested_active_subcanvas);
            }

            if (snapshot.has_new_frame && !snapshot.frame.empty()) {
                latest_output_size = snapshot.frame.size();
                cv::Mat person_mask = snapshot.person_mask;
                if (person_mask.empty() || person_mask.size() != snapshot.frame.size() ||
                    person_mask.type() != CV_8UC1) {
                    person_mask = GetWhiteboardPersonMask(snapshot.frame);
                }
                canvas.ProcessFrame(snapshot.frame, person_mask);
            }

            const bool has_content = canvas.HasContent();
            if (has_content) {
                cv::Size viewport_size = snapshot.viewport_size;
                if (!IsValidFrameSize(viewport_size.width, viewport_size.height)) {
                    viewport_size = latest_output_size;
                }

                if (IsValidFrameSize(viewport_size.width, viewport_size.height)) {
                    cv::Mat viewport;
                    if (canvas.GetViewport(snapshot.pan_x, snapshot.pan_y, snapshot.zoom,
                                           viewport_size, viewport)) {
                        last_viewport = viewport;
                    }
                }

                cv::Size overview_size = snapshot.overview_size;
                if (!IsValidOverviewSize(overview_size.width, overview_size.height)) {
                    overview_size = latest_output_size;
                }
                if (overview_size.width > kMaxOverviewWidth || overview_size.height > kMaxOverviewHeight) {
                    const float scale = std::min(
                        static_cast<float>(kMaxOverviewWidth) / overview_size.width,
                        static_cast<float>(kMaxOverviewHeight) / overview_size.height);
                    overview_size.width = static_cast<int>(overview_size.width * scale);
                    overview_size.height = static_cast<int>(overview_size.height * scale);
                }

                if (IsValidOverviewSize(overview_size.width, overview_size.height)) {
                    cv::Mat overview;
                    if (canvas.GetOverviewBlocking(overview_size, overview)) {
                        last_overview = overview;
                    }
                }
            } else {
                last_viewport.release();
                last_overview.release();
            }

            if (snapshot.graph_compare_request_id > 0 &&
                snapshot.graph_compare_request_id != last_graph_compare_request_id) {
                std::fill(std::begin(graph_compare_result), std::end(graph_compare_result), 0.0f);
                graph_compare_result_ok = canvas.CompareGraphNodes(
                    snapshot.graph_compare_node_a,
                    snapshot.graph_compare_node_b,
                    graph_compare_result);
                graph_compare_result_id = snapshot.graph_compare_request_id;
                graph_compare_result_ready = true;
                last_graph_compare_request_id = snapshot.graph_compare_request_id;
            }

            // Process user edit commands
            if (snapshot.edit_request_id > 0 &&
                snapshot.edit_request_id != last_edit_request_id) {
                bool edit_ok = false;
                if (snapshot.edit_lock_all) {
                    canvas.LockAllGraphNodes();
                }
                if (snapshot.edit_delete_count > 0 || snapshot.edit_move_count > 0) {
                    edit_ok = canvas.ApplyUserEdits(
                        snapshot.edit_delete_ids, snapshot.edit_delete_count,
                        snapshot.edit_moves, snapshot.edit_move_count);
                } else if (snapshot.edit_lock_all) {
                    edit_ok = true;  // lock-only request
                }
                edit_result_id = snapshot.edit_request_id;
                edit_result_ok = edit_ok;
                edit_result_ready = true;
                last_edit_request_id = snapshot.edit_request_id;
            }

            // Process mask data request
            int mask_bytes_written = 0;
            bool mask_result_ready_flag = false;
            int mask_result_id_val = 0;
            if (snapshot.mask_request_id > 0 &&
                snapshot.mask_request_id != last_mask_request_id) {
                // Use a heap buffer to avoid stack overflow
                static thread_local std::vector<uint8_t> local_mask_buf(kMaxMaskDataBytes);
                mask_bytes_written = canvas.GetGraphNodeMasks(
                    local_mask_buf.data(), kMaxMaskDataBytes);
                mask_result_id_val = snapshot.mask_request_id;
                mask_result_ready_flag = true;
                last_mask_request_id = snapshot.mask_request_id;

                // Write mask data directly under shared lock
                if (WaitAndLock(mutex_.get(), 50)) {
                    if (mask_bytes_written > 0) {
                        std::memcpy(shared_->mask_data, local_mask_buf.data(),
                                    mask_bytes_written);
                    }
                    shared_->mask_result_bytes = mask_bytes_written;
                    shared_->mask_result_id = mask_result_id_val;
                    shared_->mask_result_ready = 1;
                    Unlock(mutex_.get());
                }
            }

            WriteResults(canvas,
                         last_viewport,
                         last_overview,
                         graph_compare_result_ready,
                         graph_compare_result_id,
                         graph_compare_result_ok,
                         graph_compare_result,
                         edit_result_ready,
                         edit_result_id,
                         edit_result_ok);
        }

        return EXIT_SUCCESS;
    }

private:
    bool OpenSharedObjects() {
        const auto mapping_name = MakeObjectName(L"Local\\KaptchiWhiteboardMap_", session_id_utf16_);
        const auto mutex_name = MakeObjectName(L"Local\\KaptchiWhiteboardMutex_", session_id_utf16_);
        const auto event_name = MakeObjectName(L"Local\\KaptchiWhiteboardEvent_", session_id_utf16_);

        mapping_.reset(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str()));
        if (!mapping_.get()) {
            std::cerr << "[WhiteboardCanvas] Failed to open helper mapping" << std::endl;
            return false;
        }

        mutex_.reset(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutex_name.c_str()));
        if (!mutex_.get()) {
            std::cerr << "[WhiteboardCanvas] Failed to open helper mutex" << std::endl;
            return false;
        }

        wake_event_.reset(OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, event_name.c_str()));
        if (!wake_event_.get()) {
            std::cerr << "[WhiteboardCanvas] Failed to open helper event" << std::endl;
            return false;
        }

        shared_ = static_cast<SharedState*>(
            MapViewOfFile(mapping_.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
        if (!shared_) {
            std::cerr << "[WhiteboardCanvas] Failed to map helper shared memory" << std::endl;
            return false;
        }

        if (!WaitAndLock(mutex_.get(), 1000)) {
            std::cerr << "[WhiteboardCanvas] Failed to lock helper shared memory" << std::endl;
            return false;
        }
        shared_->magic = kSharedMagic;
        shared_->helper_alive = 1;
        Unlock(mutex_.get());
        return true;
    }

    bool ReadSnapshot(HelperStateSnapshot& snapshot) {
        if (!shared_) return false;
        if (!WaitAndLock(mutex_.get(), 50)) return false;

        snapshot.shutdown = shared_->shutdown != 0;
        snapshot.enabled = shared_->whiteboard_enabled != 0;
        snapshot.canvas_view_mode = shared_->canvas_view_mode != 0;
        snapshot.render_mode = shared_->render_mode == static_cast<LONG>(CanvasRenderMode::kRaw)
            ? CanvasRenderMode::kRaw
            : CanvasRenderMode::kStroke;
        snapshot.debug_enabled = shared_->whiteboard_debug != 0;
        snapshot.reset_requested = shared_->reset_requested != 0;
        snapshot.requested_active_subcanvas = shared_->pending_active_subcanvas;
        snapshot.pan_x = shared_->pan_x;
        snapshot.pan_y = shared_->pan_y;
        snapshot.zoom = shared_->zoom;
        snapshot.enhance_threshold = shared_->enhance_threshold;
        snapshot.yolo_fps = shared_->yolo_fps;
        snapshot.viewport_size = cv::Size(shared_->viewport_req_width, shared_->viewport_req_height);
        snapshot.overview_size = cv::Size(shared_->overview_req_width, shared_->overview_req_height);
        snapshot.graph_compare_request_id = static_cast<int>(shared_->graph_compare_request_id);
        snapshot.graph_compare_node_a = static_cast<int>(shared_->graph_compare_node_a);
        snapshot.graph_compare_node_b = static_cast<int>(shared_->graph_compare_node_b);

        shared_->reset_requested = 0;
        shared_->pending_active_subcanvas = kNoSubCanvasRequest;
        shared_->helper_alive = 1;

        if (shared_->frame_available != 0 && IsValidFrameSize(shared_->frame_width, shared_->frame_height)) {
            const int frame_width = shared_->frame_width;
            const int frame_height = shared_->frame_height;
            const size_t frame_bytes = FrameBytesForSize(frame_width, frame_height);
            snapshot.frame = cv::Mat(frame_height, frame_width, CV_8UC3);
            std::memcpy(snapshot.frame.data, shared_->frame_bgr, frame_bytes);
            snapshot.has_new_frame = true;

            if (shared_->mask_width == frame_width && shared_->mask_height == frame_height) {
                const size_t mask_bytes = MaskBytesForSize(frame_width, frame_height);
                snapshot.person_mask = cv::Mat(frame_height, frame_width, CV_8UC1);
                std::memcpy(snapshot.person_mask.data, shared_->person_mask, mask_bytes);
            }
            shared_->frame_available = 0;
            shared_->frame_width = 0;
            shared_->frame_height = 0;
            shared_->mask_width = 0;
            shared_->mask_height = 0;
        }

        // Read edit commands
        snapshot.edit_request_id = static_cast<int>(shared_->edit_request_id);
        snapshot.edit_lock_all = shared_->edit_lock_all != 0;
        snapshot.edit_delete_count = std::min(static_cast<int>(shared_->edit_delete_count), kMaxEditDeletes);
        snapshot.edit_move_count = std::min(static_cast<int>(shared_->edit_move_count), kMaxEditMoves);
        if (snapshot.edit_delete_count > 0) {
            std::memcpy(snapshot.edit_delete_ids, shared_->edit_delete_ids,
                        snapshot.edit_delete_count * sizeof(int));
        }
        if (snapshot.edit_move_count > 0) {
            std::memcpy(snapshot.edit_moves, shared_->edit_moves,
                        snapshot.edit_move_count * 3 * sizeof(float));
        }

        // Read mask request
        snapshot.mask_request_id = static_cast<int>(shared_->mask_request_id);

        Unlock(mutex_.get());
        return true;
    }

    void WriteResults(WhiteboardCanvas& canvas,
                      const cv::Mat& viewport,
                      const cv::Mat& overview,
                      bool graph_compare_result_ready,
                      int graph_compare_result_id,
                      bool graph_compare_result_ok,
                      const float* graph_compare_result,
                      bool edit_result_ready,
                      int edit_result_id,
                      bool edit_result_ok) {
        if (!shared_) return;

        // Read canvas state BEFORE acquiring the shared mutex.
        // GetCanvasSize/GetSubCanvasCount/GetActiveSubCanvasIndex each lock
        // state_mutex_ internally, which the worker thread may hold for
        // hundreds of ms.  Doing this outside the shared mutex prevents the
        // client from timing out on every read attempt.
        const bool has_content = canvas.HasContent();
        const cv::Size canvas_size = has_content ? canvas.GetCanvasSize() : cv::Size(0, 0);
        const int subcanvas_count = has_content ? canvas.GetSubCanvasCount() : 0;
        const int active_subcanvas = has_content ? canvas.GetActiveSubCanvasIndex() : -1;

        // Pre-read graph debug data outside the shared mutex
        int graph_node_count = 0;
        int graph_node_floats = 0;
        int graph_contour_floats = 0;
        int graph_bounds[4] = {0, 0, 0, 0};
        bool graph_bounds_valid = false;
        float local_graph_nodes[kMaxGraphNodeFloats];
        // Use a heap buffer for contours (too large for stack)
        static thread_local std::vector<float> local_graph_contours(kMaxGraphContourFloats);

        if (has_content) {
            graph_node_count = canvas.GetGraphNodeCount();
            if (graph_node_count > 0) {
                graph_node_floats = canvas.GetGraphNodes(local_graph_nodes, kMaxGraphNodes) * kGraphNodeStride;
                graph_contour_floats = canvas.GetGraphNodeContours(
                    local_graph_contours.data(), kMaxGraphContourFloats);
                graph_bounds_valid = canvas.GetGraphCanvasBounds(graph_bounds);
            }
        }

        if (!WaitAndLock(mutex_.get(), 50)) return;

        shared_->helper_alive = 1;
        shared_->has_content = has_content ? 1 : 0;
        shared_->canvas_width = canvas_size.width;
        shared_->canvas_height = canvas_size.height;
        shared_->subcanvas_count = subcanvas_count;
        shared_->active_subcanvas = active_subcanvas;
        shared_->graph_compare_result_ready = graph_compare_result_ready ? 1 : 0;
        shared_->graph_compare_result_ok = graph_compare_result_ok ? 1 : 0;
        shared_->graph_compare_result_id = graph_compare_result_id;
        if (graph_compare_result != nullptr) {
            std::memcpy(shared_->graph_compare_result,
                        graph_compare_result,
                        sizeof(shared_->graph_compare_result));
        }

        if (!has_content) {
            shared_->viewport_width = 0;
            shared_->viewport_height = 0;
            shared_->overview_width = 0;
            shared_->overview_height = 0;
            shared_->graph_node_count = 0;
            shared_->graph_node_floats_written = 0;
            shared_->graph_contour_floats_written = 0;
            shared_->graph_bounds_valid = 0;
            Unlock(mutex_.get());
            return;
        }

        if (!viewport.empty() && IsValidFrameSize(viewport.cols, viewport.rows)) {
            const size_t viewport_bytes = FrameBytesForSize(viewport.cols, viewport.rows);
            std::memcpy(shared_->viewport_bgr, viewport.data, viewport_bytes);
            shared_->viewport_width = viewport.cols;
            shared_->viewport_height = viewport.rows;
        } else {
            shared_->viewport_width = 0;
            shared_->viewport_height = 0;
        }

        if (!overview.empty() && IsValidOverviewSize(overview.cols, overview.rows)) {
            const size_t overview_bytes = FrameBytesForSize(overview.cols, overview.rows);
            std::memcpy(shared_->overview_bgr, overview.data, overview_bytes);
            shared_->overview_width = overview.cols;
            shared_->overview_height = overview.rows;
        } else {
            shared_->overview_width = 0;
            shared_->overview_height = 0;
        }

        // Write graph debug data
        shared_->graph_node_count = graph_node_count;
        shared_->graph_node_floats_written = graph_node_floats;
        shared_->graph_contour_floats_written = graph_contour_floats;
        if (graph_node_floats > 0) {
            std::memcpy(shared_->graph_nodes, local_graph_nodes,
                        static_cast<size_t>(graph_node_floats) * sizeof(float));
        }
        if (graph_contour_floats > 0) {
            std::memcpy(shared_->graph_contours, local_graph_contours.data(),
                        static_cast<size_t>(graph_contour_floats) * sizeof(float));
        }
        shared_->graph_bounds_valid = graph_bounds_valid ? 1 : 0;
        if (graph_bounds_valid) {
            shared_->graph_bounds_x = graph_bounds[0];
            shared_->graph_bounds_y = graph_bounds[1];
            shared_->graph_bounds_w = graph_bounds[2];
            shared_->graph_bounds_h = graph_bounds[3];
        }

        // Write edit command results
        if (edit_result_ready) {
            shared_->edit_result_ready = 1;
            shared_->edit_result_ok = edit_result_ok ? 1 : 0;
            shared_->edit_result_id = edit_result_id;
        }

        Unlock(mutex_.get());
    }

    std::string session_id_utf8_;
    std::wstring session_id_utf16_;
    ScopedHandle mapping_;
    ScopedHandle mutex_;
    ScopedHandle wake_event_;
    SharedState* shared_ = nullptr;
};

}  // namespace

struct WhiteboardCanvasHelperClient::Impl {
    std::wstring session_id;
    ScopedHandle mapping;
    ScopedHandle mutex;
    ScopedHandle wake_event;
    ScopedHandle process;
    ScopedHandle thread;
    SharedState* shared = nullptr;
    bool ready = false;
    std::atomic<bool> cached_has_content{false};
    std::atomic<bool> cached_canvas_view_mode{false};
    std::atomic<int> cached_render_mode{static_cast<int>(CanvasRenderMode::kStroke)};
    std::atomic<int> cached_canvas_width{kDefaultCanvasWidth};
    std::atomic<int> cached_canvas_height{kDefaultCanvasHeight};
    std::atomic<int> cached_subcanvas_count{0};
    std::atomic<int> cached_active_subcanvas{-1};
    mutable std::atomic<int> next_graph_compare_request_id{1};
    mutable std::atomic<int> next_edit_request_id{1};
    mutable std::atomic<int> next_mask_request_id{1};

    ~Impl() {
        if (shared) {
            UnmapViewOfFile(shared);
            shared = nullptr;
        }
    }

    void RefreshCachedStateUnsafe() {
        if (!shared) return;
        cached_has_content.store(shared->has_content != 0, std::memory_order_relaxed);
        cached_canvas_view_mode.store(shared->canvas_view_mode != 0,
                                      std::memory_order_relaxed);
        cached_render_mode.store(static_cast<int>(shared->render_mode),
                                 std::memory_order_relaxed);
        cached_canvas_width.store(std::max(1L, shared->canvas_width),
                                  std::memory_order_relaxed);
        cached_canvas_height.store(std::max(1L, shared->canvas_height),
                                   std::memory_order_relaxed);
        cached_subcanvas_count.store(static_cast<int>(shared->subcanvas_count),
                                     std::memory_order_relaxed);
        cached_active_subcanvas.store(static_cast<int>(shared->active_subcanvas),
                                      std::memory_order_relaxed);
    }

    void ResetCachedState() {
        cached_has_content.store(false, std::memory_order_relaxed);
        cached_canvas_view_mode.store(false, std::memory_order_relaxed);
        cached_render_mode.store(static_cast<int>(CanvasRenderMode::kStroke),
                                 std::memory_order_relaxed);
        cached_canvas_width.store(kDefaultCanvasWidth, std::memory_order_relaxed);
        cached_canvas_height.store(kDefaultCanvasHeight, std::memory_order_relaxed);
        cached_subcanvas_count.store(0, std::memory_order_relaxed);
        cached_active_subcanvas.store(-1, std::memory_order_relaxed);
    }

    bool WithLock(DWORD timeout_ms, const std::function<void()>& fn) const {
        if (!WaitAndLock(mutex.get(), timeout_ms)) {
            return false;
        }
        fn();
        Unlock(mutex.get());
        return true;
    }

    void SignalHelper() const {
        if (wake_event.get()) {
            SetEvent(wake_event.get());
        }
    }
};

WhiteboardCanvasHelperClient::WhiteboardCanvasHelperClient()
    : impl_(std::make_unique<Impl>()) {
    if (impl_) {
        impl_->ResetCachedState();
    }
}

WhiteboardCanvasHelperClient::~WhiteboardCanvasHelperClient() {
    Stop();
}

bool WhiteboardCanvasHelperClient::Start() {
    if (!impl_ || impl_->ready) return impl_ && impl_->ready;

    std::wstringstream session_builder;
    session_builder << GetCurrentProcessId() << L"_" << GetTickCount64();
    impl_->session_id = session_builder.str();

    const auto mapping_name = MakeObjectName(L"Local\\KaptchiWhiteboardMap_", impl_->session_id);
    const auto mutex_name = MakeObjectName(L"Local\\KaptchiWhiteboardMutex_", impl_->session_id);
    const auto event_name = MakeObjectName(L"Local\\KaptchiWhiteboardEvent_", impl_->session_id);

    impl_->mapping.reset(CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedState)), mapping_name.c_str()));
    if (!impl_->mapping.get()) {
        return false;
    }

    impl_->mutex.reset(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    impl_->wake_event.reset(CreateEventW(nullptr, FALSE, FALSE, event_name.c_str()));
    if (!impl_->mutex.get() || !impl_->wake_event.get()) {
        Stop();
        return false;
    }

    impl_->shared = static_cast<SharedState*>(
        MapViewOfFile(impl_->mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!impl_->shared) {
        Stop();
        return false;
    }

    std::memset(impl_->shared, 0, sizeof(SharedState));
    impl_->shared->magic = kSharedMagic;
    impl_->shared->render_mode = static_cast<LONG>(CanvasRenderMode::kStroke);
    impl_->shared->pending_active_subcanvas = kNoSubCanvasRequest;
    impl_->shared->pan_x = 0.5f;
    impl_->shared->pan_y = 0.5f;
    impl_->shared->zoom = 1.0f;
    impl_->shared->enhance_threshold = 5.0f;
    impl_->shared->yolo_fps = 2.0f;
    impl_->shared->canvas_width = kDefaultCanvasWidth;
    impl_->shared->canvas_height = kDefaultCanvasHeight;
    impl_->ResetCachedState();

    wchar_t exe_path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        Stop();
        return false;
    }

    std::wstring command_line = L"\"";
    command_line += exe_path;
    command_line += L"\" --whiteboard-helper ";
    command_line += impl_->session_id;

    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    ScopedHandle child_stdin;
    ScopedHandle child_stdout;
    ScopedHandle child_stderr;
    const HANDLE current_process = GetCurrentProcess();
    auto duplicate_inheritable_handle = [&](DWORD std_handle_id, ScopedHandle& target) {
        const HANDLE source = GetStdHandle(std_handle_id);
        if (!source || source == INVALID_HANDLE_VALUE) {
            return false;
        }

        HANDLE duplicated = nullptr;
        if (!DuplicateHandle(current_process,
                             source,
                             current_process,
                             &duplicated,
                             0,
                             TRUE,
                             DUPLICATE_SAME_ACCESS)) {
            return false;
        }

        target.reset(duplicated);
        return true;
    };
    const bool has_stdout = duplicate_inheritable_handle(STD_OUTPUT_HANDLE, child_stdout);
    const bool has_stderr = duplicate_inheritable_handle(STD_ERROR_HANDLE, child_stderr);
    const bool has_stdin = duplicate_inheritable_handle(STD_INPUT_HANDLE, child_stdin);
    const bool use_inherited_console_handles = has_stdout || has_stderr;
    if (use_inherited_console_handles) {
        startup_info.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.hStdInput = has_stdin ? child_stdin.get() : nullptr;
        startup_info.hStdOutput = has_stdout ? child_stdout.get()
                                             : (has_stderr ? child_stderr.get() : nullptr);
        startup_info.hStdError = has_stderr ? child_stderr.get()
                                            : (has_stdout ? child_stdout.get() : nullptr);
    }
    PROCESS_INFORMATION process_info = {};
    if (!CreateProcessW(
            exe_path,
            command_line.data(),
            nullptr,
            nullptr,
            use_inherited_console_handles ? TRUE : FALSE,
            0,
            nullptr,
            nullptr,
            &startup_info,
            &process_info)) {
        Stop();
        return false;
    }

    impl_->process.reset(process_info.hProcess);
    impl_->thread.reset(process_info.hThread);

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(kHelperStartTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool helper_alive = false;
        if (impl_->WithLock(10, [&]() {
                helper_alive = impl_->shared->helper_alive != 0;
            }) && helper_alive) {
            impl_->ready = true;
            return true;
        }
        DWORD wait_result = WaitForSingleObject(impl_->process.get(), 0);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    Stop();
    return false;
}

void WhiteboardCanvasHelperClient::Stop() {
    if (!impl_) return;

    if (impl_->shared && impl_->mutex.get()) {
        impl_->WithLock(50, [&]() {
            impl_->shared->shutdown = 1;
        });
        impl_->SignalHelper();
    }

    if (impl_->process.get()) {
        WaitForSingleObject(impl_->process.get(), 2000);
    }

    impl_->ready = false;
    impl_->ResetCachedState();
    impl_->thread.reset();
    impl_->process.reset();
    if (impl_->shared) {
        UnmapViewOfFile(impl_->shared);
        impl_->shared = nullptr;
    }
    impl_->wake_event.reset();
    impl_->mutex.reset();
    impl_->mapping.reset();
}

bool WhiteboardCanvasHelperClient::IsReady() const {
    return impl_ && impl_->ready;
}

void WhiteboardCanvasHelperClient::ProcessFrame(const cv::Mat& frame, const cv::Mat& person_mask) {
    if (!IsReady() || frame.empty() || !IsValidFrameSize(frame.cols, frame.rows)) return;

    const bool has_person_mask =
        !person_mask.empty() && person_mask.size() == frame.size() &&
        person_mask.type() == CV_8UC1;

    impl_->WithLock(kFrameSubmitLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
        const size_t frame_bytes = FrameBytesForSize(frame.cols, frame.rows);
        std::memcpy(impl_->shared->frame_bgr, frame.data, frame_bytes);
        impl_->shared->frame_width = frame.cols;
        impl_->shared->frame_height = frame.rows;

        if (has_person_mask) {
            const size_t mask_bytes = MaskBytesForSize(frame.cols, frame.rows);
            std::memcpy(impl_->shared->person_mask, person_mask.data, mask_bytes);
            impl_->shared->mask_width = person_mask.cols;
            impl_->shared->mask_height = person_mask.rows;
        } else {
            impl_->shared->mask_width = 0;
            impl_->shared->mask_height = 0;
        }

        impl_->shared->frame_available = 1;
    });
    impl_->SignalHelper();
}

bool WhiteboardCanvasHelperClient::GetViewport(float panX, float panY, float zoom,
                                               cv::Size viewSize, cv::Mat& out_frame) {
    if (!IsReady() || !IsValidFrameSize(viewSize.width, viewSize.height)) return false;

    bool success = false;
    impl_->WithLock(kImageReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
        impl_->shared->pan_x = panX;
        impl_->shared->pan_y = panY;
        impl_->shared->zoom = zoom;
        impl_->shared->viewport_req_width = viewSize.width;
        impl_->shared->viewport_req_height = viewSize.height;

        int source_width = impl_->shared->viewport_width;
        int source_height = impl_->shared->viewport_height;
        const unsigned char* source_buffer = impl_->shared->viewport_bgr;
        if (source_width <= 0 || source_height <= 0) {
            source_width = impl_->shared->overview_width;
            source_height = impl_->shared->overview_height;
            source_buffer = impl_->shared->overview_bgr;
        }

        if (IsValidFrameSize(source_width, source_height)) {
            cv::Mat source(source_height, source_width, CV_8UC3,
                           const_cast<unsigned char*>(source_buffer));
            if (source_width == viewSize.width && source_height == viewSize.height) {
                source.copyTo(out_frame);
            } else {
                cv::resize(source, out_frame, viewSize, 0, 0, cv::INTER_LINEAR);
            }
            success = true;
        }
    });
    impl_->SignalHelper();
    return success;
}

bool WhiteboardCanvasHelperClient::GetOverview(cv::Size viewSize, cv::Mat& out_frame) {
    if (!IsReady() || viewSize.width <= 0 || viewSize.height <= 0) return false;

    bool success = false;
    impl_->WithLock(kImageReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
        impl_->shared->overview_req_width = viewSize.width;
        impl_->shared->overview_req_height = viewSize.height;

        const int source_width = impl_->shared->overview_width;
        const int source_height = impl_->shared->overview_height;
        if (IsValidOverviewSize(source_width, source_height)) {
            cv::Mat source(source_height, source_width, CV_8UC3,
                           impl_->shared->overview_bgr);
            if (source_width == viewSize.width && source_height == viewSize.height) {
                source.copyTo(out_frame);
            } else {
                cv::resize(source, out_frame, viewSize, 0, 0, cv::INTER_LINEAR);
            }
            success = true;
        }
    });
    impl_->SignalHelper();
    return success;
}

void WhiteboardCanvasHelperClient::Reset() {
    if (!IsReady()) return;
    impl_->ResetCachedState();
    impl_->WithLock(20, [&]() {
        impl_->shared->reset_requested = 1;
        impl_->shared->viewport_width = 0;
        impl_->shared->viewport_height = 0;
        impl_->shared->overview_width = 0;
        impl_->shared->overview_height = 0;
        impl_->shared->has_content = 0;
    });
    impl_->SignalHelper();
}

bool WhiteboardCanvasHelperClient::HasContent() const {
    if (!IsReady()) return false;
    impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
    });
    return impl_->cached_has_content.load(std::memory_order_relaxed);
}

bool WhiteboardCanvasHelperClient::IsCanvasViewMode() const {
    if (!IsReady()) return false;
    return impl_->cached_canvas_view_mode.load(std::memory_order_relaxed);
}

void WhiteboardCanvasHelperClient::SetCanvasViewMode(bool mode) {
    if (!IsReady()) return;
    impl_->cached_canvas_view_mode.store(mode, std::memory_order_relaxed);
    impl_->WithLock(20, [&]() {
        impl_->shared->canvas_view_mode = mode ? 1 : 0;
        impl_->RefreshCachedStateUnsafe();
    });
    impl_->SignalHelper();
}

void WhiteboardCanvasHelperClient::SetRenderMode(CanvasRenderMode mode) {
    if (!IsReady()) return;
    impl_->cached_render_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
    impl_->WithLock(20, [&]() {
        impl_->shared->render_mode = static_cast<LONG>(mode);
        impl_->RefreshCachedStateUnsafe();
    });
    impl_->SignalHelper();
}

CanvasRenderMode WhiteboardCanvasHelperClient::GetRenderMode() const {
    if (!IsReady()) return CanvasRenderMode::kStroke;
    return impl_->cached_render_mode.load(std::memory_order_relaxed) ==
            static_cast<int>(CanvasRenderMode::kRaw)
        ? CanvasRenderMode::kRaw
        : CanvasRenderMode::kStroke;
}

cv::Size WhiteboardCanvasHelperClient::GetCanvasSize() const {
    if (!IsReady()) return cv::Size(kDefaultCanvasWidth, kDefaultCanvasHeight);
    impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
    });
    return cv::Size(
        impl_->cached_canvas_width.load(std::memory_order_relaxed),
        impl_->cached_canvas_height.load(std::memory_order_relaxed));
}

int WhiteboardCanvasHelperClient::GetSubCanvasCount() const {
    if (!IsReady()) return 0;
    impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
    });
    return impl_->cached_subcanvas_count.load(std::memory_order_relaxed);
}

int WhiteboardCanvasHelperClient::GetActiveSubCanvasIndex() const {
    if (!IsReady()) return -1;
    impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
    });
    return impl_->cached_active_subcanvas.load(std::memory_order_relaxed);
}

void WhiteboardCanvasHelperClient::SetActiveSubCanvas(int idx) {
    if (!IsReady()) return;
    impl_->WithLock(20, [&]() {
        impl_->shared->pending_active_subcanvas = idx;
    });
    impl_->SignalHelper();
}

int WhiteboardCanvasHelperClient::GetSortedSubCanvasIndex(int pos) const {
    return pos < 0 ? -1 : pos;
}

int WhiteboardCanvasHelperClient::GetSortedPosition(int idx) const {
    return idx < 0 ? -1 : idx;
}

void WhiteboardCanvasHelperClient::SyncSettings(bool debug_enabled,
                                                float enhance_threshold,
                                                float yolo_fps) {
    if (!IsReady()) return;
    impl_->WithLock(20, [&]() {
        impl_->shared->whiteboard_debug = debug_enabled ? 1 : 0;
        impl_->shared->enhance_threshold = enhance_threshold;
        impl_->shared->yolo_fps = yolo_fps;
        impl_->RefreshCachedStateUnsafe();
    });
    impl_->SignalHelper();
}

int WhiteboardCanvasHelperClient::GetGraphNodeCount() const {
    if (!IsReady()) return 0;
    int count = 0;
    impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
        count = static_cast<int>(impl_->shared->graph_node_count);
    });
    return count;
}

int WhiteboardCanvasHelperClient::GetGraphNodes(float* buffer, int max_nodes) const {
    if (!IsReady() || !buffer || max_nodes <= 0) return 0;
    int count = 0;
    impl_->WithLock(kImageReadLockTimeoutMs, [&]() {
        const int total_floats = static_cast<int>(impl_->shared->graph_node_floats_written);
        const int available_nodes = total_floats / kGraphNodeStride;
        count = std::min(available_nodes, max_nodes);
        if (count > 0) {
            std::memcpy(buffer, impl_->shared->graph_nodes,
                        static_cast<size_t>(count) * kGraphNodeStride * sizeof(float));
        }
    });
    return count;
}

int WhiteboardCanvasHelperClient::GetGraphNodeContours(float* buffer, int max_floats) const {
    if (!IsReady() || !buffer || max_floats <= 0) return 0;
    int written = 0;
    impl_->WithLock(kImageReadLockTimeoutMs, [&]() {
        const int available = static_cast<int>(impl_->shared->graph_contour_floats_written);
        written = std::min(available, max_floats);
        if (written > 0) {
            std::memcpy(buffer, impl_->shared->graph_contours,
                        static_cast<size_t>(written) * sizeof(float));
        }
    });
    return written;
}

bool WhiteboardCanvasHelperClient::GetGraphCanvasBounds(int* bounds) const {
    if (!IsReady() || !bounds) return false;
    bool valid = false;
    impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
        valid = impl_->shared->graph_bounds_valid != 0;
        if (valid) {
            bounds[0] = static_cast<int>(impl_->shared->graph_bounds_x);
            bounds[1] = static_cast<int>(impl_->shared->graph_bounds_y);
            bounds[2] = static_cast<int>(impl_->shared->graph_bounds_w);
            bounds[3] = static_cast<int>(impl_->shared->graph_bounds_h);
        }
    });
    return valid;
}

bool WhiteboardCanvasHelperClient::CompareGraphNodes(int id_a, int id_b, float* result) const {
    if (!IsReady() || !result) return false;

    const int request_id =
        impl_->next_graph_compare_request_id.fetch_add(1, std::memory_order_relaxed);
    const bool queued = impl_->WithLock(20, [&]() {
        impl_->shared->graph_compare_node_a = id_a;
        impl_->shared->graph_compare_node_b = id_b;
        impl_->shared->graph_compare_request_id = request_id;
        impl_->shared->graph_compare_result_ready = 0;
        impl_->shared->graph_compare_result_ok = 0;
        impl_->shared->graph_compare_result_id = 0;
    });
    if (!queued) return false;

    impl_->SignalHelper();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kGraphCompareTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = false;
        bool ok = false;
        float local_result[10] = {};

        impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
            ready = impl_->shared->graph_compare_result_ready != 0 &&
                    impl_->shared->graph_compare_result_id == request_id;
            if (ready) {
                ok = impl_->shared->graph_compare_result_ok != 0;
                if (ok) {
                    std::memcpy(local_result,
                                impl_->shared->graph_compare_result,
                                sizeof(local_result));
                }
            }
        });

        if (ready) {
            if (ok) {
                std::memcpy(result, local_result, sizeof(local_result));
            }
            return ok;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return false;
}

int WhiteboardCanvasHelperClient::LockAllGraphNodes() {
    if (!IsReady()) return 0;

    const int request_id =
        impl_->next_edit_request_id.fetch_add(1, std::memory_order_relaxed);
    const bool queued = impl_->WithLock(20, [&]() {
        impl_->shared->edit_lock_all = 1;
        impl_->shared->edit_delete_count = 0;
        impl_->shared->edit_move_count = 0;
        impl_->shared->edit_request_id = request_id;
        impl_->shared->edit_result_ready = 0;
        impl_->shared->edit_result_ok = 0;
        impl_->shared->edit_result_id = 0;
    });
    if (!queued) return 0;

    impl_->SignalHelper();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kEditCommandTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = false;
        bool ok = false;

        impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
            ready = impl_->shared->edit_result_ready != 0 &&
                    impl_->shared->edit_result_id == request_id;
            if (ready) {
                ok = impl_->shared->edit_result_ok != 0;
            }
        });

        if (ready) {
            // Return node count from shared state as approximation
            int count = 0;
            impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
                count = static_cast<int>(impl_->shared->graph_node_count);
            });
            return ok ? count : 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return 0;
}

bool WhiteboardCanvasHelperClient::ApplyUserEdits(const int* delete_ids, int delete_count,
                                                   const float* moves, int move_count) {
    if (!IsReady()) return false;
    if (delete_count > kMaxEditDeletes || move_count > kMaxEditMoves) return false;

    const int request_id =
        impl_->next_edit_request_id.fetch_add(1, std::memory_order_relaxed);
    const bool queued = impl_->WithLock(20, [&]() {
        impl_->shared->edit_lock_all = 0;
        impl_->shared->edit_delete_count = delete_count;
        impl_->shared->edit_move_count = move_count;
        if (delete_count > 0 && delete_ids) {
            std::memcpy(impl_->shared->edit_delete_ids, delete_ids,
                        static_cast<size_t>(delete_count) * sizeof(int));
        }
        if (move_count > 0 && moves) {
            std::memcpy(impl_->shared->edit_moves, moves,
                        static_cast<size_t>(move_count) * 3 * sizeof(float));
        }
        impl_->shared->edit_request_id = request_id;
        impl_->shared->edit_result_ready = 0;
        impl_->shared->edit_result_ok = 0;
        impl_->shared->edit_result_id = 0;
    });
    if (!queued) return false;

    impl_->SignalHelper();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kEditCommandTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = false;
        bool ok = false;

        impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
            ready = impl_->shared->edit_result_ready != 0 &&
                    impl_->shared->edit_result_id == request_id;
            if (ready) {
                ok = impl_->shared->edit_result_ok != 0;
            }
        });

        if (ready) {
            return ok;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return false;
}

int WhiteboardCanvasHelperClient::GetGraphNodeMasks(uint8_t* buffer, int max_bytes) const {
    if (!IsReady() || !buffer || max_bytes <= 0) return 0;

    const int request_id =
        impl_->next_mask_request_id.fetch_add(1, std::memory_order_relaxed);
    const bool queued = impl_->WithLock(20, [&]() {
        impl_->shared->mask_request_id = request_id;
        impl_->shared->mask_result_ready = 0;
        impl_->shared->mask_result_id = 0;
        impl_->shared->mask_result_bytes = 0;
    });
    if (!queued) return 0;

    impl_->SignalHelper();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kMaskRequestTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = false;
        int bytes_written = 0;

        impl_->WithLock(kStateReadLockTimeoutMs, [&]() {
            ready = impl_->shared->mask_result_ready != 0 &&
                    impl_->shared->mask_result_id == request_id;
            if (ready) {
                bytes_written = static_cast<int>(impl_->shared->mask_result_bytes);
                if (bytes_written > 0) {
                    const int copy_bytes = std::min(bytes_written, max_bytes);
                    std::memcpy(buffer, impl_->shared->mask_data, copy_bytes);
                    bytes_written = copy_bytes;
                }
            }
        });

        if (ready) {
            return bytes_written;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}

void SetWhiteboardCanvasHelperProcessMode(bool helper_mode, const std::string& session_id) {
    g_is_helper_process = helper_mode;
    g_helper_session_id = session_id;
}

bool IsWhiteboardCanvasHelperProcess() {
    return g_is_helper_process;
}

int RunWhiteboardCanvasHelperMain(const std::string& session_id) {
    SetWhiteboardCanvasHelperProcessMode(true, session_id);
    WhiteboardHelperServer server(session_id);
    return server.Run();
}
