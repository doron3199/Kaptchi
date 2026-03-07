#pragma once
#include <opencv2/opencv.hpp>

// Enhances a whiteboard BGR image using the pipeline from:
//   github.com/santhalakshminarayana/whiteboard-image-enhance
//
// Pipeline: DoG → Negate → ContrastStretch → GaussianBlur → Gamma → ColorBalance
//
// Input:  CV_8UC3 BGR image
// threshold: minimum DoG response kept (0 = keep all, ~10 suppresses background noise)
// Output: CV_8UC3 BGR enhanced image (bright background, dark strokes)
cv::Mat WhiteboardEnhance(const cv::Mat& bgr, float threshold = 10.0f);

