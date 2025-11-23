#pragma once

#include <flutter/texture_registrar.h>
#include <opencv2/opencv.hpp>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>

class NativeCamera {
public:
    NativeCamera(flutter::TextureRegistrar* texture_registrar);
    virtual ~NativeCamera();

    const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width, size_t height);

    int64_t GetTextureId() const { return texture_id_; }

    void Start();
    void Stop();
    void SwitchCamera();
    void SetResolution(int width, int height);
    void SetFilterSequence(int* filters, int count);
    
    void GetFrameData(uint8_t* buffer, int32_t size);
    int32_t GetFrameWidth();
    int32_t GetFrameHeight();

private:
    flutter::TextureRegistrar* texture_registrar_;
    int64_t texture_id_ = -1;
    std::unique_ptr<flutter::TextureVariant> texture_variant_;
    
    cv::VideoCapture capture_;
    std::thread capture_thread_;
    std::atomic<bool> is_running_ = false;
    std::mutex mutex_;
    
    cv::Mat current_frame_;
    std::vector<uint8_t> pixel_buffer_data_; 
    std::unique_ptr<FlutterDesktopPixelBuffer> flutter_pixel_buffer_;
    
    int filter_mode_ = 0; 
    std::vector<int> active_filters_;

    // Smart Obstacle Removal State
    cv::Ptr<cv::BackgroundSubtractor> back_sub_;
    cv::Mat accumulated_background_;
    bool reset_background_ = true;

    // Resolution settings
    int target_width_ = 1280;
    int target_height_ = 720;
    int camera_index_ = 0;

    void CaptureLoop();
    void ProcessFrame(cv::Mat& frame);
    
    // New Filter Implementations
    void ApplySmartWhiteboard(cv::Mat& frame);
    void ApplySmartObstacleRemoval(cv::Mat& frame);
};

void InitGlobalNativeCamera(flutter::TextureRegistrar* texture_registrar);
