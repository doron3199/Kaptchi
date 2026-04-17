// kaptchi_cli.exe — standalone video-to-PDF whiteboard processor
//
// Usage:
//   kaptchi_cli.exe <input_video> <output.pdf> [--interval <seconds>] [--fps <fps>]
//
// Defaults: --interval 30  --fps 1
//
// Processes a video file through WhiteboardCanvas at the given fps,
// snapshots the accumulated canvas state every <interval> seconds of video,
// and writes all snapshots as pages into a PDF file.
//
// Example:
//   kaptchi_cli.exe lecture.mp4 out.pdf --interval 30 --fps 1
//   → 60-min lecture at 1fps, snapshot every 30s → ~120 pages in out.pdf

#include "whiteboard_canvas.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

// ---------------------------------------------------------------------------
// Minimal PDF writer — embeds JPEG images, one per page
// ---------------------------------------------------------------------------
namespace PdfWriter {

struct Page {
    std::vector<uint8_t> jpeg;
    int width;
    int height;
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

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: kaptchi_cli <input_video> <output.pdf> "
                     "[--interval <seconds>] [--fps <fps>]\n"
                  << "  --interval  Snapshot interval in video seconds (default: 30)\n"
                  << "  --fps       Frames per second to process (default: 1)\n";
        return 1;
    }

    std::string input_path  = argv[1];
    std::string output_path = argv[2];
    double interval_sec = std::stod(argValue(argc, argv, "--interval", "30"));
    double target_fps   = std::stod(argValue(argc, argv, "--fps",      "1"));

    if (interval_sec <= 0) interval_sec = 30.0;
    if (target_fps   <= 0) target_fps   = 1.0;

    std::cout << "kaptchi_cli — video whiteboard processor\n"
              << "  Input:    " << input_path    << "\n"
              << "  Output:   " << output_path   << "\n"
              << "  Interval: " << interval_sec  << "s\n"
              << "  Process:  " << target_fps    << " fps\n\n";

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

    int frame_step = std::max(1, static_cast<int>(video_fps / target_fps));
    std::cout << "Video: " << static_cast<int>(total_seconds / 60) << "m "
              << static_cast<int>(total_seconds) % 60 << "s at "
              << video_fps << " fps (" << static_cast<int>(total_frames) << " frames)\n"
              << "Processing every " << frame_step << " frame(s).\n\n";

    // Create WhiteboardCanvas
    WhiteboardCanvas canvas;

    std::vector<PdfWriter::Page> pages;
    double last_snapshot_sec = -1e9;  // Force first snapshot at t=0 if interval<=0

    int frame_idx = 0;
    double prev_print_pct = -1.0;

    while (frame_idx < static_cast<int>(total_frames)) {
        // Seek to the desired frame
        cap.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(frame_idx));
        cv::Mat frame;
        if (!cap.read(frame) || frame.empty()) {
            // Try to continue to next step
            frame_idx += frame_step;
            continue;
        }

        double current_sec = frame_idx / video_fps;

        // Feed frame to WhiteboardCanvas (no person mask in CLI mode)
        canvas.ProcessFrame(frame, cv::Mat());

        // Print progress (throttled to avoid console spam)
        double pct = frame_idx / total_frames;
        if (pct - prev_print_pct >= 0.005) {
            printProgress(pct, current_sec, total_seconds);
            prev_print_pct = pct;
        }

        // Snapshot check
        if (current_sec - last_snapshot_sec >= interval_sec) {
            // GetOverviewBlocking waits for the worker thread to finish
            cv::Mat overview;
            if (canvas.GetOverviewBlocking(cv::Size(1920, 1080), overview)
                && canvas.HasContent()
                && !overview.empty()) {

                // Convert BGR→RGB for JPEG encoding (PDF /DeviceRGB)
                cv::Mat rgb;
                cv::cvtColor(overview, rgb, cv::COLOR_BGR2RGB);

                std::vector<uint8_t> jpeg_buf;
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 92};
                if (cv::imencode(".jpg", rgb, jpeg_buf, params)) {
                    pages.push_back({
                        jpeg_buf,
                        overview.cols,
                        overview.rows
                    });
                    int mm = static_cast<int>(current_sec) / 60;
                    int ss = static_cast<int>(current_sec) % 60;
                    std::cout << "\n  Snapshot at "
                              << std::setfill('0') << std::setw(2) << mm << ":"
                              << std::setw(2) << ss
                              << " → page " << pages.size() << "\n";
                    std::cout << std::flush;
                    prev_print_pct = -1.0;  // Force progress reprint
                }
            }
            last_snapshot_sec = current_sec;
        }

        frame_idx += frame_step;
    }

    // Final snapshot
    std::cout << "\nFinalizing canvas...\n";
    cv::Mat final_overview;
    if (canvas.GetOverviewBlocking(cv::Size(1920, 1080), final_overview)
        && canvas.HasContent()
        && !final_overview.empty()) {

        cv::Mat rgb;
        cv::cvtColor(final_overview, rgb, cv::COLOR_BGR2RGB);
        std::vector<uint8_t> jpeg_buf;
        std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 92};
        if (cv::imencode(".jpg", rgb, jpeg_buf, params)) {
            // Avoid duplicate of last snapshot if video ended exactly on interval
            double last_sec = (total_frames - 1) / video_fps;
            if (last_sec - last_snapshot_sec > 1.0 || pages.empty()) {
                pages.push_back({jpeg_buf, final_overview.cols, final_overview.rows});
                std::cout << "  Final snapshot → page " << pages.size() << "\n";
            }
        }
    }

    if (pages.empty()) {
        std::cerr << "No content captured. Is the video a whiteboard recording?\n";
        return 1;
    }

    std::cout << "Writing " << pages.size() << " page(s) to " << output_path << "...\n";
    if (!PdfWriter::write(output_path, pages)) {
        return 1;
    }

    std::cout << "Done. PDF saved to: " << output_path << "\n";
    return 0;
}
