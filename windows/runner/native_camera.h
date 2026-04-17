#pragma once

#include <flutter/texture_registrar.h>
#include <opencv2/opencv.hpp>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <deque>

class NativeCamera {
public:
    NativeCamera(flutter::TextureRegistrar* texture_registrar);
    virtual ~NativeCamera();

    const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width, size_t height);

    int64_t GetTextureId() const { return texture_id_; }

    void Start();
    void StartStream(const char* url);
    void Stop();
    void SetVideoSkipFrames(int skip) { video_skip_frames_ = std::max(0, skip); }
    int  GetVideoSkipFrames() const { return video_skip_frames_.load(); }
    bool IsVideoComplete() const { return video_complete_.load(); }
    float GetVideoProgress() const { return video_progress_.load(); }
    // Request the capture thread to seek to a 0..1 position in the video file.
    void SeekVideoToProgress(float progress) {
        if (progress < 0.f) progress = 0.f;
        if (progress > 1.f) progress = 1.f;
        pending_seek_progress_.store(progress);
    }
    void StartProcessingOnly(); // For screen capture: start processing thread without camera
    void SwitchCamera();
    void SelectCamera(int index);
    void SetResolution(int width, int height);
    void SetFilterSequence(int* filters, int count);
    
    void GetFrameData(uint8_t* buffer, int32_t size);
    int32_t GetFrameWidth();
    int32_t GetFrameHeight();
    void RefreshDisplayFrame();
    bool CopyLatestWhiteboardInput(cv::Mat& frame_bgr, cv::Mat& person_mask);

    uint64_t GetDisplayFrameId() const {
        return display_frame_id_.load(std::memory_order_relaxed);
    }
    bool GetFrameDataJpeg(uint8_t* buffer, int max_bytes, int* out_size, int quality);

    // External frame input (for screen capture, etc.)
    void PushExternalFrame(const cv::Mat& frame);

private:
    flutter::TextureRegistrar* texture_registrar_;
    int64_t texture_id_ = -1;
    std::unique_ptr<flutter::TextureVariant> texture_variant_;
    
    cv::VideoCapture capture_;
    std::thread capture_thread_;
    std::atomic<bool> is_running_ = false;
    std::atomic<bool> is_stream_ = false;
    std::string stream_url_;
    std::atomic<bool> restart_requested_ = false;
    std::atomic<int> pending_camera_index_ = 0;
    std::mutex mutex_;

    // Video file playback state
    std::atomic<bool> is_video_file_ = false;
    std::atomic<int> video_skip_frames_ = 0;
    double video_fps_ = 0.0;
    std::condition_variable frame_consumed_cv_; // signalled when processing thread takes a frame
    std::atomic<bool> video_complete_ = false;
    std::atomic<float> video_progress_ = 0.0f;
    double video_total_frames_ = 0.0;
    // -1.0f means "no seek pending"; otherwise a 0..1 target position
    // that the capture thread will consume on the next loop iteration.
    std::atomic<float> pending_seek_progress_ = -1.0f;
    
    cv::Mat current_frame_;
    cv::Mat last_source_frame_bgr_;
    cv::Mat last_whiteboard_input_frame_bgr_;
    cv::Mat last_person_mask_;
    std::vector<uint8_t> pixel_buffer_data_; 
    std::unique_ptr<FlutterDesktopPixelBuffer> flutter_pixel_buffer_;
    
    std::vector<int> active_filters_;

    std::atomic<uint64_t> display_frame_id_ = {0};

    // Resolution settings
    int target_width_ = 1920;
    int target_height_ = 1080;
    int camera_index_ = 1;

    void CameraThreadLoop();
    void ProcessFrame(cv::Mat& frame);

    // Async processing members
    std::thread processing_thread_;
    std::mutex processing_mutex_;
    std::condition_variable processing_cv_;
    cv::Mat pending_frame_;
    std::atomic<bool> has_new_frame_ = false;
    void ProcessingThreadLoop();
};

extern NativeCamera* g_native_camera;

void InitGlobalNativeCamera(flutter::TextureRegistrar* texture_registrar);
void ShutdownGlobalNativeCamera();
cv::Mat GetWhiteboardPersonMask(const cv::Mat& frame);
