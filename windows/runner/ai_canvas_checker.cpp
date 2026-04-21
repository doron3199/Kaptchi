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

std::string EscapeJson(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 16);
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else                r += c;
    }
    return r;
}

std::string ImageBlock(const std::string& data_url) {
    return
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" + data_url + "\"}}";
}

std::string TextBlock(const std::string& text) {
    return "{\"type\":\"text\",\"text\":\"" + EscapeJson(text) + "\"}";
}

std::string BuildCompletion(const std::string& content_arr) {
    // thoughts comes before answer so the model reasons before committing.
    return
        "{\"model\":\"google/gemma-4-e2b\","
        "\"messages\":[{\"role\":\"user\",\"content\":"
        + content_arr
                + "}],\"max_tokens\":8000,\"temperature\":0,"
        "\"response_format\":{"
          "\"type\":\"json_schema\","
          "\"json_schema\":{"
            "\"name\":\"answer\","
            "\"strict\":true,"
            "\"schema\":{"
              "\"type\":\"object\","
              "\"properties\":{"
                "\"thoughts\":{\"type\":\"string\"},"
                "\"answer\":{\"type\":\"string\",\"enum\":[\"YES\",\"NO\"]}"
              "},"
              "\"required\":[\"thoughts\",\"answer\"],"
              "\"additionalProperties\":false"
        "}}}}";
}

// Extracts the "thoughts" string from the double-escaped JSON inside the response body.
// The model emits {"thoughts":"...","answer":"..."} encoded as a JSON string value,
// so quotes inside are \" in the raw bytes.
static std::string ExtractThoughts(const std::string& json) {
    size_t pos = json.find("thoughts");
    if (pos == std::string::npos) return "(no thoughts)";
    size_t colon = json.find(':', pos + 8);
    if (colon == std::string::npos) return "(no colon)";
    size_t start = colon + 1;
    while (start < json.size() &&
           (json[start] == ' ' || json[start] == '\\' || json[start] == '"'))
        ++start;
    std::string result;
    for (size_t i = start; i < json.size() && result.size() < 5000; ++i) {
        if (json[i] == '\\') {
            if (i + 1 >= json.size()) break;
            if (json[i+1] == '"') break; // \" = end of escaped string value
            if (json[i+1] == 'n') { result += ' '; ++i; }
            else { ++i; result += json[i]; }
        } else if (json[i] == '"') {
            break;
        } else {
            result += json[i];
        }
    }
    return result.empty() ? "(empty)" : result;
}

bool ExtractYesNo(const std::string& raw_json, bool& out_yes) {
    std::string thoughts = ExtractThoughts(raw_json);

    auto parse_answer_obj = [](const json& obj, std::string& answer) -> bool {
        if (!obj.is_object()) return false;
        if (!obj.contains("answer") || !obj["answer"].is_string()) return false;
        answer = obj["answer"].get<std::string>();
        for (auto& c : answer) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return true;
    };

    std::string answer;
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
                            parsed_field = parse_answer_obj(content, answer);
                        } else if (content.is_string()) {
                            json inner = json::parse(content.get<std::string>(), nullptr, false);
                            if (!inner.is_discarded()) {
                                parsed_field = parse_answer_obj(inner, answer);
                                if (!parsed_field && inner.contains("thoughts") && inner["thoughts"].is_string()) {
                                    thoughts = inner["thoughts"].get<std::string>();
                                }
                            }
                        }
                    }
                }
            }

            // Fallback: maybe server already returned the structured object directly.
            if (!parsed_field) {
                parsed_field = parse_answer_obj(root, answer);
            }
        } catch (...) {
            parsed_field = false;
        }
    }

    if (!parsed_field) {
        AILogf("AI thoughts: %s\nAI answer:   (not found, json_len=%zu)", thoughts.c_str(), raw_json.size());
        return false;
    }

    if (answer == "YES") {
        out_yes = true;
        AILogf("AI thoughts: %s\nAI answer:   YES", thoughts.c_str());
        return true;
    }
    if (answer == "NO") {
        out_yes = false;
        AILogf("AI thoughts: %s\nAI answer:   NO", thoughts.c_str());
        return true;
    }
    AILogf("AI thoughts: %s\nAI answer:   PARSE FAIL (val=%.10s)", thoughts.c_str(), answer.c_str());
    return false;
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
    "Is this a good seed image for a whiteboard capture session? "
    "Answer YES only if ALL of these are true: "
    "(1) Only the lecturer and whiteboard are visible -- no students in the frame; "
    "(2) The camera is clearly zoomed in on the whiteboard; "
    "(3) At least 5 distinct strokes or writings are clearly visible on the whiteboard; "
    "(4) The whiteboard content is well-lit and readable. "
    "Answer NO if any condition is not met.";

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
    , stop_worker_(false) {
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
        SetStatusInternal(AICheckStatus::kIdle);
        AILog("AI checker now ENABLED (status=kIdle)");
    } else {
        SetStatusInternal(AICheckStatus::kDisabled);
        seed_approved_.store(false);
        dupe_pending_.store(false);
        dupe_passed_.store(false);
        last_seed_check_time_ = {};
        last_dupe_check_time_ = {};
        AILog("AI checker now DISABLED");
    }
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
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!queue_.empty()) { AILog("MaybeRequestSeedCheck: skip (queue not empty)"); return; }

        bool first = (last_seed_check_time_.time_since_epoch().count() == 0);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_seed_check_time_).count();
        if (!first && elapsed < kSeedCheckIntervalSec) {  // Throttle to at least 30s between seed checks
            AILogf("MaybeRequestSeedCheck: throttled (%lld s elapsed, need %d)",
                   (long long)elapsed, kSeedCheckIntervalSec);
            return;
        }

        CheckRequest req;
        req.kind  = CheckRequest::kSeed;
        req.frame = frame_bgr.clone();
        last_seed_check_time_ = now;
        queue_.push_back(std::move(req));
        AILogf("MaybeRequestSeedCheck: QUEUED seed check (first=%s, frame=%dx%d)",
               first ? "yes" : "no", frame_bgr.cols, frame_bgr.rows);
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

        bool first = (last_dupe_check_time_.time_since_epoch().count() == 0);
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_dupe_check_time_).count();
        if (!first && elapsed < kDupeCheckIntervalSec) {
            AILogf("MaybeRequestDuplicateCheck: throttled (%lld s elapsed, need %d)",
                   (long long)elapsed, kDupeCheckIntervalSec);
            return;
        }

        CheckRequest req;
        req.kind  = CheckRequest::kDuplicate;
        req.frame = frame_bgr.clone();
        for (const auto& ov : existing_overviews)
            req.existing_overviews.push_back(ov.clone());
        last_dupe_check_time_ = now;
        queue_.push_back(std::move(req));
        AILogf("MaybeRequestDuplicateCheck: QUEUED dupe check (overviews=%zu)",
               existing_overviews.size());
    }
    dupe_pending_.store(true);
    queue_cv_.notify_one();
}

void AICanvasChecker::TriggerLMSStart() {
    // LM Studio must be started and model loaded manually by the user.
    // This just resets status to kIdle so the worker retries the HTTP check.
    AILog("TriggerLMSStart: resetting to kIdle for retry");
    SetStatusInternal(AICheckStatus::kIdle);
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
    std::string content = "[" + ImageBlock(url) + "," + TextBlock(kSeedPrompt) + "]";
    auto body = BuildCompletion(content);
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
        "Answer YES if the content is already covered, NO if it is genuinely new.";

    std::string content = "[" + ImageBlock(MatToDataUrl(frame, 70, 160));
    for (const auto& ov : existing)
        content += "," + ImageBlock(MatToDataUrl(ov, 70, 64));
    content += "," + TextBlock(prompt) + "]";

    auto body = BuildCompletion(content);
    AILogf("DoDuplicateCheck: request body_len=%zu, posting...", body.size());
    auto resp = PostWithReloadRetry(body);
    AILogf("DoDuplicateCheck: response_len=%zu, first100=%.100s", resp.size(), resp.c_str());
    bool yes = false;
    if (!ExtractYesNo(resp, yes)) { AILog("DoDuplicateCheck: parse fail -> treat as NEW"); return true; }
    AILogf("DoDuplicateCheck: yes=%s -> returning %s", yes ? "true" : "false", !yes ? "NEW" : "DUPLICATE");
    return !yes;
}
