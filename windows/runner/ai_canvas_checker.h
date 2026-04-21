#pragma once
// ============================================================================
// ai_canvas_checker.h -- LM Studio vision-based canvas creation gate
//
// Runs a background worker thread that sends frames to a local LM Studio
// OpenAI-compatible API endpoint and decides:
//   (a) Whether the current frame is a good seed image (seed gate).
//   (b) Whether new subcanvas content is already covered by an existing one
//       (duplicate gate).
//
// All canvas processing continues normally while checks are in flight;
// only CreateSubCanvas calls are deferred until an approval arrives.
// ============================================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <opencv2/opencv.hpp>

enum class AICheckStatus : int {
    kDisabled      = 0,
    kIdle          = 1,
    kCheckingSeed  = 2,
    kCheckingDupe  = 3,
    kSeedApproved  = 4,
    kSeedRejected  = 5,
    kDupeNew       = 6,
    kDupeExists    = 7,
    kLmsOffline    = 8,
    kLmsStarting   = 9,
};

class AICanvasChecker {
public:
    AICanvasChecker();
    ~AICanvasChecker();

    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    // Seed gate -----------------------------------------------------------
    bool IsSeedApproved() const;
    void ClearSeedApproval();
    void MaybeRequestSeedCheck(const cv::Mat& frame_bgr);

    // Duplicate gate ------------------------------------------------------
    bool IsDuplicateCheckPending() const;
    bool IsDuplicateCheckPassed() const;
    void ResetDuplicateCheck();
    void MaybeRequestDuplicateCheck(const cv::Mat& frame_bgr,
                                    const std::vector<cv::Mat>& existing_overviews);

    // LM Studio lifecycle -------------------------------------------------
    void TriggerLMSStart();
    AICheckStatus GetStatus() const;

private:
    struct CheckRequest {
        enum Kind { kSeed, kDuplicate } kind;
        cv::Mat frame;
        std::vector<cv::Mat> existing_overviews;
    };

    void WorkerLoop();
    bool DoSeedCheck(const cv::Mat& frame);
    bool DoDuplicateCheck(const cv::Mat& frame, const std::vector<cv::Mat>& existing);
    void SetStatusInternal(AICheckStatus s);

    std::atomic<bool> enabled_;
    std::atomic<int>  ai_status_;
    std::atomic<bool> seed_approved_;
    std::atomic<bool> dupe_pending_;
    std::atomic<bool> dupe_passed_;

    static constexpr int kSeedCheckIntervalSec = 30;
    static constexpr int kDupeCheckIntervalSec = 10;

    using Clock = std::chrono::steady_clock;
    Clock::time_point last_seed_check_time_;
    Clock::time_point last_dupe_check_time_;

    std::deque<CheckRequest> queue_;
    std::mutex               queue_mutex_;
    std::condition_variable  queue_cv_;
    std::atomic<bool>        stop_worker_;
    std::thread              worker_thread_;
};
