// Capture + TensorRT benchmark: no mouse, app UI, model/config writes or screenshot I/O.
// Build with /I src for working code or /I .workbuddy/performance/baseline/src for baseline.
// Usage: benchmark_capture_tensorrt TRT SECONDS CROP GPU(0/1) [DEVICE WIDTH HEIGHT FORMAT FPS OPENCV_THREADS]
// Latency is acquisition-return to completed postprocess; sensor/input latency is not measured.
#define NOMINMAX
#include "OpnecvCapture.cpp"
#include "TensorRT.cpp"
#include "TrtRuntime.cpp"
#include <chrono>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <stdexcept>

using Clock = std::chrono::steady_clock;
struct Timing { double preprocess, inference, postprocess, total; };
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
class BenchDetector {
public:
    explicit BenchDetector(const std::string& model, const char*, bool gpu) {
        if (!gpu) throw std::runtime_error("TensorRT requires an NVIDIA GPU");
        if (saga::trt_create(model.c_str()) != 0) throw std::runtime_error(saga::trt_last_error());
    }
    ~BenchDetector() { saga::trt_destroy(); }
    void setMaxDetections(int) {}
    size_t timed(const cv::Mat& image, Timing& timing) {
        DetectObject objects[99];
        const auto begin = Clock::now();
        const int count = trtDetectBgr(image.data, image.cols, image.rows, image.step, .25f, .45f, objects);
        const auto end = Clock::now();
        if (count < 0) throw std::runtime_error(saga::trt_last_error());
        timing = {NAN, NAN, NAN, ms(begin, end)};
        return size_t(count);
    }
};
static std::string narrow(const wchar_t* text, unsigned codePage) {
    const int count = WideCharToMultiByte(codePage, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Invalid string conversion");
    std::string result(count, '\0');
    WideCharToMultiByte(codePage, 0, text, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}
static void stats(const char* name, std::vector<double> times) {
    const double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    std::sort(times.begin(), times.end());
    std::cout << name << " mean_ms=" << mean << " p50_ms=" << times[times.size() / 2]
              << " p95_ms=" << times[static_cast<size_t>((times.size() - 1) * .95)] << '\n';
}
struct CaptureScope {
    ~CaptureScope() { saga::setcjk_ex(nullptr, 0, 0, nullptr, 0, 0); }
};
int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        std::cerr << "Usage: benchmark_capture_tensorrt TRT SECONDS CROP GPU(0/1) [DEVICE WIDTH HEIGHT FORMAT FPS OPENCV_THREADS]\n";
        return 2;
    }
    try {
        const int seconds = std::stoi(argv[2]), crop = std::stoi(argv[3]);
        const bool gpu = std::stoi(argv[4]) != 0;
        if (seconds < 1 || seconds > 3600 || crop < 1 || crop > 640) throw std::runtime_error("Invalid duration/crop");
        const std::string device = argc > 5 ? narrow(argv[5], CP_UTF8) : "Live Gamer Ultra 2.1-Video";
        const int width = argc > 6 ? std::stoi(argv[6]) : 1920;
        const int height = argc > 7 ? std::stoi(argv[7]) : 1080;
        const std::string format = argc > 8 ? narrow(argv[8], CP_UTF8) : "NV12";
        const int fps = argc > 9 ? std::stoi(argv[9]) : 240;
        if (argc > 10) cv::setNumThreads(std::stoi(argv[10]));
        BenchDetector detector(narrow(argv[1], CP_ACP), "", gpu);
        detector.setMaxDetections(99);
        CaptureScope stopOnExit;
        const char* result = saga::setcjk_ex(device.c_str(), width, height, format.c_str(), fps, crop);
        if (std::strcmp(result, "成功") != 0) throw std::runtime_error(result);
        unsigned long long sequence = 0;
        CaptureFrame frame;
        // Warm 20 distinct card frames; excluded from throughput and latency.
        for (int i = 0; i < 20; ++i) {
            if (!captureAcquire(frame, sequence, 2000)) throw std::runtime_error("Capture timed out during warmup");
            cv::Mat image(frame.height, frame.width, CV_8UC3, const_cast<BYTE*>(frame.bmp + 54), frame.stride);
            Timing timing;
            detector.timed(image, timing);
            image.release(); frame = {};
        }
        std::vector<double> preprocess, inference, postprocess, total, acquireToResult;
        std::cout << "cuda_graph=" << bool(::detector->graphExec)
                  << " blocking_sync=" << bool(::detector->completion) << '\n';
        unsigned long long first = 0, last = 0, detections = 0;
        const auto begin = Clock::now();
        while (Clock::now() - begin < std::chrono::seconds(seconds)) {
            if (!captureAcquire(frame, sequence, 100)) continue;
            const auto acquired = Clock::now();
            if (!first) first = sequence;
            last = sequence;
            cv::Mat image(frame.height, frame.width, CV_8UC3, const_cast<BYTE*>(frame.bmp + 54), frame.stride);
            Timing timing;
            detections += detector.timed(image, timing);
            image.release(); frame = {};
            acquireToResult.push_back(ms(acquired, Clock::now()));
            preprocess.push_back(timing.preprocess); inference.push_back(timing.inference);
            postprocess.push_back(timing.postprocess); total.push_back(timing.total);
        }
        const double elapsed = ms(begin, Clock::now()) / 1000;
        if (total.empty()) throw std::runtime_error("No completed frames");
        // Include the latest published sequence, even if inference finished after it.
        CaptureFrame newest;
        captureAcquire(newest, sequence, 0);
        newest = {};
        const auto published = sequence - first + 1;
        std::cout << std::fixed << std::setprecision(5)
                  << "gpu=" << gpu << " crop=" << crop << " requested_capture_fps=" << fps
                  << " completed_unique_frames=" << total.size() << " published_frames=" << published
                  << " unprocessed_frames=" << published - total.size() << " last_processed_sequence=" << last
                  << " seconds=" << elapsed << " completed_fps=" << total.size() / elapsed
                  << " published_fps=" << published / elapsed << " detections=" << detections << '\n';
        stats("preprocess", preprocess); stats("inference", inference); stats("postprocess", postprocess);
        stats("inference_call", total); stats("acquired_to_result", acquireToResult);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
