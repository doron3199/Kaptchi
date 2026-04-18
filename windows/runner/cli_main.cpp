// kaptchi_cli.exe — standalone video-to-PDF whiteboard processor
//
// Usage:
//   kaptchi_cli.exe <input_video> <output.pdf|output_dir> [--skip <frames>]
//
// Default: --skip 0
//
// Processes a video file through WhiteboardCanvas while skipping the given
// number of source frames between processed frames,
// then exports pages using the same algorithm as the UI's
// "Add Peak Graph To Gallery" action:
//   1. For each sub-canvas, export the peak history frame.
//   2. Also export the latest frame if the peak is stale by 5+ history steps,
//      or if there is no peak.
//
// If the output path ends with .pdf, pages are written to a PDF.
// Otherwise, pages are written as JPEG images into the output directory.
//
// Example:
//   kaptchi_cli.exe lecture.mp4 out.pdf --skip 0
//   kaptchi_cli.exe lecture.mp4 out_images --skip 2

#include "whiteboard_canvas.h"
#include "whiteboard_canvas_process.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <filesystem>

// ---------------------------------------------------------------------------
// Minimal PDF writer — embeds JPEG images, one per page
// ---------------------------------------------------------------------------
namespace PdfWriter {

struct Page {
    std::vector<uint8_t> jpeg;
    int width;
    int height;
    std::string name;
};

// Format an integer as a PDF cross-reference table offset (20 chars padded)
static std::string xrefOffset(long off) {
    std::ostringstream ss;
    ss << std::setw(10) << std::setfill('0') << off << " 00000 n \r\n";
    return ss.str();
}

// Write all pages to a PDF file. Returns true on success.
bool write(const std::string& path, const std::vector<Page>& pages) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Cannot open output file: " << path << std::endl;
        return false;
    }

    int n = static_cast<int>(pages.size());
    if (n == 0) {
        std::cerr << "No pages to write." << std::endl;
        return false;
    }

    // Track byte offsets for xref table
    std::vector<long> offsets;

    auto pos = [&]() -> long { return static_cast<long>(f.tellp()); };
    auto write_str = [&](const std::string& s) { f.write(s.c_str(), s.size()); };

    // Header
    write_str("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");

    // Object 1: Catalog
    offsets.push_back(pos());
    write_str("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    // Object 2: Pages (placeholder — will be written last, but PDF readers
    // handle forward references fine; we patch the Kids array now).
    // We build it after knowing all page object ids.
    // Page objects start at id 3, image XObjects at id (3 + n + i).
    // Layout:
    //   obj 1  = Catalog
    //   obj 2  = Pages
    //   obj 3..3+n-1 = Page dicts
    //   obj 3+n..3+2n-1 = Image XObjects
    //   obj 3+2n = Content stream (shared, reused across pages via /Length)
    //   Actually: each page has its own content stream inline.
    //
    // Simplified layout:
    //   1 = Catalog
    //   2 = Pages
    //   For page i (0-based):
    //     3 + i*2 + 0 = Page dict
    //     3 + i*2 + 1 = Image XObject (the JPEG)
    //   Next free = 3 + n*2

    // We need to know object ids before writing, so pre-compute:
    // page_obj_id(i)  = 3 + i*2
    // image_obj_id(i) = 3 + i*2 + 1
    auto page_obj_id  = [&](int i) { return 3 + i * 2; };
    auto image_obj_id = [&](int i) { return 3 + i * 2 + 1; };

    // Build Kids array string
    std::string kids = "[";
    for (int i = 0; i < n; ++i) {
        if (i > 0) kids += " ";
        kids += std::to_string(page_obj_id(i)) + " 0 R";
    }
    kids += "]";

    // Object 2: Pages
    offsets.push_back(pos());
    write_str("2 0 obj\n<< /Type /Pages /Kids " + kids +
              " /Count " + std::to_string(n) + " >>\nendobj\n");

    // For each page: Page dict + Image XObject
    for (int i = 0; i < n; ++i) {
        const Page& pg = pages[i];
        std::string w = std::to_string(pg.width);
        std::string h = std::to_string(pg.height);
        std::string img_id = std::to_string(image_obj_id(i));

        // Content stream: draw the image to fill the page
        // q W 0 0 H cm /Im Do Q
        std::string cs = "q " + w + " 0 0 " + h + " 0 0 cm /Im Do Q";
        std::string cs_len = std::to_string(cs.size());

        // --- Page dict ---
        offsets.push_back(pos());  // offset for page_obj_id(i)
        write_str(std::to_string(page_obj_id(i)) + " 0 obj\n"
                  "<< /Type /Page\n"
                  "   /Parent 2 0 R\n"
                  "   /MediaBox [0 0 " + w + " " + h + "]\n"
                  "   /Resources << /XObject << /Im " + img_id + " 0 R >> >>\n"
                  "   /Contents << /Length " + cs_len + " >>\n"
                  ">>\n"
                  "stream\n" + cs + "\nendstream\nendobj\n");

        // --- Image XObject (JPEG) ---
        offsets.push_back(pos());  // offset for image_obj_id(i)
        std::string jpeg_len = std::to_string(pg.jpeg.size());
        write_str(img_id + " 0 obj\n"
                  "<< /Type /XObject /Subtype /Image\n"
                  "   /Width " + w + " /Height " + h + "\n"
                  "   /ColorSpace /DeviceRGB /BitsPerComponent 8\n"
                  "   /Filter /DCTDecode /Length " + jpeg_len + "\n"
                  ">>\nstream\n");
        f.write(reinterpret_cast<const char*>(pg.jpeg.data()), pg.jpeg.size());
        write_str("\nendstream\nendobj\n");
    }

    // Cross-reference table
    long xref_pos = pos();
    int total_objects = 2 + n * 2;  // objects 1..2+n*2
    write_str("xref\n0 " + std::to_string(total_objects + 1) + "\n");
    write_str("0000000000 65535 f \r\n");  // free list head
    for (long off : offsets) {
        write_str(xrefOffset(off));
    }

    // Trailer
    write_str("trailer\n"
              "<< /Size " + std::to_string(total_objects + 1) +
              " /Root 1 0 R >>\n"
              "startxref\n" + std::to_string(xref_pos) + "\n%%EOF\n");

    f.close();
    return true;
}

} // namespace PdfWriter

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------
static std::string argValue(int argc, char** argv, const std::string& flag, const std::string& def) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == flag) return argv[i + 1];
    }
    return def;
}

static bool hasFlag(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

static std::string lowercaseCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static bool isPdfOutputPath(const std::string& output_path) {
    return lowercaseCopy(std::filesystem::path(output_path).extension().string()) == ".pdf";
}

static void printProgress(double pct, double currentSec, double totalSec) {
    int p = static_cast<int>(pct * 100.0);
    int cur_m = static_cast<int>(currentSec) / 60;
    int cur_s = static_cast<int>(currentSec) % 60;
    int tot_m = static_cast<int>(totalSec) / 60;
    int tot_s = static_cast<int>(totalSec) % 60;
    std::cout << "\r[" << std::setw(3) << p << "%] "
              << std::setfill('0')
              << std::setw(2) << cur_m << ":" << std::setw(2) << cur_s
              << " / "
              << std::setw(2) << tot_m << ":" << std::setw(2) << tot_s
              << std::setfill(' ')
              << "   " << std::flush;
}

static bool appendOverviewPage(const cv::Mat& overview,
                               std::vector<PdfWriter::Page>& pages,
                               std::string name) {
    if (overview.empty()) return false;

    cv::Mat rgb;
    cv::cvtColor(overview, rgb, cv::COLOR_BGR2RGB);

    std::vector<uint8_t> jpeg_buf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 92};
    if (!cv::imencode(".jpg", rgb, jpeg_buf, params)) {
        return false;
    }

    pages.push_back({jpeg_buf, overview.cols, overview.rows, std::move(name)});
    return true;
}

static bool writeImagesToDirectory(const std::string& output_dir,
                                   const std::vector<PdfWriter::Page>& pages) {
    namespace fs = std::filesystem;

    std::error_code error;
    const fs::path dir_path(output_dir);
    if (fs::exists(dir_path, error)) {
        if (error) {
            std::cerr << "Failed to access output path: " << output_dir << "\n";
            return false;
        }
        if (!fs::is_directory(dir_path, error)) {
            std::cerr << "Output path exists but is not a directory: " << output_dir << "\n";
            return false;
        }
    } else {
        fs::create_directories(dir_path, error);
        if (error) {
            std::cerr << "Failed to create output directory: " << output_dir << "\n";
            return false;
        }
    }

    for (size_t i = 0; i < pages.size(); ++i) {
        std::ostringstream file_name;
        file_name << std::setfill('0') << std::setw(3) << (i + 1);
        if (!pages[i].name.empty()) {
            file_name << "_" << pages[i].name;
        }
        file_name << ".jpg";

        const fs::path image_path = dir_path / file_name.str();
        std::ofstream file(image_path, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Failed to create image file: " << image_path.string() << "\n";
            return false;
        }

        file.write(reinterpret_cast<const char*>(pages[i].jpeg.data()),
                   static_cast<std::streamsize>(pages[i].jpeg.size()));
        if (!file.good()) {
            std::cerr << "Failed to write image file: " << image_path.string() << "\n";
            return false;
        }
    }

    return true;
}

static int exportPeakGraphPages(WhiteboardCanvas& canvas,
                                std::vector<PdfWriter::Page>& pages) {
    constexpr int kStaleThreshold = 5;

    const cv::Size canvas_size = canvas.GetCanvasSize();
    const int export_w = canvas_size.width > 0
        ? std::clamp(canvas_size.width, 1, 16384)
        : 1920;
    const int export_h = canvas_size.height > 0
        ? std::clamp(canvas_size.height, 1, 16384)
        : 1080;
    const cv::Size export_size(export_w, export_h);

    const int canvas_count = canvas.GetSubCanvasCount();
    int added = 0;

    for (int canvas_idx = 0; canvas_idx < canvas_count; ++canvas_idx) {
        const int peak_idx = canvas.GetSubCanvasPeakIndex(canvas_idx);
        const int history_count = canvas.GetSubCanvasHistoryCount(canvas_idx);
        const int latest_idx = history_count - 1;

        if (peak_idx >= 0) {
            cv::Mat peak_overview;
            if (canvas.GetOverviewBlockingForCanvas(
                    canvas_idx,
                    peak_idx,
                    export_size,
                    peak_overview) &&
                appendOverviewPage(
                    peak_overview,
                    pages,
                    "canvas_" + std::to_string(canvas_idx) + "_peak_" + std::to_string(peak_idx))) {
                ++added;
                std::cout << "  Canvas " << canvas_idx
                          << " peak history " << peak_idx
                          << " -> page " << pages.size() << "\n";
            }
        }

        const bool peak_is_stale = peak_idx < 0 || (latest_idx - peak_idx >= kStaleThreshold);
        if (peak_is_stale && latest_idx >= 0) {
            cv::Mat latest_overview;
            if (canvas.GetOverviewBlockingForCanvas(
                    canvas_idx,
                    latest_idx,
                    export_size,
                    latest_overview) &&
                appendOverviewPage(
                    latest_overview,
                    pages,
                    "canvas_" + std::to_string(canvas_idx) + "_latest_" + std::to_string(latest_idx))) {
                ++added;
                std::cout << "  Canvas " << canvas_idx
                          << " latest history " << latest_idx
                          << " -> page " << pages.size() << "\n";
            }
        }
    }

    return added;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--whiteboard-helper") {
            const std::string session_id = (i + 1 < argc) ? argv[i + 1] : "";
            return RunWhiteboardCanvasHelperMain(session_id);
        }
    }

    if (argc < 3) {
        std::cerr << "Usage: kaptchi_cli <input_video> <output.pdf|output_dir> [--skip <frames>]\n"
                  << "  --skip      Frames to skip between processed frames (default: 0)\n";
        return 1;
    }

    std::string input_path  = argv[1];
    std::string output_path = argv[2];
    int skip_frames = std::stoi(argValue(argc, argv, "--skip", "0"));
    const bool write_pdf = isPdfOutputPath(output_path);

    if (skip_frames < 0) skip_frames = 0;

    std::cout << "kaptchi_cli — video whiteboard processor\n"
              << "  Input:    " << input_path    << "\n"
              << "  Output:   " << output_path   << "\n"
              << "  Format:   " << (write_pdf ? "pdf" : "images") << "\n"
              << "  Skip:     " << skip_frames   << " frame(s)\n\n";

    if (hasFlag(argc, argv, "--interval")) {
        std::cout << "Note: --interval is ignored; pages now export using the UI peak-graph gallery algorithm.\n\n";
    }

    if (hasFlag(argc, argv, "--fps")) {
        std::cout << "Note: --fps is ignored; use --skip to control frame sampling.\n\n";
    }

    // Open video
    cv::VideoCapture cap(input_path);
    if (!cap.isOpened()) {
        std::cerr << "Failed to open video: " << input_path << "\n";
        return 1;
    }

    double video_fps     = cap.get(cv::CAP_PROP_FPS);
    double total_frames  = cap.get(cv::CAP_PROP_FRAME_COUNT);
    double total_seconds = total_frames / video_fps;

    if (video_fps <= 0 || total_frames <= 0) {
        std::cerr << "Could not read video properties. "
                     "Is this a valid video file?\n";
        return 1;
    }

    const int frame_step = skip_frames + 1;
    const double effective_fps = video_fps / static_cast<double>(frame_step);
    std::cout << "Video: " << static_cast<int>(total_seconds / 60) << "m "
              << static_cast<int>(total_seconds) % 60 << "s at "
              << video_fps << " fps (" << static_cast<int>(total_frames) << " frames)\n"
              << "Processing 1 frame every " << frame_step << " frame(s)"
              << " (effective " << std::fixed << std::setprecision(3)
              << effective_fps << " fps).\n\n";

    // Create WhiteboardCanvas
    WhiteboardCanvas canvas;

    std::vector<PdfWriter::Page> pages;

    int frame_idx = 0;
    int next_process_frame = 0;
    double prev_print_pct = -1.0;

    // Sequential grab/retrieve: grab() decodes only the container header (cheap);
    // retrieve() decodes the pixel data and is called only on frames we process.
    // This avoids the per-frame seek that cap.set(POS_FRAMES) requires.
    while (cap.grab()) {
        const double current_sec = frame_idx / video_fps;

        if (frame_idx == next_process_frame) {
            cv::Mat frame;
            cap.retrieve(frame);

            if (!frame.empty()) {
                // ProcessFrameSync: synchronous, no worker thread, accepts empty mask.
                canvas.ProcessFrameSync(frame);

                // Progress (throttled)
                double pct = frame_idx / total_frames;
                if (pct - prev_print_pct >= 0.005) {
                    printProgress(pct, current_sec, total_seconds);
                    prev_print_pct = pct;
                }
            }

            next_process_frame = frame_idx + frame_step;
        }

        ++frame_idx;
    }

    std::cout << "\nFinalizing canvas and exporting peak graph pages...\n";
    const int added_pages = exportPeakGraphPages(canvas, pages);

    if (pages.empty()) {
        if (added_pages == 0 && canvas.GetSubCanvasCount() == 0) {
            std::cerr << "No canvas data available. Is the video a whiteboard recording?\n";
        } else {
            std::cerr << "No peak graph images were available for export.\n";
        }
        return 1;
    }

    std::cout << canvas.DumpProfile();

    if (write_pdf) {
        std::cout << "Writing " << pages.size() << " page(s) to " << output_path << "...\n";
        if (!PdfWriter::write(output_path, pages)) {
            return 1;
        }
        std::cout << "Done. PDF saved to: " << output_path << "\n";
    } else {
        std::cout << "Writing " << pages.size() << " image(s) to directory " << output_path << "...\n";
        if (!writeImagesToDirectory(output_path, pages)) {
            return 1;
        }
        std::cout << "Done. Images saved to: " << output_path << "\n";
    }
    return 0;
}
