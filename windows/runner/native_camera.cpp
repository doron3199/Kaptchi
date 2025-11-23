#include "native_camera.h"
#include <iostream>

// Global instance pointer
NativeCamera* g_native_camera = nullptr;

void InitGlobalNativeCamera(flutter::TextureRegistrar* texture_registrar) {
    if (g_native_camera) return;
    g_native_camera = new NativeCamera(texture_registrar);
}

NativeCamera::NativeCamera(flutter::TextureRegistrar* texture_registrar)
    : texture_registrar_(texture_registrar) {
    
    texture_variant_ = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([this](size_t width, size_t height) -> const FlutterDesktopPixelBuffer* {
            return this->CopyPixelBuffer(width, height);
        }));

    texture_id_ = texture_registrar_->RegisterTexture(texture_variant_.get());
    flutter_pixel_buffer_ = std::make_unique<FlutterDesktopPixelBuffer>();
    flutter_pixel_buffer_->width = 0;
    flutter_pixel_buffer_->height = 0;
    flutter_pixel_buffer_->buffer = nullptr;
    flutter_pixel_buffer_->release_callback = nullptr;
    flutter_pixel_buffer_->release_context = nullptr;
}

NativeCamera::~NativeCamera() {
    Stop();
    texture_registrar_->UnregisterTexture(texture_id_);
}

void NativeCamera::Start() {
    if (is_running_) return;
    
    // Try opening with DirectShow first (often more stable for webcams)
    capture_.open(0, cv::CAP_DSHOW); 
    if (!capture_.isOpened()) {
        // Fallback to MSMF
        std::cout << "DirectShow failed, trying MSMF..." << std::endl;
        capture_.open(0, cv::CAP_MSMF);
    }

    if (!capture_.isOpened()) {
        std::cerr << "Failed to open camera" << std::endl;
        return;
    }

    // Set resolution to 1280x720
    capture_.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, 720);

    is_running_ = true;
    capture_thread_ = std::thread(&NativeCamera::CaptureLoop, this);
}

void NativeCamera::Stop() {
    is_running_ = false;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (capture_.isOpened()) {
        capture_.release();
    }
}

void NativeCamera::SetFilterSequence(int* filters, int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_filters_.clear();
    for (int i = 0; i < count; i++) {
        active_filters_.push_back(filters[i]);
    }
}

void NativeCamera::GetFrameData(uint8_t* buffer, int32_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_frame_.empty()) return;
    
    int32_t expected_size = static_cast<int32_t>(current_frame_.total() * 4);
    if (size < expected_size) return;
    
    memcpy(buffer, current_frame_.data, expected_size);
}

int32_t NativeCamera::GetFrameWidth() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_frame_.empty() ? 0 : current_frame_.cols;
}

int32_t NativeCamera::GetFrameHeight() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_frame_.empty() ? 0 : current_frame_.rows;
}

void NativeCamera::CaptureLoop() {
    while (is_running_) {
        cv::Mat frame;
        if (capture_.read(frame)) {
            if (!frame.empty()) {
                ProcessFrame(frame);

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Convert BGR to RGBA (Flutter expects RGBA on Windows)
                    cv::cvtColor(frame, current_frame_, cv::COLOR_BGR2RGBA);
                }
                
                // Notify Flutter that a new frame is available
                texture_registrar_->MarkTextureFrameAvailable(texture_id_);
            }
        }
    }
}

void NativeCamera::ProcessFrame(cv::Mat& frame) {
    std::vector<int> filters_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        filters_copy = active_filters_;
    }

    for (int mode : filters_copy) {
        if (mode == 1) {
            // Invert
            cv::bitwise_not(frame, frame);
        } else if (mode == 2) {
            // Whiteboard
            cv::Mat gray;
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            cv::adaptiveThreshold(gray, gray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, 10);
            cv::cvtColor(gray, frame, cv::COLOR_GRAY2BGR);
        } else if (mode == 3) {
            // Obstacle / Blur
            cv::GaussianBlur(frame, frame, cv::Size(15, 15), 0);
        }
    }
}

const FlutterDesktopPixelBuffer* NativeCamera::CopyPixelBuffer(size_t width, size_t height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_frame_.empty()) {
        return nullptr;
    }

    if (pixel_buffer_data_.size() != current_frame_.total() * 4) {
        pixel_buffer_data_.resize(current_frame_.total() * 4);
    }
    
    memcpy(pixel_buffer_data_.data(), current_frame_.data, pixel_buffer_data_.size());

    flutter_pixel_buffer_->buffer = pixel_buffer_data_.data();
    flutter_pixel_buffer_->width = current_frame_.cols;
    flutter_pixel_buffer_->height = current_frame_.rows;
    
    return flutter_pixel_buffer_.get();
}

// FFI Exports
extern "C" {
    __declspec(dllexport) int64_t GetTextureId() {
        if (!g_native_camera) return -1;
        return g_native_camera->GetTextureId();
    }

    __declspec(dllexport) void StartCamera() {
        if (g_native_camera) g_native_camera->Start();
    }

    __declspec(dllexport) void StopCamera() {
        if (g_native_camera) g_native_camera->Stop();
    }

    __declspec(dllexport) void SetFilterSequence(int* filters, int count) {
        if (g_native_camera) g_native_camera->SetFilterSequence(filters, count);
    }

    __declspec(dllexport) void GetFrameData(uint8_t* buffer, int32_t size) {
        if (g_native_camera) g_native_camera->GetFrameData(buffer, size);
    }

    __declspec(dllexport) int32_t GetFrameWidth() {
        if (!g_native_camera) return 0;
        return g_native_camera->GetFrameWidth();
    }

    __declspec(dllexport) int32_t GetFrameHeight() {
        if (!g_native_camera) return 0;
        return g_native_camera->GetFrameHeight();
    }
}
