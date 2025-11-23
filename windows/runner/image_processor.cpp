#include <opencv2/opencv.hpp>
#include <vector>
#include <cstdint>

// Avoid name mangling so Dart can find the functions
extern "C" {

    // 0: None, 1: Invert, 2: Whiteboard, 3: Obstacle Removal
    __declspec(dllexport) void process_frame(uint8_t* bytes, int width, int height, int mode) {
        if (mode == 0) return;

        // Create a Mat wrapper around the existing byte array (BGRA format from Flutter)
        // CV_8UC4 corresponds to 4 channels (BGRA)
        cv::Mat frame(height, width, CV_8UC4, bytes);

        if (mode == 1) {
            // Invert Colors
            cv::bitwise_not(frame, frame);
        }
        else if (mode == 2) {
            // Whiteboard Enhancement
            cv::Mat gray;
            cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
            
            // Adaptive Thresholding
            // Block size 21, C=10 are tunable parameters
            cv::adaptiveThreshold(gray, gray, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, 10);
            
            // Convert back to BGRA so it can be displayed
            cv::cvtColor(gray, frame, cv::COLOR_GRAY2BGRA);
        }
        else if (mode == 3) {
            // Simple Obstacle Removal (Placeholder for Median Filter)
            // For a true median filter, we need to store state (history of frames).
            // For now, let's just do a heavy blur to show it's working differently
            cv::GaussianBlur(frame, frame, cv::Size(15, 15), 0);
        }
    }
}
