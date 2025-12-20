#include "screen_capture_source.h"
#include "native_camera.h"
#include <iostream>
#include <dwmapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// Callback for window enumeration
static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
    
    // Skip invisible windows
    if (!IsWindowVisible(hwnd)) return TRUE;
    
    // Skip minimized windows
    if (IsIconic(hwnd)) return TRUE;
    
    // Get window title
    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    
    // Skip windows with no title
    if (wcslen(title) == 0) return TRUE;
    
    // Get class name
    wchar_t className[256] = {0};
    GetClassNameW(hwnd, className, 256);
    
    // Skip certain system windows
    if (wcscmp(className, L"Progman") == 0) return TRUE;
    if (wcscmp(className, L"WorkerW") == 0) return TRUE;
    if (wcscmp(className, L"Shell_TrayWnd") == 0) return TRUE;
    
    // Check if window is cloaked (invisible on modern Windows)
    BOOL isCloaked = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    if (isCloaked) return TRUE;
    
    WindowInfo info;
    info.hwnd = hwnd;
    info.title = title;
    info.className = className;
    windows->push_back(info);
    
    return TRUE;
}

ScreenCaptureSource::ScreenCaptureSource() {}

ScreenCaptureSource::~ScreenCaptureSource() {
    StopCapture();
    CleanupDXGI();
}

void ScreenCaptureSource::Init(NativeCamera* camera) {
    native_camera_ = camera;
}

std::vector<WindowInfo> ScreenCaptureSource::EnumerateWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

std::vector<MonitorInfo> ScreenCaptureSource::EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    
    // Create a temporary D3D device to enumerate outputs
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevels, 1, D3D11_SDK_VERSION,
        &device, &featureLevel, &context
    );
    
    if (FAILED(hr) || !device) return monitors;
    
    // Get DXGI device and adapter
    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        device->Release();
        if (context) context->Release();
        return monitors;
    }
    
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        device->Release();
        if (context) context->Release();
        return monitors;
    }
    
    // Enumerate all outputs (monitors)
    IDXGIOutput* output = nullptr;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc))) {
            MonitorInfo info;
            info.index = i;
            info.name = desc.DeviceName;
            info.bounds = desc.DesktopCoordinates;
            // Primary monitor is at (0,0)
            info.isPrimary = (desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0);
            monitors.push_back(info);
        }
        output->Release();
    }
    
    adapter->Release();
    device->Release();
    if (context) context->Release();
    
    std::cout << "[ScreenCapture] Found " << monitors.size() << " monitor(s)" << std::endl;
    return monitors;
}


bool ScreenCaptureSource::InitializeDXGI(int monitorIndex) {
    HRESULT hr;
    
    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;
    
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &d3d_device_,
        &featureLevel,
        &d3d_context_
    );
    
    if (FAILED(hr)) {
        last_error_ = "Failed to create D3D11 device";
        return false;
    }
    
    // Get DXGI device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) {
        last_error_ = "Failed to get DXGI device";
        return false;
    }
    
    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        last_error_ = "Failed to get DXGI adapter";
        return false;
    }
    
    // Get the specified output (monitor)
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(monitorIndex, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr)) {
        last_error_ = "Failed to get DXGI output for monitor " + std::to_string(monitorIndex);
        return false;
    }
    
    // Get Output1 for duplication
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);
    dxgiOutput->Release();
    if (FAILED(hr)) {
        last_error_ = "Failed to get DXGI Output1";
        return false;
    }
    
    // Create desktop duplication
    hr = dxgiOutput1->DuplicateOutput(d3d_device_, &duplication_);
    dxgiOutput1->Release();
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            last_error_ = "Desktop duplication not available (too many apps using it?)";
        } else if (hr == E_ACCESSDENIED) {
            last_error_ = "Access denied for desktop duplication";
        } else {
            last_error_ = "Failed to create desktop duplication: " + std::to_string(hr);
        }
        return false;
    }
    
    std::cout << "[ScreenCapture] DXGI initialized for monitor " << monitorIndex << std::endl;
    return true;
}

void ScreenCaptureSource::CleanupDXGI() {
    if (staging_texture_) {
        staging_texture_->Release();
        staging_texture_ = nullptr;
    }
    if (duplication_) {
        duplication_->Release();
        duplication_ = nullptr;
    }
    if (d3d_context_) {
        d3d_context_->Release();
        d3d_context_ = nullptr;
    }
    if (d3d_device_) {
        d3d_device_->Release();
        d3d_device_ = nullptr;
    }
}

bool ScreenCaptureSource::GetWindowCaptureRect(HWND hwnd, RECT& rect) {
    if (!hwnd) {
        // Full screen
        rect.left = 0;
        rect.top = 0;
        rect.right = GetSystemMetrics(SM_CXSCREEN);
        rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        return true;
    }
    
    // Get window rect (including DWM frame)
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (FAILED(hr)) {
        // Fallback to GetWindowRect
        if (!GetWindowRect(hwnd, &rect)) {
            return false;
        }
    }
    
    return true;
}

bool ScreenCaptureSource::StartCapture(int monitorIndex, HWND targetWindow) {
    if (is_capturing_) {
        StopCapture();
    }
    
    target_window_ = targetWindow;
    
    if (!GetWindowCaptureRect(target_window_, capture_rect_)) {
        last_error_ = "Failed to get window rect";
        return false;
    }
    
    // If capturing a specific window, auto-detect which monitor it's on
    if (targetWindow != nullptr) {
        // Find which monitor contains the window by checking bounds
        auto monitors = EnumerateMonitors();
        int detectedIndex = 0; // Default to primary
        bool foundMonitor = false;
        
        // First, set default to primary monitor bounds (in case no match)
        for (const auto& monitor : monitors) {
            if (monitor.index == 0) {
                monitor_bounds_ = monitor.bounds;
                break;
            }
        }
        
        for (const auto& monitor : monitors) {
            // Check if the window's center point is within this monitor's bounds
            int windowCenterX = (capture_rect_.left + capture_rect_.right) / 2;
            int windowCenterY = (capture_rect_.top + capture_rect_.bottom) / 2;
            
            if (windowCenterX >= monitor.bounds.left && 
                windowCenterX < monitor.bounds.right &&
                windowCenterY >= monitor.bounds.top && 
                windowCenterY < monitor.bounds.bottom) {
                detectedIndex = monitor.index;
                monitor_bounds_ = monitor.bounds;  // Store monitor bounds for coordinate conversion
                foundMonitor = true;
                std::cout << "[ScreenCapture] Window detected on monitor " << detectedIndex 
                          << " (bounds: " << monitor.bounds.left << "," << monitor.bounds.top 
                          << " to " << monitor.bounds.right << "," << monitor.bounds.bottom << ")" << std::endl;
                break;
            }
        }
        
        if (!foundMonitor) {
            std::cout << "[ScreenCapture] Window not found on any monitor, using primary (index 0)" << std::endl;
        }
        
        target_monitor_ = detectedIndex;
    } else {
        target_monitor_ = monitorIndex;
        // Get monitor bounds for the specified monitor
        auto monitors = EnumerateMonitors();
        bool foundMonitor = false;
        for (const auto& monitor : monitors) {
            if (monitor.index == monitorIndex) {
                monitor_bounds_ = monitor.bounds;
                foundMonitor = true;
                break;
            }
        }
        // Fallback to primary if specified monitor not found
        if (!foundMonitor && !monitors.empty()) {
            monitor_bounds_ = monitors[0].bounds;
            target_monitor_ = monitors[0].index;
            std::cout << "[ScreenCapture] Monitor " << monitorIndex << " not found, using primary" << std::endl;
        }
    }
    
    if (!InitializeDXGI(target_monitor_)) {
        return false;
    }
    
    is_capturing_ = true;
    capture_thread_ = std::thread(&ScreenCaptureSource::CaptureLoop, this);
    
    std::cout << "[ScreenCapture] Started capturing monitor " << target_monitor_ << std::endl;
    return true;
}

void ScreenCaptureSource::StopCapture() {
    is_capturing_ = false;
    
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    
    CleanupDXGI();
    std::cout << "[ScreenCapture] Stopped capturing" << std::endl;
}

void ScreenCaptureSource::CaptureLoop() {
    cv::Mat frame;
    
    while (is_capturing_) {
        if (CaptureFrame(frame)) {
            if (native_camera_ && !frame.empty()) {
                native_camera_->PushExternalFrame(frame);
            }
        }
        
        // Cap at ~30 FPS
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

bool ScreenCaptureSource::CaptureFrame(cv::Mat& output) {
    if (!duplication_) return false;
    
    HRESULT hr;
    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    
    // Try to acquire next frame
    hr = duplication_->AcquireNextFrame(100, &frameInfo, &desktopResource);
    
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame available, that's OK
        return false;
    }
    
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Need to reinitialize with the same monitor
            CleanupDXGI();
            InitializeDXGI(target_monitor_);
        }
        return false;
    }
    
    // Get texture from resource
    ID3D11Texture2D* desktopTexture = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);
    desktopResource->Release();
    
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }
    
    // Get texture description
    D3D11_TEXTURE2D_DESC desc;
    desktopTexture->GetDesc(&desc);
    
    // Create staging texture if needed
    if (!staging_texture_) {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        
        hr = d3d_device_->CreateTexture2D(&stagingDesc, nullptr, &staging_texture_);
        if (FAILED(hr)) {
            desktopTexture->Release();
            duplication_->ReleaseFrame();
            return false;
        }
    }
    
    // Copy to staging texture
    d3d_context_->CopyResource(staging_texture_, desktopTexture);
    desktopTexture->Release();
    
    // Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = d3d_context_->Map(staging_texture_, 0, D3D11_MAP_READ, 0, &mapped);
    
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }
    
    // Create cv::Mat from mapped data (BGRA format)
    cv::Mat fullFrame(desc.Height, desc.Width, CV_8UC4, mapped.pData, mapped.RowPitch);
    
    // If capturing a specific window, crop to that region
    if (target_window_) {
        // Update window rect in case it moved
        GetWindowCaptureRect(target_window_, capture_rect_);
        
        // Clamp to screen bounds
        int screenWidth = desc.Width;
        int screenHeight = desc.Height;
        
        // Convert screen coordinates to monitor-local coordinates
        // The captured texture is for a specific monitor, which may not start at (0,0)
        // For example, a secondary monitor might be at (1920,0) to (3840,1080)
        // We need to subtract the monitor's top-left corner to get local coordinates
        int x = (int)capture_rect_.left - (int)monitor_bounds_.left;
        int y = (int)capture_rect_.top - (int)monitor_bounds_.top;
        int w = (int)(capture_rect_.right - capture_rect_.left);
        int h = (int)(capture_rect_.bottom - capture_rect_.top);
        
        // Clamp to monitor bounds
        x = std::max(0, x);
        y = std::max(0, y);
        w = std::min(screenWidth - x, w);
        h = std::min(screenHeight - y, h);
        
        if (w > 0 && h > 0 && x + w <= screenWidth && y + h <= screenHeight) {
            cv::Rect roi(x, y, w, h);
            cv::Mat cropped = fullFrame(roi);
            cv::cvtColor(cropped, output, cv::COLOR_BGRA2BGR);
        } else {
            cv::cvtColor(fullFrame, output, cv::COLOR_BGRA2BGR);
        }
    } else {
        cv::cvtColor(fullFrame, output, cv::COLOR_BGRA2BGR);
    }
    
    d3d_context_->Unmap(staging_texture_, 0);
    duplication_->ReleaseFrame();
    
    return true;
}
