#include "native_camera.h"
#include <iostream>
#include <cmath>

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

    // Initialize Background Subtractor
    back_sub_ = cv::createBackgroundSubtractorKNN();
    // Note: setHistory is not directly available on the base class pointer in some versions, 
    // but createBackgroundSubtractorKNN returns a Ptr to BackgroundSubtractorKNN which has it.
    // We cast it or just rely on default. Let's try to cast if needed, or just use default.
    // Default history is usually 500. Python code used 300.
    auto knn = std::dynamic_pointer_cast<cv::BackgroundSubtractorKNN>(back_sub_);
    if (knn) {
        knn->setHistory(300);
    }
}

NativeCamera::~NativeCamera() {
    Stop();
    texture_registrar_->UnregisterTexture(texture_id_);
}

void NativeCamera::Start() {
    if (is_running_) return;
    
    // Try opening with DirectShow first (often more stable for webcams)
    capture_.open(camera_index_, cv::CAP_DSHOW); 
    if (!capture_.isOpened()) {
        // Fallback to MSMF
        std::cout << "DirectShow failed, trying MSMF..." << std::endl;
        capture_.open(camera_index_, cv::CAP_MSMF);
    }

    if (!capture_.isOpened()) {
        std::cerr << "Failed to open camera " << camera_index_ << std::endl;
        return;
    }

    // Set resolution
    capture_.set(cv::CAP_PROP_FRAME_WIDTH, target_width_);
    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, target_height_);

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

void NativeCamera::SwitchCamera() {
    Stop();
    
    // Try next index
    camera_index_++;
    
    // Try to open
    capture_.open(camera_index_, cv::CAP_DSHOW);
    if (!capture_.isOpened()) {
        capture_.open(camera_index_, cv::CAP_MSMF);
    }
    
    if (!capture_.isOpened()) {
        // If failed, loop back to 0
        std::cout << "Camera " << camera_index_ << " not found, looping back to 0" << std::endl;
        camera_index_ = 0;
        capture_.open(camera_index_, cv::CAP_DSHOW);
        if (!capture_.isOpened()) {
            capture_.open(camera_index_, cv::CAP_MSMF);
        }
    }
    
    if (capture_.isOpened()) {
        capture_.set(cv::CAP_PROP_FRAME_WIDTH, target_width_);
        capture_.set(cv::CAP_PROP_FRAME_HEIGHT, target_height_);
        
        is_running_ = true;
        capture_thread_ = std::thread(&NativeCamera::CaptureLoop, this);
    } else {
        std::cerr << "Failed to open any camera" << std::endl;
    }
}

void NativeCamera::SetResolution(int width, int height) {
    target_width_ = width;
    target_height_ = height;
    
    if (capture_.isOpened()) {
        // If camera is already running, try to update on the fly
        // Note: Some backends might require a restart, but let's try setting first.
        // If the resolution change requires a restart, we might need to Stop() and Start() here.
        // For safety and consistency, let's restart the capture if it's running.
        bool was_running = is_running_;
        if (was_running) {
            Stop();
            Start();
        } else {
             capture_.set(cv::CAP_PROP_FRAME_WIDTH, target_width_);
             capture_.set(cv::CAP_PROP_FRAME_HEIGHT, target_height_);
        }
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
            // Whiteboard (Legacy)
            cv::Mat gray;
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            cv::adaptiveThreshold(gray, gray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, 10);
            cv::cvtColor(gray, frame, cv::COLOR_GRAY2BGR);
        } else if (mode == 3) {
            // Obstacle / Blur (Legacy)
            cv::GaussianBlur(frame, frame, cv::Size(15, 15), 0);
        } else if (mode == 4) {
            // Smart Whiteboard
            ApplySmartWhiteboard(frame);
        } else if (mode == 5) {
            // Smart Obstacle Removal
            ApplySmartObstacleRemoval(frame);
        } else if (mode == 6) {
            // Moving Average
            ApplyMovingAverage(frame);
        } else if (mode == 7) {
            // CLAHE
            ApplyCLAHE(frame);
        } else if (mode == 8) {
            // Sharpening
            ApplySharpening(frame);
        }
    }
}

void NativeCamera::ApplyCLAHE(cv::Mat& frame) {
    // Convert to LAB color space
    cv::Mat lab_image;
    cv::cvtColor(frame, lab_image, cv::COLOR_BGR2Lab);

    // Extract the L channel
    std::vector<cv::Mat> lab_planes(3);
    cv::split(lab_image, lab_planes);  // now we have the L image in lab_planes[0]

    // Apply the CLAHE algorithm to the L channel
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
    clahe->setClipLimit(4);
    clahe->apply(lab_planes[0], lab_planes[0]);

    // Merge the color planes back into an Lab image
    cv::merge(lab_planes, lab_image);

    // Convert back to RGB
    cv::cvtColor(lab_image, frame, cv::COLOR_Lab2BGR);
}

void NativeCamera::ApplySharpening(cv::Mat& frame) {
    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(0, 0), 3);
    cv::addWeighted(frame, 1.5, blurred, -0.5, 0, frame);
}

void NativeCamera::ApplyMovingAverage(cv::Mat& frame) {
    // Add current frame to history
    // We need to clone because 'frame' is reused/modified
    frame_history_.push_back(frame.clone());

    // Maintain history size
    if (frame_history_.size() > history_size_) {
        frame_history_.pop_front();
    }

    // Calculate average
    if (frame_history_.empty()) return;

    cv::Mat sum = cv::Mat::zeros(frame.size(), CV_32FC3);
    
    for (const auto& f : frame_history_) {
        cv::Mat float_f;
        f.convertTo(float_f, CV_32F);
        cv::accumulate(float_f, sum);
    }

    // Divide by count
    sum = sum / static_cast<double>(frame_history_.size());

    // Convert back to 8-bit
    sum.convertTo(frame, CV_8U);
}

void NativeCamera::ApplySmartWhiteboard(cv::Mat& frame) {
    // 1. Create blurred version (Median Blur requires 8-bit input)
    cv::Mat blurred_8u;
    cv::medianBlur(frame, blurred_8u, 7);

    // 2. Convert to float for division
    cv::Mat float_frame;
    frame.convertTo(float_frame, CV_32F);

    cv::Mat blurred;
    blurred_8u.convertTo(blurred, CV_32F);
    cv::GaussianBlur(blurred, blurred, cv::Size(3, 3), 0);

    // 3. Normalize: image / blurred
    // Avoid division by zero by adding a small epsilon or ensuring blurred is not 0
    // But for simplicity, we trust the image content.
    cv::Mat normalized;
    cv::divide(float_frame, blurred, normalized);
    
    // 4. Minimum with 1.0
    cv::min(normalized, 1.0f, normalized);

    // 5. Enhance: 0.5 - 0.5 * cos(normalized^5 * pi)
    cv::pow(normalized, 5, normalized);
    normalized = normalized * CV_PI;
    
    // Calculate Cosine
    cv::Mat cos_val = normalized.clone();
    int rows = cos_val.rows;
    int cols = cos_val.cols * cos_val.channels();
    if (cos_val.isContinuous()) {
        cols *= rows;
        rows = 1;
    }
    for (int i = 0; i < rows; ++i) {
        float* ptr = cos_val.ptr<float>(i);
        for (int j = 0; j < cols; ++j) {
            ptr[j] = std::cos(ptr[j]);
        }
    }
    
    cv::Mat enhanced = 0.5f - 0.5f * cos_val;

    // 6. Scale back to 0-255
    enhanced = enhanced * 255.0f;
    
    // 7. Convert back to 8-bit
    enhanced.convertTo(frame, CV_8U);
}

void NativeCamera::ApplySmartObstacleRemoval(cv::Mat& frame) {
    // Initialize accumulated background if needed
    if (accumulated_background_.empty() || accumulated_background_.size() != frame.size()) {
        frame.copyTo(accumulated_background_);
    }

    // 1. Resize for mask calculation (Scale 0.1)
    cv::Mat small_frame;
    cv::resize(frame, small_frame, cv::Size(), 0.1, 0.1);

    // 2. Apply Background Subtractor
    cv::Mat fgmask;
    back_sub_->apply(small_frame, fgmask);

    // 3. Analyze columns (NUMBER_OF_PARTS = 15)
    int num_parts = 15;
    int w = frame.cols;
    int mask_w = fgmask.cols;
    
    std::vector<bool> is_static(num_parts, false);
    
    // Calculate column boundaries for the mask
    std::vector<int> mask_dist(num_parts + 1);
    double step_mask = static_cast<double>(mask_w) / num_parts;
    for(int i=0; i<=num_parts; ++i) mask_dist[i] = static_cast<int>(i * step_mask);

    // Check sums
    for (int i = 0; i < num_parts; ++i) {
        int start = mask_dist[i];
        int end = mask_dist[i+1];
        if (start >= end) continue;
        
        cv::Mat roi = fgmask.colRange(start, end);
        int sum = cv::countNonZero(roi);
        is_static[i] = (sum == 0);
    }

    // 4. Create update mask for the full image
    cv::Mat update_mask = cv::Mat::zeros(frame.size(), CV_8UC1);
    
    std::vector<int> dist(num_parts + 1);
    double step = static_cast<double>(w) / num_parts;
    for(int i=0; i<=num_parts; ++i) dist[i] = static_cast<int>(i * step);

    for (int i = 0; i < num_parts; ++i) {
        bool should_update = false;
        if (i == 0) {
            should_update = is_static[i] && is_static[i + 1];
        } else if (i == num_parts - 1) {
            should_update = is_static[i] && is_static[i - 1];
        } else {
            should_update = is_static[i] && is_static[i - 1] && is_static[i + 1];
        }

        if (should_update) {
            int start = dist[i];
            int end = dist[i+1];
            if (start < end) {
                // Set this region in the mask to 255 (update)
                update_mask.colRange(start, end).setTo(255);
            }
        }
    }

    // 5. Update accumulated background
    // accumulated_background = np.where(mask, image, accumulated_background)
    // In C++: frame.copyTo(accumulated_background, update_mask)
    frame.copyTo(accumulated_background_, update_mask);

    // 6. Output is the accumulated background
    accumulated_background_.copyTo(frame);
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

// Helper functions for external processing (WebRTC stream)
// These are duplicated from NativeCamera to allow stateless/independent processing

static cv::Ptr<cv::BackgroundSubtractor> g_back_sub;
static cv::Mat g_accumulated_background;
static std::deque<cv::Mat> g_frame_history;
static const size_t g_history_size = 5;

void ExternalApplyCLAHE(cv::Mat& frame) {
    cv::Mat lab_image;
    cv::cvtColor(frame, lab_image, cv::COLOR_BGR2Lab);
    std::vector<cv::Mat> lab_planes(3);
    cv::split(lab_image, lab_planes);
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
    clahe->setClipLimit(4);
    clahe->apply(lab_planes[0], lab_planes[0]);
    cv::merge(lab_planes, lab_image);
    cv::cvtColor(lab_image, frame, cv::COLOR_Lab2BGR);
}

void ExternalApplySharpening(cv::Mat& frame) {
    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(0, 0), 3);
    cv::addWeighted(frame, 1.5, blurred, -0.5, 0, frame);
}

void ExternalApplyMovingAverage(cv::Mat& frame) {
    g_frame_history.push_back(frame.clone());
    if (g_frame_history.size() > g_history_size) {
        g_frame_history.pop_front();
    }
    if (g_frame_history.empty()) return;

    cv::Mat sum = cv::Mat::zeros(frame.size(), CV_32FC3);
    for (const auto& f : g_frame_history) {
        cv::Mat float_f;
        f.convertTo(float_f, CV_32F);
        cv::accumulate(float_f, sum);
    }
    sum = sum / static_cast<double>(g_frame_history.size());
    sum.convertTo(frame, CV_8U);
}

void ExternalApplySmartWhiteboard(cv::Mat& frame) {
    cv::Mat blurred_8u;
    cv::medianBlur(frame, blurred_8u, 7);
    cv::Mat float_frame;
    frame.convertTo(float_frame, CV_32F);
    cv::Mat blurred;
    blurred_8u.convertTo(blurred, CV_32F);
    cv::GaussianBlur(blurred, blurred, cv::Size(3, 3), 0);
    cv::Mat normalized;
    cv::divide(float_frame, blurred, normalized);
    cv::min(normalized, 1.0f, normalized);
    cv::pow(normalized, 5, normalized);
    normalized = normalized * CV_PI;
    
    cv::Mat cos_val = normalized.clone();
    int rows = cos_val.rows;
    int cols = cos_val.cols * cos_val.channels();
    if (cos_val.isContinuous()) {
        cols *= rows;
        rows = 1;
    }
    for (int i = 0; i < rows; ++i) {
        float* ptr = cos_val.ptr<float>(i);
        for (int j = 0; j < cols; ++j) {
            ptr[j] = std::cos(ptr[j]);
        }
    }
    
    cv::Mat enhanced = 0.5f - 0.5f * cos_val;
    enhanced = enhanced * 255.0f;
    enhanced.convertTo(frame, CV_8U);
}

void ExternalApplySmartObstacleRemoval(cv::Mat& frame) {
    if (g_back_sub.empty()) {
        g_back_sub = cv::createBackgroundSubtractorKNN();
        auto knn = std::dynamic_pointer_cast<cv::BackgroundSubtractorKNN>(g_back_sub);
        if (knn) knn->setHistory(300);
    }

    if (g_accumulated_background.empty() || g_accumulated_background.size() != frame.size()) {
        frame.copyTo(g_accumulated_background);
    }

    cv::Mat small_frame;
    cv::resize(frame, small_frame, cv::Size(), 0.1, 0.1);
    cv::Mat fgmask;
    g_back_sub->apply(small_frame, fgmask);

    int num_parts = 15;
    int w = frame.cols;
    int mask_w = fgmask.cols;
    std::vector<bool> is_static(num_parts, false);
    std::vector<int> mask_dist(num_parts + 1);
    double step_mask = static_cast<double>(mask_w) / num_parts;
    for(int i=0; i<=num_parts; ++i) mask_dist[i] = static_cast<int>(i * step_mask);

    for (int i = 0; i < num_parts; ++i) {
        int start = mask_dist[i];
        int end = mask_dist[i+1];
        if (start >= end) continue;
        cv::Mat roi = fgmask.colRange(start, end);
        int sum = cv::countNonZero(roi);
        is_static[i] = (sum == 0);
    }

    cv::Mat update_mask = cv::Mat::zeros(frame.size(), CV_8UC1);
    std::vector<int> dist(num_parts + 1);
    double step = static_cast<double>(w) / num_parts;
    for(int i=0; i<=num_parts; ++i) dist[i] = static_cast<int>(i * step);

    for (int i = 0; i < num_parts; ++i) {
        bool should_update = false;
        if (i == 0) should_update = is_static[i] && is_static[i + 1];
        else if (i == num_parts - 1) should_update = is_static[i] && is_static[i - 1];
        else should_update = is_static[i] && is_static[i - 1] && is_static[i + 1];

        if (should_update) {
            int start = dist[i];
            int end = dist[i+1];
            if (start < end) update_mask.colRange(start, end).setTo(255);
        }
    }

    frame.copyTo(g_accumulated_background, update_mask);
    g_accumulated_background.copyTo(frame);
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

    __declspec(dllexport) void SwitchCamera() {
        if (g_native_camera) g_native_camera->SwitchCamera();
    }

    __declspec(dllexport) void SetResolution(int width, int height) {
        if (g_native_camera) g_native_camera->SetResolution(width, height);
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

    __declspec(dllexport) void process_frame(uint8_t* bytes, int32_t width, int32_t height, int32_t mode) {
        if (bytes == nullptr || width <= 0 || height <= 0) return;

        // Wrap bytes in Mat. Assumes BGRA (4 channels)
        cv::Mat frame(height, width, CV_8UC4, bytes);

        // Convert to BGR for processing
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);

        if (mode == 1) { // Invert
            cv::bitwise_not(bgr, bgr);
        } else if (mode == 2) { // Whiteboard Legacy
            cv::Mat gray;
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::adaptiveThreshold(gray, gray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, 10);
            cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
        } else if (mode == 3) { // Blur Legacy
            cv::GaussianBlur(bgr, bgr, cv::Size(15, 15), 0);
        } else if (mode == 4) { // Smart Whiteboard
            ExternalApplySmartWhiteboard(bgr);
        } else if (mode == 5) { // Smart Obstacle
            ExternalApplySmartObstacleRemoval(bgr);
        } else if (mode == 6) { // Moving Average
            ExternalApplyMovingAverage(bgr);
        } else if (mode == 7) { // CLAHE
            ExternalApplyCLAHE(bgr);
        } else if (mode == 8) { // Sharpening
            ExternalApplySharpening(bgr);
        }

        // Convert back to BGRA and write to original buffer
        cv::cvtColor(bgr, frame, cv::COLOR_BGR2BGRA);
    }
}
