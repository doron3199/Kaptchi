#include "native_camera.h"
#include <iostream>
#include <cmath>
#include <chrono>
#include <objbase.h>
#include <opencv2/dnn.hpp>
#include <windows.h> // For GetModuleFileName

// Forward declaration
void ApplyFilterSequenceInternal(cv::Mat& bgr, int32_t* modes, int32_t count);

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

// Helper functions for external processing (WebRTC stream)
// These are duplicated from NativeCamera to allow stateless/independent processing

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

        // 4. Replace Persons in Current Frame
        // Copy the clean background pixels onto the current frame where persons are detected
        g_bg_model_8u.copyTo(frame, person_mask);
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
            cv::Mat gray;
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::adaptiveThreshold(gray, gray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, 10);
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
        }
    }
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
