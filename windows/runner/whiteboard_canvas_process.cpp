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
constexpr int kMaxOverviewWidth = 1920;
constexpr int kMaxOverviewHeight = 1080;
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
constexpr int kYoloPerfLogMinFrames = 30;
const auto kYoloPerfLogMinInterval = std::chrono::seconds(10);

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
    unsigned char frame_bgr[kMaxFrameBytes];
    unsigned char person_mask[kMaxMaskBytes];
    unsigned char viewport_bgr[kMaxFrameBytes];
    unsigned char overview_bgr[kMaxOverviewBytes];
};
#pragma pack(pop)

struct HelperYoloPerfStats {
    double inference_ms = 0.0;
    int frames = 0;
    int refreshes = 0;
    int reuses = 0;
    std::chrono::time_point<std::chrono::steady_clock> last_log =
        std::chrono::steady_clock::now();
};

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

double DurationMs(const std::chrono::steady_clock::time_point& start,
                  const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void MaybePrintYoloPerfLog(HelperYoloPerfStats& stats) {
    const auto now = std::chrono::steady_clock::now();
    const bool enough_frames = stats.frames >= kYoloPerfLogMinFrames;
    const bool enough_time =
        stats.frames > 0 && (now - stats.last_log) >= kYoloPerfLogMinInterval;

    if (!enough_frames && !enough_time) return;

    const double frames = static_cast<double>(std::max(1, stats.frames));
    const double refreshes = static_cast<double>(std::max(1, stats.refreshes));
    const double elapsed_seconds = std::max(
        0.001,
        std::chrono::duration<double>(now - stats.last_log).count());
    const double fps = frames / elapsed_seconds;
    std::cout << "[WhiteboardYoloPerf] frames=" << stats.frames
              << " refreshes=" << stats.refreshes
              << " reused=" << stats.reuses
              << " fps=" << fps
              << " avgMs/frame=" << (stats.inference_ms / frames)
              << " avgMs/refresh=" << (stats.inference_ms / refreshes)
              << std::endl;

    stats = HelperYoloPerfStats();
    stats.last_log = now;
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
        HelperYoloPerfStats yolo_perf_stats;
        cv::Mat last_viewport;
        cv::Mat last_overview;
        cv::Size latest_output_size(kDefaultCanvasWidth, kDefaultCanvasHeight);

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
                if (person_mask.empty()) {
                    const auto yolo_start = std::chrono::steady_clock::now();
                    person_mask = GetWhiteboardPersonMask(snapshot.frame);
                    const auto yolo_end = std::chrono::steady_clock::now();
                    yolo_perf_stats.inference_ms += DurationMs(yolo_start, yolo_end);
                    yolo_perf_stats.refreshes++;
                    yolo_perf_stats.frames++;
                    MaybePrintYoloPerfLog(yolo_perf_stats);
                }
                canvas.ProcessFrame(snapshot.frame, person_mask);
            }

            const bool has_content = canvas.HasContent();
            const bool canvas_view_mode = canvas.IsCanvasViewMode();

            if (has_content && latest_output_size.width > 0 && latest_output_size.height > 0) {
                // Cap overview size to fit shared memory buffer.
                cv::Size overview_size = latest_output_size;
                if (overview_size.width > kMaxOverviewWidth || overview_size.height > kMaxOverviewHeight) {
                    const float scale = std::min(
                        static_cast<float>(kMaxOverviewWidth) / overview_size.width,
                        static_cast<float>(kMaxOverviewHeight) / overview_size.height);
                    overview_size.width = static_cast<int>(overview_size.width * scale);
                    overview_size.height = static_cast<int>(overview_size.height * scale);
                }

                cv::Mat overview;
                if (canvas.GetOverviewBlocking(overview_size, overview)) {
                    last_overview = overview;
                    if (canvas_view_mode) {
                        last_viewport = overview;
                    }
                }
            }

            WriteResults(canvas, last_viewport, last_overview);
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

        Unlock(mutex_.get());
        return true;
    }

    void WriteResults(WhiteboardCanvas& canvas,
                      const cv::Mat& viewport,
                      const cv::Mat& overview) {
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

        if (!WaitAndLock(mutex_.get(), 50)) return;

        shared_->helper_alive = 1;
        shared_->has_content = has_content ? 1 : 0;
        shared_->canvas_width = canvas_size.width;
        shared_->canvas_height = canvas_size.height;
        shared_->subcanvas_count = subcanvas_count;
        shared_->active_subcanvas = active_subcanvas;

        if (!has_content) {
            shared_->viewport_width = 0;
            shared_->viewport_height = 0;
            shared_->overview_width = 0;
            shared_->overview_height = 0;
            Unlock(mutex_.get());
            return;
        }

        if (!viewport.empty() && IsValidFrameSize(viewport.cols, viewport.rows)) {
            const size_t viewport_bytes = FrameBytesForSize(viewport.cols, viewport.rows);
            std::memcpy(shared_->viewport_bgr, viewport.data, viewport_bytes);
            shared_->viewport_width = viewport.cols;
            shared_->viewport_height = viewport.rows;
        }

        if (!overview.empty() && IsValidOverviewSize(overview.cols, overview.rows)) {
            const size_t overview_bytes = FrameBytesForSize(overview.cols, overview.rows);
            std::memcpy(shared_->overview_bgr, overview.data, overview_bytes);
            shared_->overview_width = overview.cols;
            shared_->overview_height = overview.rows;
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
    PROCESS_INFORMATION process_info = {};
    if (!CreateProcessW(
            exe_path,
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
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

    impl_->WithLock(kFrameSubmitLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();
        const size_t frame_bytes = FrameBytesForSize(frame.cols, frame.rows);
        std::memcpy(impl_->shared->frame_bgr, frame.data, frame_bytes);
        impl_->shared->frame_width = frame.cols;
        impl_->shared->frame_height = frame.rows;

        if (!person_mask.empty() && person_mask.size() == frame.size()) {
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
    return success;
}

bool WhiteboardCanvasHelperClient::GetOverview(cv::Size viewSize, cv::Mat& out_frame) {
    if (!IsReady() || viewSize.width <= 0 || viewSize.height <= 0) return false;

    bool success = false;
    impl_->WithLock(kImageReadLockTimeoutMs, [&]() {
        impl_->RefreshCachedStateUnsafe();

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
