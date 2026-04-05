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
    void SyncSettings(bool debug_enabled,
                      bool duplicate_debug_enabled,
                      float absence_score_seen_threshold,
                      float enhance_threshold,
                      float yolo_fps);

    // Graph debug methods (read from shared memory written by helper process)
    int GetGraphNodeCount() const;
    int GetGraphNodes(float* buffer, int max_nodes) const;
    int GetGraphHardEdges(int* buffer, int max_edges) const;
    int GetGraphNodeContours(float* buffer, int max_floats) const;
    bool GetGraphCanvasBounds(int* bounds) const;
    bool CompareGraphNodes(int id_a, int id_b, float* result) const;
    int  GetGraphNodeMasks(uint8_t* buffer, int max_bytes) const;

    // User edit commands (routed through shared memory to helper process)
    int LockAllGraphNodes();
    bool ApplyUserEdits(const int* delete_ids, int delete_count,
                        const float* moves, int move_count);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void SetWhiteboardCanvasHelperProcessMode(bool helper_mode, const std::string& session_id);
bool IsWhiteboardCanvasHelperProcess();
int RunWhiteboardCanvasHelperMain(const std::string& session_id);