#pragma once

#include <opencv2/opencv.hpp>
#include <memory>
#include <string>

class WhiteboardCanvas;
enum class CanvasRenderMode : int;

class WhiteboardCanvasHelperClient {
public:
    WhiteboardCanvasHelperClient();
    ~WhiteboardCanvasHelperClient();

    bool Start();
    void Stop();

    bool IsReady() const;
    void ProcessFrame(const cv::Mat& frame, const cv::Mat& person_mask);
    bool GetViewport(float panX, float panY, float zoom,
                     cv::Size viewSize, cv::Mat& out_frame);
    bool GetOverview(cv::Size viewSize, cv::Mat& out_frame);
    void Reset();
    bool HasContent() const;
    bool IsCanvasViewMode() const;
    void SetCanvasViewMode(bool mode);
    void SetRenderMode(CanvasRenderMode mode);
    CanvasRenderMode GetRenderMode() const;
    cv::Size GetCanvasSize() const;
    int GetSubCanvasCount() const;
    int GetActiveSubCanvasIndex() const;
    void SetActiveSubCanvas(int idx);
    int GetSortedSubCanvasIndex(int pos) const;
    int GetSortedPosition(int idx) const;
    void SyncSettings(bool debug_enabled, float enhance_threshold, float yolo_fps);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void SetWhiteboardCanvasHelperProcessMode(bool helper_mode, const std::string& session_id);
bool IsWhiteboardCanvasHelperProcess();
int RunWhiteboardCanvasHelperMain(const std::string& session_id);