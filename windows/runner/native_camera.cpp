#include "native_camera.h"
#include "screen_capture_source.h"
#include <iostream>
#include <cmath>
#include <chrono>
#include <objbase.h>
#include <opencv2/dnn.hpp>
#include <windows.h> // For GetModuleFileName

// Forward declaration
void ApplyFilterSequenceInternal(cv::Mat& bgr, int32_t* modes, int32_t count);

// --- Global State for Filters ---
static cv::Mat g_prev_gray_stab;
static cv::Point2f g_shaking_offset(0,0);
static float g_avg_brightness = -1.0f;

// Person Removal Smooth Mask
static cv::Mat g_person_prob_mask;

// --- Filter Parameters ---
#include <unordered_map>
static std::unordered_map<int, float> g_filter_params;
static std::mutex g_filter_params_mutex;


// Helper to get absolute path to models
std::string GetModelPath(const std::string& modelName) {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string::size_type pos = std::string(buffer).find_last_of("\\/");
    std::string dir = std::string(buffer).substr(0, pos);
    std::string fullPath = dir + "\\models\\" + modelName;
    
    std::cout << "[NativeCamera] Executable Dir: " << dir << std::endl;
    std::cout << "[NativeCamera] Looking for model at: " << fullPath << std::endl;
    
    return fullPath;
}

// Global instance pointer
NativeCamera* g_native_camera = nullptr;
ScreenCaptureSource* g_screen_capture = nullptr;

// Forward declarations
static void ApplyLivePerspectiveCrop(cv::Mat& frame);

void InitGlobalNativeCamera(flutter::TextureRegistrar* texture_registrar) {
    if (g_native_camera) return;
    g_native_camera = new NativeCamera(texture_registrar);
    
    // Initialize screen capture source
    if (!g_screen_capture) {
        g_screen_capture = new ScreenCaptureSource();
        g_screen_capture->Init(g_native_camera);
    }
}

NativeCamera::NativeCamera(flutter::TextureRegistrar* texture_registrar)
    : texture_registrar_(texture_registrar) {
    
    texture_variant_ = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([this](size_t width, size_t height) -> const FlutterDesktopPixelBuffer* {
            return this->CopyPixelBuffer(width, height);
        }));

    texture_id_ = texture_registrar_->RegisterTexture(texture_variant_.get());
    std::cout << "NativeCamera initialized. Texture ID: " << texture_id_ << std::endl;
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
    if (is_running_ && !is_stream_) return;

    // Stop screen capture if running to prevent conflicts
    if (g_screen_capture && g_screen_capture->IsCapturing()) {
        g_screen_capture->StopCapture();
    }

    // Clear any stale frame from previous sessions to avoid ghosting
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (target_width_ > 0 && target_height_ > 0) {
             // Create a black frame (RGBA)
             current_frame_ = cv::Mat(target_height_, target_width_, CV_8UC4, cv::Scalar(0, 0, 0, 255));
        } else {
             current_frame_ = cv::Mat();
        }
    }
    // Notify Flutter to redraw (with the black frame)
    if (texture_id_ != -1) {
        texture_registrar_->MarkTextureFrameAvailable(texture_id_);
    }

    is_running_ = true;
    is_stream_ = false;
    restart_requested_ = false;
    capture_thread_ = std::thread(&NativeCamera::CameraThreadLoop, this);
    processing_thread_ = std::thread(&NativeCamera::ProcessingThreadLoop, this);
}

void NativeCamera::StartStream(const char* url) {
    if (is_running_ && is_stream_ && stream_url_ == url) return;

    std::cout << "StartStream requested. URL: " << url << std::endl;

    Stop(); // Stop existing if any

    stream_url_ = url;
    is_running_ = true;
    is_stream_ = true;
    restart_requested_ = false;
    capture_thread_ = std::thread(&NativeCamera::CameraThreadLoop, this);
    processing_thread_ = std::thread(&NativeCamera::ProcessingThreadLoop, this);
}

void NativeCamera::Stop() {
    is_running_ = false;
    
    // Wake up processing thread so it can exit
    {
        std::lock_guard<std::mutex> lock(processing_mutex_);
        has_new_frame_ = true; // Force wake
    }
    processing_cv_.notify_all();

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
    // capture_.release() is now handled inside the thread loop
}

void NativeCamera::StartProcessingOnly() {
    // If already running with processing, nothing to do
    if (is_running_) return;
    
    // Start only the processing thread (no camera capture thread)
    // This is used for screen capture, which pushes frames via PushExternalFrame()
    is_running_ = true;
    is_stream_ = false;
    processing_thread_ = std::thread(&NativeCamera::ProcessingThreadLoop, this);
    std::cout << "[NativeCamera] Started processing thread only (for screen capture)" << std::endl;
}

void NativeCamera::SwitchCamera() {
    if (is_running_) {
        pending_camera_index_ = camera_index_ + 1;
        restart_requested_ = true;
    } else {
        camera_index_++;
        Start();
    }
}

void NativeCamera::SelectCamera(int index) {
    // Swap indices 0 and 1 to match Flutter's enumeration order with OpenCV's on Windows.
    // Flutter often lists Integrated as 0, External as 1.
    // OpenCV often lists External as 0, Integrated as 1 (or vice versa depending on driver load order).
    // Based on user report, they are swapped.
    int mapped_index = index;
    if (index == 0) mapped_index = 1;
    else if (index == 1) mapped_index = 0;

    if (camera_index_ == mapped_index && is_running_) return;
    
    if (is_running_) {
        pending_camera_index_ = mapped_index;
        restart_requested_ = true;
    } else {
        camera_index_ = mapped_index;
        Start();
    }
}

void NativeCamera::SetResolution(int width, int height) {
    target_width_ = width;
    target_height_ = height;
    
    if (is_running_) {
        pending_camera_index_ = camera_index_;
        restart_requested_ = true;
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

void NativeCamera::PushExternalFrame(const cv::Mat& frame) {
    if (frame.empty()) return;
    
    // Queue frame for async processing
    {
        std::lock_guard<std::mutex> lock(processing_mutex_);
        frame.copyTo(pending_frame_);
        has_new_frame_ = true;
    }
    processing_cv_.notify_one();
}

void NativeCamera::CameraThreadLoop() {
    // Initialize COM for this thread (Required for MSMF/DirectShow)
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    bool needs_open = true;

    while (is_running_) {
        if (restart_requested_) {
            if (capture_.isOpened()) {
                capture_.release();
            }

            // Clear the frame to black to avoid showing the previous camera's image
            // while the new camera is initializing.
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Create a black frame (RGBA)
                current_frame_ = cv::Mat(target_height_, target_width_, CV_8UC4, cv::Scalar(0, 0, 0, 255));
            }
            texture_registrar_->MarkTextureFrameAvailable(texture_id_);

            camera_index_ = pending_camera_index_.load();
            restart_requested_ = false;
            needs_open = true;
        }

        if (needs_open) {
            needs_open = false;
            
            if (is_stream_) {
                std::cout << "Opening stream " << stream_url_ << "..." << std::endl;
                capture_.open(stream_url_);
                if (capture_.isOpened()) {
                    std::cout << "Stream opened successfully." << std::endl;
                } else {
                    std::cerr << "Failed to open stream: " << stream_url_ << std::endl;
                }
            } else {
                // Always prioritize DirectShow (DSHOW) for stability.
                // MSMF (Media Foundation) can cause freezes with some external cameras
                // and the enumeration order might differ from Flutter's.
                // DSHOW is generally more robust for hot-plugging and switching.
                
                std::cout << "Opening camera " << camera_index_ << " with DirectShow..." << std::endl;
                capture_.open(camera_index_, cv::CAP_DSHOW);

                if (!capture_.isOpened()) {
                    std::cout << "DirectShow failed for camera " << camera_index_ << ", trying MSMF..." << std::endl;
                    capture_.open(camera_index_, cv::CAP_MSMF); 
                }

                if (!capture_.isOpened()) {
                    // If failed, try looping back to 0 (legacy behavior for SwitchCamera)
                    if (camera_index_ > 0) {
                        std::cout << "Camera " << camera_index_ << " failed, trying 0..." << std::endl;
                        camera_index_ = 0;
                        capture_.open(camera_index_, cv::CAP_DSHOW);
                        if (!capture_.isOpened()) {
                            capture_.open(camera_index_, cv::CAP_MSMF);
                        }
                    }
                }

                if (capture_.isOpened()) {
                    // Set resolution
                    capture_.set(cv::CAP_PROP_FRAME_WIDTH, target_width_);
                    capture_.set(cv::CAP_PROP_FRAME_HEIGHT, target_height_);
                }
            }

            if (!capture_.isOpened()) {
                std::cerr << "Failed to open " << (is_stream_ ? "stream" : "camera") << std::endl;
                // Sleep to avoid busy loop if camera fails
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        if (!capture_.isOpened()) {
             std::this_thread::sleep_for(std::chrono::milliseconds(100));
             continue;
        }

        cv::Mat frame;
        if (capture_.read(frame)) {
            if (!frame.empty()) {
                // Rotate if vertical (portrait) to make it horizontal (landscape)
                if (is_stream_ && frame.rows > frame.cols) {
                    // std::cout << "Rotating frame..." << std::endl;
                    cv::Mat rotated;
                    cv::rotate(frame, rotated, cv::ROTATE_90_CLOCKWISE);
                    frame = rotated;
                }

                {
                    std::lock_guard<std::mutex> lock(processing_mutex_);
                    frame.copyTo(pending_frame_);
                    has_new_frame_ = true;
                }
                processing_cv_.notify_one();
            } else {
                 std::cerr << "Captured empty frame." << std::endl;
            }
        } else {
             // std::cerr << "Failed to read frame." << std::endl;
             std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    if (capture_.isOpened()) {
        capture_.release();
    }

    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
}

void NativeCamera::ProcessFrame(cv::Mat& frame) {
    // Always apply live perspective crop first (independent of filters)
    ApplyLivePerspectiveCrop(frame);

    std::vector<int> filters_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        filters_copy = active_filters_;
    }

    if (filters_copy.empty()) return;

    ApplyFilterSequenceInternal(frame, filters_copy.data(), static_cast<int32_t>(filters_copy.size()));
}

void NativeCamera::ProcessingThreadLoop() {
    while (is_running_) {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lock(processing_mutex_);
            processing_cv_.wait(lock, [this] { return has_new_frame_ || !is_running_; });
            
            if (!is_running_) break;
            
            if (pending_frame_.empty()) {
                has_new_frame_ = false;
                continue;
            }

            pending_frame_.copyTo(frame);
            has_new_frame_ = false;
        }

        ProcessFrame(frame);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Convert BGR to RGBA (Flutter expects RGBA on Windows)
            cv::cvtColor(frame, current_frame_, cv::COLOR_BGR2RGBA);
        }
        
        texture_registrar_->MarkTextureFrameAvailable(texture_id_);
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
    
    // Use cv::Mat to handle potential stride/padding issues during copy
    // This ensures we copy row-by-row correctly even if the source is not continuous
    cv::Mat wrapper(current_frame_.rows, current_frame_.cols, CV_8UC4, pixel_buffer_data_.data());
    current_frame_.copyTo(wrapper);

    flutter_pixel_buffer_->buffer = pixel_buffer_data_.data();
    flutter_pixel_buffer_->width = current_frame_.cols;
    flutter_pixel_buffer_->height = current_frame_.rows;
    
    return flutter_pixel_buffer_.get();
}

// Image Processing Helper Functions

static cv::Ptr<cv::BackgroundSubtractor> g_back_sub;
static cv::Mat g_accumulated_background;
static std::deque<cv::Mat> g_frame_history;
static const size_t g_history_size = 5;

static void ApplyCLAHE(cv::Mat& frame) {
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

static void ApplySharpening(cv::Mat& frame) {
    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(0, 0), 3);
    cv::addWeighted(frame, 1.5, blurred, -0.5, 0, frame);
}

static void ApplyMovingAverage(cv::Mat& frame) {
    // Clear history if frame size changed (e.g., camera switch)
    if (!g_frame_history.empty() && g_frame_history.front().size() != frame.size()) {
        g_frame_history.clear();
    }

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

static void ApplySmartWhiteboard(cv::Mat& frame) {
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

static void ApplySmartObstacleRemoval(cv::Mat& frame) {
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

// --- New Filters ---

static void ApplyStabilization(cv::Mat& frame) {
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    
    if (g_prev_gray_stab.empty() || g_prev_gray_stab.size() != gray.size()) {
        gray.copyTo(g_prev_gray_stab);
        return;
    }

    // Phase correlation to find translation
    cv::Mat window; // Hanning window to reduce edge effects
    cv::createHanningWindow(window, gray.size(), CV_32F);
    
    // 2. Prepare floating point images for phaseCorrelate (MUST be CV_32F or CV_64F)
    cv::Mat prev_32f, curr_32f;
    g_prev_gray_stab.convertTo(prev_32f, CV_32F);
    gray.convertTo(curr_32f, CV_32F);

    // 3. Estimate translation
    // phaseCorrelate returns the shift to align prev to curr (or curr to prev?)
    // actually it calculates shift from src1 to src2. 
    cv::Point2d shift = cv::phaseCorrelate(prev_32f, curr_32f, window);
    
    // Check confidence or max shift?
    // simple low-pass filter on the shift
    // We want to counteract the shift.
    // If camera moved (dx, dy), we want to shift image by (-dx, -dy) to keep it stable relative to previous.
    // However, we don't want to freeze panning. We only want to remove high freq jitter.
    
    // Current "absolute" position relative to stabilized view
    // smoothed_pos = alpha * (smoothed_pos + shift) + (1-alpha) * 0 ? 
    // Simplify: Just dampen the inter-frame movement
    
    double dx = shift.x;
    double dy = shift.y;
    
    // Ignore large shifts (intentional panning)
    if (fabs(dx) > 20 || fabs(dy) > 20) {
        dx = 0;
        dy = 0;
    }

    // Accumulate offset (with decay to re-center)
    g_shaking_offset.x = g_shaking_offset.x * 0.9f - (float)dx;
    g_shaking_offset.y = g_shaking_offset.y * 0.9f - (float)dy;
    
    // Build affine matrix
    cv::Mat M = (cv::Mat_<double>(2,3) << 1, 0, g_shaking_offset.x, 0, 1, g_shaking_offset.y);
    
    cv::Mat stabilized;
    cv::warpAffine(frame, stabilized, M, frame.size());
    
    stabilized.copyTo(frame);
    gray.copyTo(g_prev_gray_stab);
}

static void ApplyLightStabilization(cv::Mat& frame) {
    cv::Mat hsv;
    cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
    
    // Calculate mean brightness (V channel is index 2)
    // Faster: just subsample
    cv::Scalar mean = cv::mean(hsv);
    float current_v = (float)mean[2];
    
    if (g_avg_brightness < 0) {
        g_avg_brightness = current_v;
    } else {
        // Smooth changes
        g_avg_brightness = g_avg_brightness * 0.95f + current_v * 0.05f;
    }
    
    if (current_v > 1.0f) { // Avoid div by zero
        float gain = g_avg_brightness / current_v;
        // Clamp gain to avoid extreme noise
        if (gain < 0.8f) gain = 0.8f;
        if (gain > 1.2f) gain = 1.2f;
        
        // Apply gain to V channel
        // Splitting is slow, can we do it in place?
        // Iterate or use scaling
        // hsv is 3 channels. 
        // We can use convertScaleAbs on the V channel if we split.
        
        std::vector<cv::Mat> channels;
        cv::split(hsv, channels);
        channels[2] = channels[2] * gain; 
        cv::merge(channels, hsv);
        
        cv::cvtColor(hsv, frame, cv::COLOR_HSV2BGR);
    }
}

static void ApplyCornerSmoothing(cv::Mat& frame) {
    // Bilateral Filter is good for edge preserving smoothing
    cv::Mat temp;
    // d=5, sigmaColor=75, sigmaSpace=75 is standard
    // For "Corner Smoothing" (Text Readability), we might want stronger smoothing on flat areas?
    cv::bilateralFilter(frame, temp, 9, 75, 75);
    temp.copyTo(frame);
}

// --- Smart Video Crop State ---
// --- Smart Video Crop State ---
static cv::Mat g_prev_crop_small;      // Previous frame (downscaled)
static cv::Mat g_motion_energy_small;  // Motion energy (downscaled)
static float g_crop_top_target = 0.0f;     // Normalized (0.0 - 1.0)
static float g_crop_bottom_target = 1.0f;  // Normalized (0.0 - 1.0)
static float g_crop_top_current = 0.0f;    // Normalized (0.0 - 1.0)
static float g_crop_bottom_current = 1.0f; // Normalized (0.0 - 1.0)

// Config
static const int kAnalysisWidth = 256; // Low resolution for fast analysis

static void ApplySmartVideoCrop(cv::Mat& frame) {
    if (frame.empty()) return;

    // 1. Downscale for Analysis
    float aspect = (float)frame.rows / (float)frame.cols;
    int analysis_height = (int)(kAnalysisWidth * aspect);
    if (analysis_height < 10) analysis_height = 10; // Safety

    cv::Mat small_frame;
    cv::resize(frame, small_frame, cv::Size(kAnalysisWidth, analysis_height), 0, 0, cv::INTER_LINEAR);

    cv::Mat small_gray;
    cv::cvtColor(small_frame, small_gray, cv::COLOR_BGR2GRAY);
    
    // 2. Initialize or Reset if size changed (or first run)
    if (g_prev_crop_small.empty() || g_prev_crop_small.size() != small_gray.size()) {
        small_gray.copyTo(g_prev_crop_small);
        g_motion_energy_small = cv::Mat::zeros(small_gray.size(), CV_32F);
        g_crop_top_target = 0.0f;
        g_crop_bottom_target = 1.0f;
        g_crop_top_current = 0.0f;
        g_crop_bottom_current = 1.0f;
        return;
    }

    // 3. Compute Frame Difference (Motion) on Small Frame
    cv::Mat diff;
    cv::absdiff(small_gray, g_prev_crop_small, diff);
    small_gray.copyTo(g_prev_crop_small);

    cv::Mat diff_float;
    diff.convertTo(diff_float, CV_32F, 1.0/255.0);

    // Threshold noise
    cv::threshold(diff_float, diff_float, 0.05, 1.0, cv::THRESH_TOZERO);

    // 4. Update Motion Energy
    // Accumulate: energy = energy * decay + diff
    cv::addWeighted(g_motion_energy_small, 0.95, diff_float, 1.0, 0, g_motion_energy_small);
    
    // 5. Analyze Row Energy
    cv::Mat row_sums;
    cv::reduce(g_motion_energy_small, row_sums, 1, cv::REDUCE_SUM, CV_32F); 
    
    // Dynamic threshold based on width (which is fixed kAnalysisWidth now)
    float threshold = (float)kAnalysisWidth * 0.02f; // 2% of pixels moving on average

    int top_content_y = 0;
    int bottom_content_y = analysis_height;

    for (int y = 0; y < analysis_height; y++) {
        if (row_sums.at<float>(y, 0) > threshold) {
            top_content_y = y;
            break;
        }
    }
    
    for (int y = analysis_height - 1; y >= 0; y--) {
        if (row_sums.at<float>(y, 0) > threshold) {
            bottom_content_y = y + 1;
            break;
        }
    }

    // Normalize targets (0.0 to 1.0)
    float target_top_norm = (float)top_content_y / (float)analysis_height;
    float target_bottom_norm = (float)bottom_content_y / (float)analysis_height;

    // Safety: Don't crop everything
    if (target_bottom_norm <= target_top_norm + 0.1f) { 
        target_top_norm = 0.0f;
        target_bottom_norm = 1.0f;
    }

    // 6. Smooth Transitions (Hysteresis) - Interpolate on Normalized Coordinates
    g_crop_top_target = target_top_norm;
    g_crop_bottom_target = target_bottom_norm;

    float alpha = 0.05f; // Slower transition for smoothness
    g_crop_top_current = g_crop_top_current * (1.0f - alpha) + g_crop_top_target * alpha;
    g_crop_bottom_current = g_crop_bottom_current * (1.0f - alpha) + g_crop_bottom_target * alpha;

    // 7. Apply Crop to Original Frame
    int final_top = (int)(g_crop_top_current * frame.rows);
    int final_bottom = (int)(g_crop_bottom_current * frame.rows);

    // Clamp
    final_top = std::max(0, final_top);
    final_bottom = std::min(frame.rows, final_bottom);
    if (final_bottom <= final_top) {
        final_bottom = frame.rows;
        final_top = 0;
    }
    
    // Even height precaution
    int height = final_bottom - final_top;
    if (height % 2 != 0) height--;
    final_bottom = final_top + height;

    if (final_top > 0 || final_bottom < frame.rows) {
        cv::Mat cropped = frame.rowRange(final_top, final_bottom).clone();
        frame = cropped; 
    }
}

// --- Live Perspective Crop State ---
static bool g_live_crop_enabled = false;
static double g_live_crop_corners[8] = {0}; // TL(x,y), TR(x,y), BR(x,y), BL(x,y) - Normalized 0..1
static std::mutex g_live_crop_mutex;

static void ApplyLivePerspectiveCrop(cv::Mat& frame) {
    if (!g_live_crop_enabled || frame.empty()) return;

    double corners[8];
    {
        std::lock_guard<std::mutex> lock(g_live_crop_mutex);
        memcpy(corners, g_live_crop_corners, sizeof(corners));
    }

    int origWidth = frame.cols;
    int origHeight = frame.rows;

    // Convert normalized corners to pixel coordinates
    std::vector<cv::Point2f> srcPoints;
    for (int i = 0; i < 4; i++) {
        srcPoints.push_back(cv::Point2f(
            (float)(corners[i*2] * frame.cols),
            (float)(corners[i*2+1] * frame.rows)
        ));
    }

    // Calculate destination size based on quad dimensions
    auto dist = [](cv::Point2f a, cv::Point2f b) -> float {
        return (float)std::sqrt(std::pow(a.x - b.x, 2) + std::pow(a.y - b.y, 2));
    };

    float w1 = dist(srcPoints[0], srcPoints[1]);
    float w2 = dist(srcPoints[3], srcPoints[2]);
    float maxWidth = std::max(w1, w2);

    float h1 = dist(srcPoints[0], srcPoints[3]);
    float h2 = dist(srcPoints[1], srcPoints[2]);
    float maxHeight = std::max(h1, h2);

    if (maxWidth < 10 || maxHeight < 10) return; // Safety check

    std::vector<cv::Point2f> dstPoints;
    dstPoints.push_back(cv::Point2f(0, 0));
    dstPoints.push_back(cv::Point2f(maxWidth - 1, 0));
    dstPoints.push_back(cv::Point2f(maxWidth - 1, maxHeight - 1));
    dstPoints.push_back(cv::Point2f(0, maxHeight - 1));

    // Apply perspective transform to get cropped content
    cv::Mat M = cv::getPerspectiveTransform(srcPoints, dstPoints);
    cv::Mat warped;
    cv::warpPerspective(frame, warped, M, cv::Size((int)maxWidth, (int)maxHeight));

    // Now resize to fit the original frame height while maintaining aspect ratio
    float cropAspect = maxWidth / maxHeight;
    int finalHeight = origHeight;
    int finalWidth = (int)(finalHeight * cropAspect);
    
    cv::Mat resized;
    cv::resize(warped, resized, cv::Size(finalWidth, finalHeight));

    // Create output frame with original dimensions, filled with black
    frame = cv::Mat::zeros(origHeight, origWidth, frame.type());
    
    // Center the resized image horizontally
    int xOffset = (origWidth - finalWidth) / 2;
    if (xOffset < 0) {
        // Cropped content is wider than frame - scale to fit width instead
        finalWidth = origWidth;
        finalHeight = (int)(origWidth / cropAspect);
        cv::resize(warped, resized, cv::Size(finalWidth, finalHeight));
        xOffset = 0;
        int yOffset = (origHeight - finalHeight) / 2;
        if (yOffset < 0) yOffset = 0;
        cv::Rect destRect(0, yOffset, finalWidth, std::min(finalHeight, origHeight - yOffset));
        cv::Rect srcRect(0, 0, finalWidth, std::min(finalHeight, origHeight - yOffset));
        resized(srcRect).copyTo(frame(destRect));
    } else {
        // Normal case - content fits, center horizontally
        cv::Rect destRect(xOffset, 0, finalWidth, finalHeight);
        resized.copyTo(frame(destRect));
    }
}

// --- YOLOv11 Person Detection Helpers ---

static cv::dnn::Net g_yolo11_net;
static bool g_yolo11_initialized = false;
static bool g_yolo11_failed = false;

// Background modeling for person removal
static cv::Mat g_bg_model_float;
static cv::Mat g_bg_model_8u;

static void ApplyYOLO11Detection(cv::Mat& frame) {
    if (g_yolo11_failed) return;
    if (!g_yolo11_initialized) {
        try {
            std::string modelPath = GetModelPath("yolo11n.onnx");
            g_yolo11_net = cv::dnn::readNetFromONNX(modelPath);
            g_yolo11_net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            g_yolo11_net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            g_yolo11_initialized = !g_yolo11_net.empty();
        } catch (const cv::Exception& e) {
            std::cerr << "Failed to load YOLOv11 model: " << e.what() << std::endl;
            g_yolo11_failed = true;
            return;
        } catch (...) {
            std::cerr << "Failed to load YOLOv11 model (Unknown error)." << std::endl;
            g_yolo11_failed = true;
            return;
        }
    }

    if (!g_yolo11_initialized) return;

    try {
        // Ensure OpenCV uses all available threads
        cv::setNumThreads(cv::getNumberOfCPUs());

        // YOLOv11n standard input size is 640x640
        const int input_w = 640;
        const int input_h = 640;

        auto start_preprocess = std::chrono::high_resolution_clock::now();

        // Preprocessing: Resize and Normalize
        cv::Mat blob = cv::dnn::blobFromImage(frame, 1.0 / 255.0, cv::Size(input_w, input_h), cv::Scalar(), true, false);
        
        g_yolo11_net.setInput(blob);

        auto end_preprocess = std::chrono::high_resolution_clock::now();
        
        // Measure inference time
        auto start_inference = std::chrono::high_resolution_clock::now();

        std::vector<cv::Mat> outputs;
        g_yolo11_net.forward(outputs, g_yolo11_net.getUnconnectedOutLayersNames());

        auto end_inference = std::chrono::high_resolution_clock::now();
        
        long long duration_preprocess = std::chrono::duration_cast<std::chrono::milliseconds>(end_preprocess - start_preprocess).count();
        long long duration_inference = std::chrono::duration_cast<std::chrono::milliseconds>(end_inference - start_inference).count();
        
        // Print FPS every 30 frames
        static int frame_count = 0;
        if (++frame_count % 30 == 0) {
            double fps = (duration_inference > 0) ? (1000.0 / duration_inference) : 0.0;
            std::cout << "YOLOv11 Timing - Preprocess: " << duration_preprocess << "ms | Inference: " << duration_inference << "ms (~" << fps << " FPS)" << std::endl;
        }

        if (outputs.empty()) return;

        // Output shape is typically [1, 84, 8400] for YOLOv11n (1 class + 4 box coords + ... wait, v11 might be different)
        // YOLOv8/v11 output: [Batch, 4 + NumClasses, NumAnchors] -> [1, 4+80, 8400] usually.
        // For Person only model or COCO? The user said "yolo11n.onnx", likely COCO (80 classes).
        // So 4 box + 80 classes = 84 channels.
        
        cv::Mat output = outputs[0];
        
        // Check dimensions
        if (output.dims != 3) {
            // std::cerr << "Unexpected output dimensions: " << output.dims << std::endl;
            return;
        }

        // int batch = output.size[0]; // Unused
        int dimensions = output.size[1]; // 84 (cx, cy, w, h, class0, class1...)
        int rows = output.size[2];       // 8400 (anchors)

        if (dimensions < 5) return; // Need at least box + 1 class

        // Transpose to [Batch, Rows, Dimensions] -> [1, 8400, 84] for easier iteration
        // OpenCV output is [1, 84, 8400]. We want to iterate over 8400 anchors.
        cv::Mat output2D = output.reshape(1, dimensions); // Reshape to 2D: [84, 8400]
        cv::Mat output2D_t;
        cv::transpose(output2D, output2D_t); // [8400, 84]

        float* data = (float*)output2D_t.data;
        
        std::vector<int> class_ids;
        std::vector<float> confidences;
        std::vector<cv::Rect> boxes;

        float conf_threshold = 0.45f;
        float nms_threshold = 0.5f;
        
        float x_factor = (float)frame.cols / (float)input_w;
        float y_factor = (float)frame.rows / (float)input_h;

        for (int i = 0; i < rows; ++i) {
            float* row_ptr = data + i * dimensions;
            
            // Find the class with the highest score
            // First 4 elements are box coordinates (cx, cy, w, h)
            // The rest are class scores.
            float* scores_ptr = row_ptr + 4;
            // int num_classes = dimensions - 4; // Unused
            
            // Optimization: We only care about Person (class 0)
            // If the model is COCO, Person is index 0.
            float person_score = scores_ptr[0]; 

            if (person_score > conf_threshold) {
                float cx = row_ptr[0];
                float cy = row_ptr[1];
                float w = row_ptr[2];
                float h = row_ptr[3];

                int left = int((cx - 0.5 * w) * x_factor);
                int top = int((cy - 0.5 * h) * y_factor);
                int width = int(w * x_factor);
                int height = int(h * y_factor);

                boxes.push_back(cv::Rect(left, top, width, height));
                confidences.push_back(person_score);
                class_ids.push_back(0);
            }
        }

        std::vector<int> indices;
        cv::dnn::NMSBoxes(boxes, confidences, conf_threshold, nms_threshold, indices);

        // --- Person Removal Logic ---
        
        // 1. Initialize background model if needed or if size changed
        if (g_bg_model_float.empty() || g_bg_model_float.size() != frame.size()) {
            frame.convertTo(g_bg_model_float, CV_32F);
            g_bg_model_8u = frame.clone();
        }

        // 2. Create a mask of all detected persons
        cv::Mat person_mask = cv::Mat::zeros(frame.size(), CV_8UC1);
        for (int idx : indices) {
            cv::Rect box = boxes[idx];
            // Slightly expand the box to ensure we cover the person edges
            int pad = 10;
            cv::Rect padded_box = box;
            padded_box.x = std::max(0, box.x - pad);
            padded_box.y = std::max(0, box.y - pad);
            padded_box.width = std::min(frame.cols - padded_box.x, box.width + 2 * pad);
            padded_box.height = std::min(frame.rows - padded_box.y, box.height + 2 * pad);

            cv::rectangle(person_mask, padded_box, cv::Scalar(255), cv::FILLED);
        }

        // 3. Update Background Model
        // We want to update the background model with the current frame to adapt to lighting,
        // BUT we must exclude the person regions so they don't become part of the background.
        // We do this by constructing a synthetic frame that has the "old" background 
        // in the person regions, and the "new" frame everywhere else.
        cv::Mat update_frame = frame.clone();
        if (!g_bg_model_8u.empty()) {
            g_bg_model_8u.copyTo(update_frame, person_mask);
        }

        // Update running average (alpha = 0.05 means it adapts to lighting in ~1 second)
        cv::accumulateWeighted(update_frame, g_bg_model_float, 0.05);
        
        // Update the 8-bit background model for next time
        cv::convertScaleAbs(g_bg_model_float, g_bg_model_8u);

        // 4. Update/Smooth Person Mask
        // To prevent flickering: use a probaility mask with decay
        // mask_prob = mask_prob * 0.8 + new_mask * 0.2
        
        if (g_person_prob_mask.empty() || g_person_prob_mask.size() != frame.size()) {
             g_person_prob_mask = cv::Mat::zeros(frame.size(), CV_32FC1);
        }
        
        cv::Mat current_detection_f;
        person_mask.convertTo(current_detection_f, CV_32F, 1.0/255.0);
        
        // Decay existing probability
        // If person detected (1.0), prob increases. If not (0.0), prob decreases.
        // alpha up = 0.4 (fast appear), alpha down = 0.1 (slow disappear)
        
        // Ideally we do: new_prob = old_prob * (1-alpha) + new_det * alpha
        // But we want asymmetric decay
        // Let's stick to simple exponential moving average for now, but bias towards keeping it?
        
        cv::accumulateWeighted(current_detection_f, g_person_prob_mask, 0.2); // 0.2 speed
        
        // Threshold to get binary mask
        // If prob > 0.2 (arbitrary), treat as person
        cv::Mat final_mask;
        cv::compare(g_person_prob_mask, 0.2, final_mask, cv::CMP_GT); // binary 255/0
        
        // 5. Replace Persons in Current Frame
        // Copy the clean background pixels onto the current frame where persons are detected
        g_bg_model_8u.copyTo(frame, final_mask);
    } catch (const cv::Exception& e) {
        std::cerr << "YOLO Inference Error: " << e.what() << std::endl;
        g_yolo11_failed = true; 
    } catch (...) {
        std::cerr << "YOLO Inference Error (Unknown)" << std::endl;
        g_yolo11_failed = true;
    }
}

void ApplyFilterSequenceInternal(cv::Mat& bgr, int32_t* modes, int32_t count) {
    for (int i = 0; i < count; i++) {
        int mode = modes[i];
        if (mode == 1) { // Invert
            cv::bitwise_not(bgr, bgr);
        } else if (mode == 2) { // Whiteboard Legacy
            float c_value = 15.0f; // default threshold
            {
                std::lock_guard<std::mutex> lock(g_filter_params_mutex);
                auto it = g_filter_params.find(2);
                if (it != g_filter_params.end()) {
                    c_value = it->second;
                }
            }
            cv::Mat gray;
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::adaptiveThreshold(gray, gray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, (int)c_value);
            cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
        } else if (mode == 3) { // Blur Legacy
            cv::GaussianBlur(bgr, bgr, cv::Size(15, 15), 0);
        } else if (mode == 4) { // Smart Whiteboard
            ApplySmartWhiteboard(bgr);
        } else if (mode == 5) { // Smart Obstacle
            ApplySmartObstacleRemoval(bgr);
        } else if (mode == 6) { // Moving Average
            ApplyMovingAverage(bgr);
        } else if (mode == 7) { // CLAHE
            ApplyCLAHE(bgr);
        } else if (mode == 8) { // Sharpening
            ApplySharpening(bgr);
        } else if (mode == 11) { // YOLOv11 Person Detection
            ApplyYOLO11Detection(bgr);
        } else if (mode == 12) { // Shaking Stabilization
            ApplyStabilization(bgr);
        } else if (mode == 13) { // Light Stabilization
            ApplyLightStabilization(bgr);
        } else if (mode == 14) { // Corner Smoothing
            ApplyCornerSmoothing(bgr);
        } else if (mode == 15) { // Smart Video Crop
            ApplySmartVideoCrop(bgr);
        }
    }
}

// --- Perspective Crop Export ---

extern "C" __declspec(dllexport) void ProcessPerspectiveCrop(
    uint8_t* inputBytes, int inputSize,
    double* corners, // 8 doubles: x1,y1, x2,y2, x3,y3, x4,y4 (TL, TR, BR, BL) - Normalized 0..1
    uint8_t** outputBytes, int* outputSize
) {
    if (inputBytes == nullptr || inputSize <= 0) return;

    // 1. Decode
    std::vector<uint8_t> data(inputBytes, inputBytes + inputSize);
    cv::Mat image = cv::imdecode(data, cv::IMREAD_COLOR);
    if (image.empty()) return;

    // 2. Points
    // Corners are normalized (0..1). Convert to image coords.
    std::vector<cv::Point2f> srcPoints;
    for(int i=0; i<4; i++) {
        srcPoints.push_back(cv::Point2f((float)(corners[i*2] * image.cols), (float)(corners[i*2+1] * image.rows)));
    }

    // 3. Destination Size & Points
    // We need to estimate the width/height of the rectified image.
    // Width = max(dist(TL, TR), dist(BL, BR))
    // Height = max(dist(TL, BL), dist(TR, BR))
    
    auto dist = [](cv::Point2f a, cv::Point2f b) -> float {
        return (float)std::sqrt(std::pow(a.x - b.x, 2) + std::pow(a.y - b.y, 2));
    };

    float w1 = dist(srcPoints[0], srcPoints[1]);
    float w2 = dist(srcPoints[3], srcPoints[2]);
    float maxWidth = std::max(w1, w2);

    float h1 = dist(srcPoints[0], srcPoints[3]);
    float h2 = dist(srcPoints[1], srcPoints[2]);
    float maxHeight = std::max(h1, h2);

    std::vector<cv::Point2f> dstPoints;
    dstPoints.push_back(cv::Point2f(0, 0));
    dstPoints.push_back(cv::Point2f(maxWidth - 1, 0));
    dstPoints.push_back(cv::Point2f(maxWidth - 1, maxHeight - 1));
    dstPoints.push_back(cv::Point2f(0, maxHeight - 1));

    // 4. Perspective Transform
    cv::Mat M = cv::getPerspectiveTransform(srcPoints, dstPoints);
    cv::Mat warped;
    cv::warpPerspective(image, warped, M, cv::Size((int)maxWidth, (int)maxHeight));

    // 5. Encode
    std::vector<uint8_t> encoded;
    cv::imencode(".jpg", warped, encoded);

    // 6. Copy to output buffer (Caller must free used dedicated Free function if we were strict, 
    // but here we allocate using CoTaskMemAlloc or similar if crossing interop? 
    // Standard Dart FFI with "malloc" usually allocates via system malloc.
    // If we return a pointer, Dart needs to free it using the same allocator.
    
    // Safer: Dart allocates output buffer? No, size is unknown.
    // We will allocate here using malloc, and Dart `calloc.free` (from ffi) is compatible on Windows (usually).
    // Or we keep it simple: Use a global buffer? No, concurrency issues.
    // Use `CoTaskMemAlloc` on Windows for COM compatibility? 
    // Flutter `ffi` package uses standard `malloc/free`.
    // Let's use `malloc`.
    
    *outputSize = (int)encoded.size();
    *outputBytes = (uint8_t*)malloc(*outputSize);
    memcpy(*outputBytes, encoded.data(), *outputSize);
}

extern "C" __declspec(dllexport) void FreeBuffer(uint8_t* buffer) {
    if (buffer) free(buffer);
}

// Set or clear live perspective crop corners
// corners: 8 doubles (TL.x, TL.y, TR.x, TR.y, BR.x, BR.y, BL.x, BL.y) - Normalized 0..1
// Pass nullptr to disable live crop
extern "C" __declspec(dllexport) void SetLiveCropCorners(double* corners) {
    std::lock_guard<std::mutex> lock(g_live_crop_mutex);
    if (corners == nullptr) {
        g_live_crop_enabled = false;
    } else {
        memcpy(g_live_crop_corners, corners, sizeof(g_live_crop_corners));
        g_live_crop_enabled = true;
    }
}

extern "C" __declspec(dllexport) void SetFilterParameter(int32_t filterId, float param1) {
    std::lock_guard<std::mutex> lock(g_filter_params_mutex);
    g_filter_params[filterId] = param1;
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

    __declspec(dllexport) void StartStream(const char* url) {
        if (g_native_camera) g_native_camera->StartStream(url);
    }

    __declspec(dllexport) void StopCamera() {
        if (g_native_camera) g_native_camera->Stop();
    }

    __declspec(dllexport) void SwitchCamera() {
        if (g_native_camera) g_native_camera->SwitchCamera();
    }

    __declspec(dllexport) void SelectCamera(int index) {
        if (g_native_camera) g_native_camera->SelectCamera(index);
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

    // --- Screen Capture FFI Exports ---
    
    // Get count of capturable windows
    __declspec(dllexport) int32_t GetWindowCount() {
        auto windows = ScreenCaptureSource::EnumerateWindows();
        return static_cast<int32_t>(windows.size());
    }
    
    // Get window title at index (returns length, writes to buffer)
    // Buffer should be preallocated by caller
    __declspec(dllexport) int32_t GetWindowTitle(int32_t index, char* buffer, int32_t bufferSize) {
        auto windows = ScreenCaptureSource::EnumerateWindows();
        if (index < 0 || index >= static_cast<int32_t>(windows.size())) return 0;
        
        const auto& title = windows[index].title;
        // Convert wstring to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0 || len > bufferSize) return 0;
        WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, buffer, bufferSize, nullptr, nullptr);
        return len - 1; // Don't count null terminator
    }
    
    // Get window handle at index (as int64 for FFI)
    __declspec(dllexport) int64_t GetWindowHandle(int32_t index) {
        auto windows = ScreenCaptureSource::EnumerateWindows();
        if (index < 0 || index >= static_cast<int32_t>(windows.size())) return 0;
        return reinterpret_cast<int64_t>(windows[index].hwnd);
    }
    
    // --- Monitor Enumeration FFI Exports ---
    
    // Get count of monitors
    __declspec(dllexport) int32_t GetMonitorCount() {
        auto monitors = ScreenCaptureSource::EnumerateMonitors();
        return static_cast<int32_t>(monitors.size());
    }
    
    // Get monitor name at index (returns length, writes to buffer)
    __declspec(dllexport) int32_t GetMonitorName(int32_t index, char* buffer, int32_t bufferSize) {
        auto monitors = ScreenCaptureSource::EnumerateMonitors();
        if (index < 0 || index >= static_cast<int32_t>(monitors.size())) return 0;
        
        const auto& monitor = monitors[index];
        // Format: "Monitor N (Primary)" or "Monitor N"
        std::wstring displayName = L"Monitor " + std::to_wstring(index + 1);
        if (monitor.isPrimary) {
            displayName += L" (Primary)";
        }
        
        // Convert wstring to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, displayName.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0 || len > bufferSize) return 0;
        WideCharToMultiByte(CP_UTF8, 0, displayName.c_str(), -1, buffer, bufferSize, nullptr, nullptr);
        return len - 1; // Don't count null terminator
    }
    
    // Get monitor bounds (left, top, right, bottom as 4 ints)
    __declspec(dllexport) void GetMonitorBounds(int32_t index, int32_t* left, int32_t* top, int32_t* right, int32_t* bottom) {
        auto monitors = ScreenCaptureSource::EnumerateMonitors();
        if (index < 0 || index >= static_cast<int32_t>(monitors.size())) {
            *left = *top = *right = *bottom = 0;
            return;
        }
        const auto& rect = monitors[index].bounds;
        *left = rect.left;
        *top = rect.top;
        *right = rect.right;
        *bottom = rect.bottom;
    }
    
    // Start screen capture with monitor selection
    // monitorIndex: which monitor to capture (0 = primary)
    // windowHandle: 0 = full screen, otherwise specific window handle
    __declspec(dllexport) int32_t StartScreenCapture(int32_t monitorIndex, int64_t windowHandle) {
        if (!g_screen_capture || !g_native_camera) return 0;
        
        // Stop camera if running (screen capture will feed frames instead)
        g_native_camera->Stop();
        
        HWND hwnd = (windowHandle == 0) ? nullptr : reinterpret_cast<HWND>(windowHandle);
        bool success = g_screen_capture->StartCapture(monitorIndex, hwnd);
        
        if (success) {
            // Start the processing thread so frames pushed by screen capture are processed
            g_native_camera->StartProcessingOnly();
        }
        
        return success ? 1 : 0;
    }
    
    // Stop screen capture
    __declspec(dllexport) void StopScreenCapture() {
        if (g_screen_capture) g_screen_capture->StopCapture();
    }
    
    // Check if screen capture is active
    __declspec(dllexport) int32_t IsScreenCaptureActive() {
        if (!g_screen_capture) return 0;
        return g_screen_capture->IsCapturing() ? 1 : 0;
    }

    __declspec(dllexport) void process_frame(uint8_t* bytes, int32_t width, int32_t height, int32_t mode) {
        if (bytes == nullptr || width <= 0 || height <= 0) return;

        // Wrap bytes in Mat. Assumes BGRA (4 channels)
        cv::Mat frame(height, width, CV_8UC4, bytes);

        // Convert to BGR for processing
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);

        ApplyFilterSequenceInternal(bgr, &mode, 1);

        // Convert back to BGRA and write to original buffer
        cv::cvtColor(bgr, frame, cv::COLOR_BGR2BGRA);
    }

    __declspec(dllexport) void process_frame_rgba(uint8_t* bytes, int32_t width, int32_t height, int32_t mode) {
        if (bytes == nullptr || width <= 0 || height <= 0) return;

        // Wrap bytes in Mat. Assumes RGBA (4 channels)
        cv::Mat frame(height, width, CV_8UC4, bytes);

        // Convert to BGR for processing
        // Input is RGBA, so we use RGBA2BGR
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_RGBA2BGR);

        ApplyFilterSequenceInternal(bgr, &mode, 1);

        // Convert back to BGRA (for BMP display) and write to original buffer
        // We want the output to be BGRA so Image.memory displays it correctly
        cv::cvtColor(bgr, frame, cv::COLOR_BGR2BGRA);
    }

    __declspec(dllexport) void process_frame_sequence_rgba(uint8_t* bytes, int32_t width, int32_t height, int32_t* modes, int32_t count) {
        if (bytes == nullptr || width <= 0 || height <= 0) return;

        // Wrap bytes in Mat. Assumes RGBA (4 channels)
        cv::Mat frame(height, width, CV_8UC4, bytes);

        // Convert to BGR for processing
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_RGBA2BGR);

        ApplyFilterSequenceInternal(bgr, modes, count);

        // Convert back to BGRA (for BMP display)
        cv::cvtColor(bgr, frame, cv::COLOR_BGR2BGRA);
    }

    __declspec(dllexport) void process_frame_sequence_bgra(uint8_t* bytes, int32_t width, int32_t height, int32_t* modes, int32_t count) {
        if (bytes == nullptr || width <= 0 || height <= 0) return;

        // Wrap bytes in Mat. Assumes BGRA (4 channels)
        cv::Mat frame(height, width, CV_8UC4, bytes);

        // Convert to BGR for processing
        cv::Mat bgr;
        cv::cvtColor(frame, bgr, cv::COLOR_BGRA2BGR);

        ApplyFilterSequenceInternal(bgr, modes, count);

        // Convert back to BGRA
        cv::cvtColor(bgr, frame, cv::COLOR_BGR2BGRA);
    }
}
