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
    void StartProcessingOnly(); // For screen capture: start processing thread without camera
    void SwitchCamera();
    void SelectCamera(int index);
    void SetResolution(int width, int height);
    void SetFilterSequence(int* filters, int count);
    
    void GetFrameData(uint8_t* buffer, int32_t size);
    int32_t GetFrameWidth();
    int32_t GetFrameHeight();

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
    
    cv::Mat current_frame_;
    std::vector<uint8_t> pixel_buffer_data_; 
    std::unique_ptr<FlutterDesktopPixelBuffer> flutter_pixel_buffer_;
    
    std::vector<int> active_filters_;

    // Resolution settings
    int target_width_ = 4096;
    int target_height_ = 2160;
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

void InitGlobalNativeCamera(flutter::TextureRegistrar* texture_registrar);
