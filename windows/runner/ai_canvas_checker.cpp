// ============================================================================
// ai_canvas_checker.cpp -- LM Studio vision-based canvas creation gate
//
// WinHTTP lives in http_client.cpp so that its Windows headers never mix
// with OpenCV headers in the same translation unit.
// ============================================================================

#include "ai_canvas_checker.h"
#include "http_client.h"
#include "json.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using nlohmann::json;

// Simple debug log — visible in DebugView and VS Output window
static void AILog(const char* msg) {
    OutputDebugStringA("[AI] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    // Also print to stderr so it appears in flutter run console
    fprintf(stderr, "[AI] %s\n", msg);
    fflush(stderr);
}
static void AILogf(const char* fmt, ...) {
    char buf[5120];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    AILog(buf);
}

// ============================================================================
//  Internal helpers (anonymous namespace)
// ============================================================================
namespace {

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) b |= static_cast<uint32_t>(data[i + 2]);
        out += kB64[(b >> 18) & 0x3F];
        out += kB64[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? kB64[(b >>  6) & 0x3F] : '=';
        out += (i + 2 < len) ? kB64[(b      ) & 0x3F] : '=';
    }
    return out;
}

std::string MatToDataUrl(const cv::Mat& frame, int quality = 70, int max_dim = 160) {
    if (frame.empty()) return {};
    cv::Mat src;
    if (frame.channels() == 4)
        cv::cvtColor(frame, src, cv::COLOR_RGBA2BGR);
    else
        src = frame;
    if (src.rows > max_dim || src.cols > max_dim) {
        double scale = static_cast<double>(max_dim) / std::max(src.rows, src.cols);
        cv::resize(src, src, cv::Size(), scale, scale, cv::INTER_AREA);
    }
    std::vector<uchar> buf;
    cv::imencode(".jpg", src, buf, {cv::IMWRITE_JPEG_QUALITY, quality});
    return "data:image/jpeg;base64," + Base64Encode(buf.data(), buf.size());
}

bool IsLMStudioReachable() {
    AILog("Probing LM Studio at localhost:1234/v1/models (GET) ...");
    auto resp = HttpGetLocal(1234, "/v1/models", 10000);  // 10s timeout for CPU-only inference
    if (resp.empty()) {
        AILog("LMS probe result: OFFLINE (no response)");
        return false;
    }
    // Must have at least one loaded model (response contains "id")
    bool has_model = resp.find("\"id\"") != std::string::npos;
    AILogf("LMS probe result: %s (resp_len=%zu)",
           has_model ? "REACHABLE+MODEL" : "REACHABLE_NO_MODEL", resp.size());
    if (!has_model) {
        AILogf("LMS models response: %.200s", resp.c_str());
    }
    return has_model;
}

std::string BuildCompletion(const std::vector<std::string>& image_urls,
                            const std::string& prompt_text) {
    json content = json::array();
    for (const auto& url : image_urls)
        content.push_back({{"type", "image_url"}, {"image_url", {{"url", url}}}});
    content.push_back({{"type", "text"}, {"text", prompt_text}});

    json body = {
        {"model", "google/gemma-4-e2b"},
        {"messages", json::array({{{"role", "user"}, {"content", content}}})},
        {"max_tokens", 500},
        {"temperature", 0.5},
        {"response_format", {
            {"type", "json_schema"},
            {"json_schema", {
                {"name", "answer"},
                {"strict", true},
                {"schema", {
                    {"type", "object"},
                    {"properties", {
                        {"thoughts", {{"type", "string"}}},
                        {"answer",   {{"type", "boolean"}}}
                    }},
                    {"required", {"thoughts", "answer"}},
                    {"additionalProperties", false}
                }}
            }}
        }}
    };
    return body.dump();
}

bool ExtractYesNo(const std::string& raw_json, bool& out_yes) {
    auto parse_answer_obj = [](const json& obj, bool& answer,
                               std::string& thoughts) -> bool {
        if (!obj.is_object()) return false;
        if (!obj.contains("answer") || !obj["answer"].is_boolean()) return false;
        answer = obj["answer"].get<bool>();
        if (obj.contains("thoughts") && obj["thoughts"].is_string())
            thoughts = obj["thoughts"].get<std::string>();
        return true;
    };

    bool answer = false;
    std::string thoughts = "(no thoughts)";
    bool parsed_field = false;

    json root = json::parse(raw_json, nullptr, false);
    if (!root.is_discarded()) {
        try {
            // OpenAI-style envelope: choices[0].message.content
            if (root.contains("choices") && root["choices"].is_array() && !root["choices"].empty()) {
                const auto& choice0 = root["choices"][0];
                if (choice0.contains("message") && choice0["message"].is_object()) {
                    const auto& msg = choice0["message"];
                    if (msg.contains("content")) {
                        const auto& content = msg["content"];
                        if (content.is_object()) {
                            parsed_field = parse_answer_obj(content, answer, thoughts);
                        } else if (content.is_string()) {
                            std::string s = content.get<std::string>();
                            json inner = json::parse(s, nullptr, false);
                            if (!inner.is_discarded()) {
                                parsed_field = parse_answer_obj(inner, answer, thoughts);
                            }
                            if (!parsed_field)
                                AILogf("ExtractYesNo: content string failed structured parse (raw=%.120s)",
                                       s.c_str());
                        }
                    }
                }
            }

            // Fallback: maybe server already returned the structured object directly.
            if (!parsed_field)
                parsed_field = parse_answer_obj(root, answer, thoughts);
        } catch (...) {
            parsed_field = false;
        }
    }

    if (!parsed_field) {
        AILogf("AI answer: (not found, json_len=%zu, raw=%.200s)", raw_json.size(), raw_json.c_str());
        return false;
    }

    out_yes = answer;
    AILogf("AI thoughts: %s\nAI answer:   %s", thoughts.c_str(), answer ? "true (APPROVE)" : "false (REJECT)");
    return true;
}

std::string PostWithReloadRetry(const std::string& body) {
    constexpr int kCompletionTimeoutMs = 2400000; // 40 minutes (10x)
    auto resp = HttpPostLocal(1234, "/v1/chat/completions", body, kCompletionTimeoutMs);
    
    // Detect transient errors: empty response or explicit model reload message
    bool looks_transient =
        resp.empty() ||
        (resp.find("\"error\"") != std::string::npos &&
         resp.find("Model reloaded") != std::string::npos);
    
    // Also detect truncated responses (thoughts without answer = model unloaded mid-generation)
    bool looks_truncated =
        resp.find("\"thoughts\"") != std::string::npos &&
        resp.find("\"answer\"") == std::string::npos;
    
    if (looks_transient || looks_truncated) {
        if (looks_truncated) {
            AILog("PostWithReloadRetry: truncated (model unloaded mid-response) -> retry @2400s");
        } else {
            AILog("PostWithReloadRetry: transient (empty/reloaded) -> retry @2400s");
        }
        resp = HttpPostLocal(1234, "/v1/chat/completions", body, kCompletionTimeoutMs);
    }
    return resp;
}

static const char kSeedPrompt[] =
    "The following image is taken from a classroom camera feed. it may contain students, a lecturer, and a whiteboard. "
    "Set answer=true only if ALL of these are true: "
    "(1) The camera is zoomed on the whiteboard area (not showing the whole room); the lecturer may be visible; students should NOT be visible; "
    "(2) The whiteboard is actively being used -- i.e. there are visible writings or drawings on it; "
    "(3) The whiteboard content is well-lit, focused and readable. "
    "Set answer=false if any condition is not met.";

} // namespace

// ============================================================================
//  AICanvasChecker -- out-of-line methods
// ============================================================================

AICanvasChecker::AICanvasChecker()
    : enabled_(false)
    , ai_status_(static_cast<int>(AICheckStatus::kDisabled))
    , seed_approved_(false)
    , dupe_pending_(false)
    , dupe_passed_(false)
    , stop_worker_(false)
    , video_pos_ms_(-1)
    , last_seed_check_video_sec_(-1.0)
    , last_dupe_check_video_sec_(-1.0) {
    worker_thread_ = std::thread(&AICanvasChecker::WorkerLoop, this);
}

AICanvasChecker::~AICanvasChecker() {
    stop_worker_.store(true);
    queue_cv_.notify_all();
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool AICanvasChecker::IsEnabled() const {
    return enabled_.load(std::memory_order_relaxed);
}

bool AICanvasChecker::IsSeedApproved() const {
    return seed_approved_.load(std::memory_order_relaxed);
}

void AICanvasChecker::ClearSeedApproval() {
    seed_approved_.store(false, std::memory_order_relaxed);
}

bool AICanvasChecker::IsDuplicateCheckPending() const {
    return dupe_pending_.load(std::memory_order_relaxed);
}

bool AICanvasChecker::IsDuplicateCheckPassed() const {
    return dupe_passed_.load(std::memory_order_relaxed);
}

AICheckStatus AICanvasChecker::GetStatus() const {
    return static_cast<AICheckStatus>(ai_status_.load(std::memory_order_relaxed));
}

void AICanvasChecker::SetStatusInternal(AICheckStatus s) {
    ai_status_.store(static_cast<int>(s), std::memory_order_relaxed);
}

void AICanvasChecker::SetEnabled(bool enabled) {
    AILogf("SetEnabled(%s)", enabled ? "true" : "false");
    enabled_.store(enabled);
    if (enabled) {
        enabled_time_ = Clock::now();
        SetStatusInternal(AICheckStatus::kIdle);
        AILog("AI checker now ENABLED (status=kIdle, initial delay=10s)");
    } else {
        SetStatusInternal(AICheckStatus::kDisabled);
        seed_approved_.store(false);
        dupe_pending_.store(false);
        dupe_passed_.store(false);
        last_seed_check_time_ = {};
        last_dupe_check_time_ = {};
        last_seed_check_video_sec_ = -1.0;
        last_dupe_check_video_sec_ = -1.0;
        AILog("AI checker now DISABLED");
    }
}

void AICanvasChecker::SetVideoPosition(double sec) {
    video_pos_ms_.store(static_cast<int64_t>(sec * 1000.0), std::memory_order_relaxed);
}

void AICanvasChecker::ResetDuplicateCheck() {
    dupe_pending_.store(false);
    dupe_passed_.store(false);
    last_dupe_check_time_ = {};
}

void AICanvasChecker::MaybeRequestSeedCheck(const cv::Mat& frame_bgr) {
    if (!enabled_.load()) { AILog("MaybeRequestSeedCheck: skip (disabled)"); return; }
    if (seed_approved_.load()) { AILog("MaybeRequestSeedCheck: skip (already approved)"); return; }

    auto now = Clock::now();
#ifndef KAPTCHI_CLI
    {
        auto elapsed_since_enable = std::chrono::duration_cast<std::chrono::seconds>(
            now - enabled_time_).count();
        if (elapsed_since_enable < kInitialDelaySeconds) {
            AILogf("MaybeRequestSeedCheck: skip (initial delay, %lld/%d s)",
                   (long long)elapsed_since_enable, kInitialDelaySeconds);
            return;
        }
    }
#endif
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!queue_.empty()) { AILog("MaybeRequestSeedCheck: skip (queue not empty)"); return; }

        const int64_t vpos_ms = video_pos_ms_.load(std::memory_order_relaxed);
        const bool use_video_time = (vpos_ms >= 0);
        if (use_video_time) {
            const double vpos = vpos_ms / 1000.0;
            const bool first = (last_seed_check_video_sec_ < 0.0);
            const double elapsed = vpos - last_seed_check_video_sec_;
            if (!first && elapsed < kSeedCheckIntervalSec) {
                AILogf("MaybeRequestSeedCheck: throttled (%.1f video-s elapsed, need %d)",
                       elapsed, kSeedCheckIntervalSec);
                return;
            }
        } else {
            bool first = (last_seed_check_time_.time_since_epoch().count() == 0);
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_seed_check_time_).count();
            if (!first && elapsed < kSeedCheckIntervalSec) {
                AILogf("MaybeRequestSeedCheck: throttled (%lld real-s elapsed, need %d)",
                       (long long)elapsed, kSeedCheckIntervalSec);
                return;
            }
        }

        CheckRequest req;
        req.kind  = CheckRequest::kSeed;
        req.frame = frame_bgr.clone();
        last_seed_check_time_ = now;
        if (use_video_time) last_seed_check_video_sec_ = vpos_ms / 1000.0;
        queue_.push_back(std::move(req));
        AILogf("MaybeRequestSeedCheck: QUEUED seed check (frame=%dx%d)",
               frame_bgr.cols, frame_bgr.rows);
    }
    queue_cv_.notify_one();
}

void AICanvasChecker::MaybeRequestDuplicateCheck(
    const cv::Mat& frame_bgr, const std::vector<cv::Mat>& existing_overviews) {
    if (!enabled_.load()) { AILog("MaybeRequestDuplicateCheck: skip (disabled)"); return; }
    if (dupe_pending_.load()) { AILog("MaybeRequestDuplicateCheck: skip (already pending)"); return; }

    auto now = Clock::now();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!queue_.empty()) { AILog("MaybeRequestDuplicateCheck: skip (queue not empty)"); return; }

        const int64_t vpos_ms = video_pos_ms_.load(std::memory_order_relaxed);
        const bool use_video_time = (vpos_ms >= 0);
        if (use_video_time) {
            const double vpos = vpos_ms / 1000.0;
            const bool first = (last_dupe_check_video_sec_ < 0.0);
            const double elapsed = vpos - last_dupe_check_video_sec_;
            if (!first && elapsed < kDupeCheckIntervalSec) {
                AILogf("MaybeRequestDuplicateCheck: throttled (%.1f video-s elapsed, need %d)",
                       elapsed, kDupeCheckIntervalSec);
                return;
            }
        } else {
            bool first = (last_dupe_check_time_.time_since_epoch().count() == 0);
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_dupe_check_time_).count();
            if (!first && elapsed < kDupeCheckIntervalSec) {
                AILogf("MaybeRequestDuplicateCheck: throttled (%lld real-s elapsed, need %d)",
                       (long long)elapsed, kDupeCheckIntervalSec);
                return;
            }
        }

        CheckRequest req;
        req.kind  = CheckRequest::kDuplicate;
        req.frame = frame_bgr.clone();
        for (const auto& ov : existing_overviews)
            req.existing_overviews.push_back(ov.clone());
        last_dupe_check_time_ = now;
        if (use_video_time) last_dupe_check_video_sec_ = vpos_ms / 1000.0;
        queue_.push_back(std::move(req));
        AILogf("MaybeRequestDuplicateCheck: QUEUED dupe check (overviews=%zu)",
               existing_overviews.size());
    }
    dupe_pending_.store(true);
    queue_cv_.notify_one();
}

static void RunDetached(const char* cmdline) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "cmd.exe /C %s", cmdline);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessA(nullptr, buf, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        AILogf("RunDetached: CreateProcess failed (err=%lu) cmd=%.200s", GetLastError(), buf);
    }
}

void AICanvasChecker::TriggerLMSStart() {
    AILog("TriggerLMSStart: checking if LMS is already running...");
    if (IsLMStudioReachable()) {
        AILog("TriggerLMSStart: LMS already reachable -- skipping spawn");
        SetStatusInternal(AICheckStatus::kIdle);
        return;
    }
    AILog("TriggerLMSStart: LMS offline -> launching server + loading gemma-4-e2b --gpu 1.0");
    SetStatusInternal(AICheckStatus::kIdle);
    RunDetached("lms server start && lms load gemma-4-e2b --gpu 1.0");
}

void AICanvasChecker::WorkerLoop() {
    AILog("WorkerLoop: started");
    while (!stop_worker_.load()) {
        CheckRequest req;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return stop_worker_.load() || !queue_.empty();
            });
            if (stop_worker_.load()) break;
            req = std::move(queue_.front());
            queue_.pop_front();
        }

        AILogf("WorkerLoop: processing %s request",
               req.kind == CheckRequest::kSeed ? "SEED" : "DUPLICATE");

        if (!IsLMStudioReachable()) {
            AILog("WorkerLoop: LMS offline -> kLmsOffline");
            SetStatusInternal(AICheckStatus::kLmsOffline);
            if (req.kind == CheckRequest::kDuplicate)
                dupe_pending_.store(false);
            continue;
        }

        if (req.kind == CheckRequest::kSeed) {
            SetStatusInternal(AICheckStatus::kCheckingSeed);
            bool approved = DoSeedCheck(req.frame);
            seed_approved_.store(approved);
            AILogf("WorkerLoop: seed check result = %s", approved ? "APPROVED" : "REJECTED");
            SetStatusInternal(approved ? AICheckStatus::kSeedApproved
                                       : AICheckStatus::kSeedRejected);
        } else {
            SetStatusInternal(AICheckStatus::kCheckingDupe);
            bool is_new = DoDuplicateCheck(req.frame, req.existing_overviews);
            dupe_passed_.store(is_new);
            dupe_pending_.store(false);
            AILogf("WorkerLoop: dupe check result = %s", is_new ? "NEW" : "DUPLICATE");
            SetStatusInternal(is_new ? AICheckStatus::kDupeNew : AICheckStatus::kDupeExists);
        }
    }
    AILog("WorkerLoop: exiting");
}

bool AICanvasChecker::DoSeedCheck(const cv::Mat& frame) {
    AILogf("DoSeedCheck: encoding frame %dx%d", frame.cols, frame.rows);
    auto url = MatToDataUrl(frame, 70, 160);
    if (url.empty()) { AILog("DoSeedCheck: MatToDataUrl returned empty!"); return false; }
    AILogf("DoSeedCheck: data URL len=%zu, posting to LMS...", url.size());
    auto body = BuildCompletion({url}, kSeedPrompt);
    AILogf("DoSeedCheck: request body_len=%zu", body.size());
    auto resp = PostWithReloadRetry(body);
    bool yes = false;
    bool parsed = ExtractYesNo(resp, yes);
    AILogf("DoSeedCheck: parsed=%s yes=%s -> returning %s",
           parsed ? "true" : "false", yes ? "true" : "false",
           (parsed && yes) ? "APPROVED" : "REJECTED");
    return parsed && yes;
}

bool AICanvasChecker::DoDuplicateCheck(const cv::Mat& frame,
                                        const std::vector<cv::Mat>& existing) {
    AILogf("DoDuplicateCheck: frame %dx%d, existing=%zu", frame.cols, frame.rows, existing.size());
    if (existing.empty()) { AILog("DoDuplicateCheck: no existing canvases -> NEW"); return true; }

    std::string prompt =
        "I have a new whiteboard image (first image) and " +
        std::to_string(existing.size()) +
        " existing canvas snapshot(s) (remaining image(s)). "
        "Does the new image contain content that is ALREADY visible in one of the "
        "existing snapshots? "
        "Set answer=true if the content is already covered, answer=false if it is genuinely new.";

    std::vector<std::string> urls = {MatToDataUrl(frame, 70, 160)};
    for (const auto& ov : existing)
        urls.push_back(MatToDataUrl(ov, 70, 64));
    auto body = BuildCompletion(urls, prompt);
    AILogf("DoDuplicateCheck: request body_len=%zu, posting...", body.size());
    auto resp = PostWithReloadRetry(body);
    AILogf("DoDuplicateCheck: response_len=%zu, first100=%.100s", resp.size(), resp.c_str());
    bool yes = false;
    if (!ExtractYesNo(resp, yes)) { AILog("DoDuplicateCheck: parse fail -> treat as NEW"); return true; }
    AILogf("DoDuplicateCheck: yes=%s -> returning %s", yes ? "true" : "false", !yes ? "NEW" : "DUPLICATE");
    return !yes;
}
