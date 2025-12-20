#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <opencv2/opencv.hpp>

// Forward declaration
class NativeCamera;

// Structure to hold window info
struct WindowInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring className;
};

// Structure to hold monitor info
struct MonitorInfo {
    int index;
    std::wstring name;
    RECT bounds;
    bool isPrimary;
};

class ScreenCaptureSource {
public:
    ScreenCaptureSource();
    ~ScreenCaptureSource();

    void Init(NativeCamera* camera);

    // Window enumeration
    static std::vector<WindowInfo> EnumerateWindows();
    
    // Monitor enumeration
    static std::vector<MonitorInfo> EnumerateMonitors();
    
    // Capture control
    bool StartCapture(int monitorIndex = 0, HWND targetWindow = nullptr);
    void StopCapture();
    bool IsCapturing() const { return is_capturing_; }

    // Get last error
    std::string GetLastError() const { return last_error_; }

private:
    NativeCamera* native_camera_ = nullptr;
    
    // DXGI/D3D11 resources
    ID3D11Device* d3d_device_ = nullptr;
    ID3D11DeviceContext* d3d_context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* staging_texture_ = nullptr;
    
    // Capture state
    std::atomic<bool> is_capturing_{false};
    std::thread capture_thread_;
    std::mutex mutex_;
    
    // Target window (nullptr = full desktop)
    HWND target_window_ = nullptr;
    RECT capture_rect_ = {0};
    int target_monitor_ = 0;
    RECT monitor_bounds_ = {0};  // Screen coordinates of target monitor
    
    std::string last_error_;
    
    // Internal methods
    bool InitializeDXGI(int monitorIndex);
    void CleanupDXGI();
    void CaptureLoop();
    bool CaptureFrame(cv::Mat& output);
    
    // Helper to convert window handle to capture rect
    bool GetWindowCaptureRect(HWND hwnd, RECT& rect);
};

