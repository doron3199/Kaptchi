#include "whiteboard_enhance.h"

#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// normalize_kernel
// Properly zero-summing kernel normalization.
// Positive entries are scaled to sum to scaling_factor.
// Negative entries are scaled so their absolute sum equals scaling_factor.
// ---------------------------------------------------------------------------
static void NormalizeKernel(std::vector<double>& kernel,
                             double scaling_factor = 1.0) {
    const double K_EPS = 1.0e-12;
    double pos_range = 0.0, neg_range = 0.0;
    for (auto& v : kernel) {
        if (std::abs(v) < K_EPS) v = 0.0;
        if (v < 0.0) neg_range += v;
        else         pos_range += v;
    }

    double pos_scale = (std::abs(pos_range) >= K_EPS)
                           ? scaling_factor / pos_range
                           : 0.0;
    double neg_scale = (std::abs(neg_range) >= K_EPS)
                           ? scaling_factor / (-neg_range)
                           : 0.0;

    for (auto& v : kernel) {
        if (!std::isnan(v))
            v *= (v >= 0.0) ? pos_scale : neg_scale;
    }
}

// ---------------------------------------------------------------------------
// DoG  — Difference of Gaussian
// sigma_2 == 0 → unity (delta) kernel subtracted at centre.
// Operates in float32 to avoid uint8 clipping of negative values.
// ---------------------------------------------------------------------------
static cv::Mat DoG(const cv::Mat& img,
                   int k_size, double sigma_1, double sigma_2) {
    const int x = (k_size - 1) / 2;
    const int y = (k_size - 1) / 2;
    std::vector<double> kernel(k_size * k_size, 0.0);

    // First Gaussian (or unity)
    if (sigma_1 > 0.0) {
        const double co1 = 1.0 / (2.0 * sigma_1 * sigma_1);
        const double co2 = 1.0 / (2.0 * M_PI * sigma_1 * sigma_1);
        int i = 0;
        for (int v = -y; v <= y; ++v)
            for (int u = -x; u <= x; ++u)
                kernel[i++] = std::exp(-(u*u + v*v) * co1) * co2;
    } else {
        kernel[x + y * k_size] = 1.0;
    }

    // Subtract second Gaussian (or unity)
    if (sigma_2 > 0.0) {
        const double co1 = 1.0 / (2.0 * sigma_2 * sigma_2);
        const double co2 = 1.0 / (2.0 * M_PI * sigma_2 * sigma_2);
        int i = 0;
        for (int v = -y; v <= y; ++v)
            for (int u = -x; u <= x; ++u)
                kernel[i++] -= std::exp(-(u*u + v*v) * co1) * co2;
    } else {
        kernel[x + y * k_size] -= 1.0;
    }

    NormalizeKernel(kernel, 1.0);

    cv::Mat K(k_size, k_size, CV_64F, kernel.data());
    cv::Mat img_f;
    img.convertTo(img_f, CV_32F);
    cv::Mat result_f;
    cv::filter2D(img_f, result_f, CV_32F, K);
    cv::Mat result;
    result_f.convertTo(result, CV_8U);  // round and clip to [0,255]
    return result;
}

// ---------------------------------------------------------------------------
// get_black_white_indices — histogram scan for clip points
// ---------------------------------------------------------------------------
static std::pair<int, int> GetBlackWhiteIndices(const std::vector<float>& hist,
                                                 float black_count,
                                                 float white_threshold) {
    int black_ind = 0, white_ind = 255;
    float co = 0.0f;
    for (int i = 0; i < 256; ++i) {
        co += hist[i];
        if (co > black_count) { black_ind = i; break; }
    }
    co = 0.0f;
    for (int i = 255; i >= 0; --i) {
        co += hist[i];
        if (co > white_threshold) { white_ind = i; break; }
    }
    return {black_ind, white_ind};
}

// ---------------------------------------------------------------------------
// contrast_stretch
// black_point / white_point are percentages (e.g. 2.0 and 99.5).
// ---------------------------------------------------------------------------
static cv::Mat ContrastStretch(const cv::Mat& img,
                               float black_point, float white_point) {
    // Total pixels across all channels
    const int tot = img.rows * img.cols * img.channels();
    const float black_count = tot * black_point / 100.0f;
    const float white_count = tot * white_point / 100.0f;
    const float white_thresh = tot - white_count;

    // 1. Build a single global histogram
    std::vector<float> hist(256, 0.0f);
    if (img.isContinuous()) {
        const uchar* ptr = img.ptr<uchar>();
        for (int i = 0; i < tot; ++i) {
            hist[ptr[i]] += 1.0f;
        }
    } else {
        for (int r = 0; r < img.rows; ++r) {
            const uchar* ptr = img.ptr<uchar>(r);
            for (int c = 0; c < img.cols * img.channels(); ++c) {
                hist[ptr[c]] += 1.0f;
            }
        }
    }

    // 2. Get global black/white indices
    auto [bi, wi] = GetBlackWhiteIndices(hist, black_count, white_thresh);

    // 3. Apply the same LUT to all channels
    uchar lut_data[256];
    for (int i = 0; i < 256; ++i) {
        if (i < bi)
            lut_data[i] = 0;
        else if (i > wi)
            lut_data[i] = 255;
        else if (wi - bi > 0)
            lut_data[i] = static_cast<uchar>(
                std::round(static_cast<float>(i - bi) /
                           static_cast<float>(wi - bi) * 255.0f));
        else
            lut_data[i] = 0;
    }
    
    cv::Mat lut(1, 256, CV_8U, lut_data);
    cv::Mat result;
    // cv::LUT cleanly applies the single-channel LUT to all 3 channels
    cv::LUT(img, lut, result); 
    return result;
}

// ---------------------------------------------------------------------------
// gamma correction
// ---------------------------------------------------------------------------
static cv::Mat GammaCorrect(const cv::Mat& img, double gamma_value) {
    const double ig = 1.0 / gamma_value;
    uchar lut_data[256];
    for (int i = 0; i < 256; ++i)
        lut_data[i] = static_cast<uchar>(
            std::round(std::pow(i / 255.0, ig) * 255.0));
    cv::Mat lut(1, 256, CV_8U, lut_data);
    cv::Mat result;
    cv::LUT(img, lut, result);
    return result;
}

// ---------------------------------------------------------------------------
// color_balance — per-channel contrast stretch via cumulative histogram.
// low_per / high_per are percentages (e.g. 2.0 and 1.0).
// ---------------------------------------------------------------------------
static cv::Mat ColorBalance(const cv::Mat& img,
                             float low_per, float high_per) {
    const int tot = img.rows * img.cols;
    const float low_count  = tot * low_per / 100.0f;
    const float high_count = tot * (100.0f - high_per) / 100.0f;

    std::vector<cv::Mat> channels;
    cv::split(img, channels);

    for (auto& ch : channels) {
        // Cumulative histogram
        float cum[256] = {};
        for (int r = 0; r < ch.rows; ++r)
            for (int c = 0; c < ch.cols; ++c)
                cum[ch.at<uchar>(r, c)] += 1.0f;
        for (int i = 1; i < 256; ++i)
            cum[i] += cum[i - 1];

        // searchsorted equivalent
        int li = 0, hi = 255;
        for (int i = 0; i < 256; ++i) { if (cum[i] >= low_count)  { li = i; break; } }
        for (int i = 0; i < 256; ++i) { if (cum[i] >= high_count) { hi = i; break; } }

        if (li == hi) continue;

        uchar lut_data[256];
        for (int i = 0; i < 256; ++i) {
            if (i < li)
                lut_data[i] = 0;
            else if (i > hi)
                lut_data[i] = 255;
            else
                lut_data[i] = static_cast<uchar>(
                    std::round(static_cast<float>(i - li) /
                               static_cast<float>(hi - li) * 255.0f));
        }
        cv::Mat lut(1, 256, CV_8U, lut_data);
        cv::LUT(ch, lut, ch);
    }

    cv::Mat result;
    cv::merge(channels, result);
    return result;
}

// ---------------------------------------------------------------------------
// WhiteboardEnhance — public entry point
// Parameters are taken verbatim from the reference Python implementation.
// ---------------------------------------------------------------------------
cv::Mat WhiteboardEnhance(const cv::Mat& bgr, float threshold) {
    // --- parameters matching the Python reference ---
    constexpr int    DOG_K    = 31;
    constexpr double DOG_S1   = 100.0;
    constexpr double DOG_S2   = 0.0;
    constexpr float  CS_BLK   = 2.0f;
    constexpr float  CS_WHT   = 99.5f;
    constexpr int    GB_K     = 3;
    constexpr double GB_SIG   = 1.0;
    constexpr double GAMMA    = 1.1;
    constexpr float  CB_LOW   = 0.1f;
    constexpr float  CB_HIGH  = 1.0f;
    cv::Mat dog_img = DoG(bgr, DOG_K, DOG_S1, DOG_S2);
    // Suppress background noise: zero out pixels below the threshold before
    // contrast-stretching, so faint surface texture doesn't get amplified.
    if (threshold > 0.0f) {
        cv::Mat gray_dog;
        cv::cvtColor(dog_img, gray_dog, cv::COLOR_BGR2GRAY);
        dog_img.setTo(0, gray_dog < static_cast<int>(threshold));
    }
    cv::Mat neg_img;
    cv::bitwise_not(dog_img, neg_img);
    cv::Mat cs_img    = ContrastStretch(neg_img, CS_BLK, CS_WHT);
    cv::Mat blur_img;
    {
        cv::Mat k1d = cv::getGaussianKernel(GB_K, GB_SIG);
        cv::sepFilter2D(cs_img, blur_img, -1, k1d, k1d);
    }
    cv::Mat gamma_img = GammaCorrect(blur_img, GAMMA);
    return ColorBalance(gamma_img, CB_LOW, CB_HIGH);
}

