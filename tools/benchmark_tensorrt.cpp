#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <numeric>
#include <limits>
// Uses the production TensorRT runtime, preprocessing and postprocessing.
// Stage times are unavailable; total measures the complete BGR inference API.
#include <cstring>
#include <iostream>
#include "../src/TensorRT.cpp"
#include "../src/TrtRuntime.cpp"

using Clock = std::chrono::steady_clock;
struct Timing { double preprocess, inference, postprocess, total; };
static double elapsed(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
struct BenchDetection { cv::Rect box; int classId; float conf; };
class BenchDetector {
    std::vector<BenchDetection> dets;
public:
    explicit BenchDetector(const std::string& model) {
        if (saga::trt_create(model.c_str()) != 0) throw std::runtime_error(saga::trt_last_error());
    }
    ~BenchDetector() { saga::trt_destroy(); }
    void setMaxDetections(int) {}
    void describe() { std::cout << "TensorRT production BGR runtime; maxDetections=99\n"; }
    const std::vector<BenchDetection>& timed(const cv::Mat& image, Timing& timing) {
        DetectObject objects[99];
        const auto start = Clock::now();
        const int count = trtDetectBgr(image.data, image.cols, image.rows, image.step, .25f, .45f, objects);
        const auto end = Clock::now();
        if (count < 0) throw std::runtime_error(saga::trt_last_error());
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        timing = {nan, nan, nan, elapsed(start, end)};
        dets.clear();
        for (int i = 0; i < count; ++i) {
            const auto& d = objects[i];
            dets.push_back({cv::Rect(int(d.x), int(d.y), int(d.width), int(d.height)), d.label, d.prob});
        }
        return dets;
    }
    void detectBorrowed(const cv::Mat& image, float, float) { Timing timing; timed(image, timing); }
};
static std::string ansi(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    s.pop_back(); return s;
}
static void adapters() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        std::cout << "adapter " << i << ": " << ansi(desc.Description) << " VRAM="
                  << desc.DedicatedVideoMemory / (1024 * 1024) << "MiB flags=" << desc.Flags << '\n';
        adapter->Release();
    }
    factory->Release();
}
static void stats(const char* name, std::vector<double> v) {
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    std::sort(v.begin(), v.end());
    std::cout << name << " mean_ms=" << sum / v.size() << " p50_ms=" << v[v.size()/2]
              << " p95_ms=" << v[static_cast<size_t>((v.size()-1)*0.95)] << '\n';
}
template<class T> static void write(std::ofstream& stream, T value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
static double processCpuMs() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) / 10000.0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 8) {
        std::cerr << "Usage: benchmark_tensorrt TRT IMAGES CROP(0=full) IMAGE_COUNT ROUNDS OUTPUT_PREFIX GPU(1) [OPENCV_THREADS]\n";
        return 2;
    }
    try {
        adapters();
        const int crop = std::stoi(argv[3]), limit = std::stoi(argv[4]), rounds = std::stoi(argv[5]);
        if (crop < 0 || limit < 1 || rounds < 1) throw std::runtime_error("Invalid benchmark dimensions/counts");
        const auto parent = std::filesystem::path(argv[6]).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        if (argc > 8) cv::setNumThreads(std::stoi(argv[8]));
        std::vector<std::filesystem::path> paths;
        for (auto& entry : std::filesystem::directory_iterator(argv[2])) {
            if (!entry.is_regular_file()) continue;
            auto ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp") paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        const size_t count = (std::min)(paths.size(), static_cast<size_t>(limit));
        std::vector<cv::Mat> images;
        std::ofstream manifest(std::filesystem::path(std::wstring(argv[6]) + L".manifest.txt"));
        for (size_t i = 0; i < count; ++i) {
            const auto& p = paths[count == 1 ? 0 : i * (paths.size()-1) / (count-1)];
            std::ifstream input(p, std::ios::binary);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
            cv::Mat image = cv::imdecode(bytes, cv::IMREAD_COLOR);
            if (image.empty()) throw std::runtime_error("Image decode failed");
            if (crop > 0) {
                const int w = (std::min)(crop, image.cols), h = (std::min)(crop, image.rows);
                image = image(cv::Rect((image.cols-w)/2, (image.rows-h)/2, w, h)).clone();
            }
            images.push_back(std::move(image));
            // UTF-8 filenames keep the manifest usable outside the program.
            const auto utf8 = p.u8string();
            manifest.write(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            manifest << '\n';
        }
        if (images.empty()) throw std::runtime_error("No images found");
        std::cout << "dataset=" << paths.size() << " sampled=" << images.size() << " crop=" << crop
                  << " rounds=" << rounds << " conf=0.25 iou=0.45 maxDetections=99 cvThreads=" << cv::getNumThreads() << '\n';
        const auto loadStart = Clock::now();
        if (std::stoi(argv[7]) != 1) throw std::runtime_error("TensorRT requires GPU=1"); BenchDetector detector(ansi(argv[1]));
        detector.setMaxDetections(99);
        detector.describe();
        std::cout << "load_ms=" << elapsed(loadStart, Clock::now()) << '\n';
        for (int i = 0; i < 50; ++i) detector.detectBorrowed(images[i % images.size()], 0.25f, 0.45f);
        std::cout << "cuda_graph=" << bool(::detector->graphExec)
                  << " blocking_sync=" << ::detector->blockingSync
                  << " input_bytes=" << ::detector->inputBytes
                  << " scratch_bytes=" << ::detector->preprocessing.blob.capacity() * sizeof(float) << '\n';
        std::ofstream csv(std::filesystem::path(std::wstring(argv[6]) + L".csv"));
        std::ofstream output(std::filesystem::path(std::wstring(argv[6]) + L".detections.bin"), std::ios::binary);
        csv << "round,image,preprocess_ms,inference_ms,postprocess_ms,total_ms,detections\n" << std::setprecision(10);
        std::vector<double> pre, infer, post, total;
        const double cpuStart = processCpuMs();
        const auto batchStart = Clock::now();
        for (int r = 0; r < rounds; ++r) for (size_t i = 0; i < images.size(); ++i) {
            Timing t{};
            const auto& dets = detector.timed(images[i], t);
            csv << r << ',' << i << ',' << t.preprocess << ',' << t.inference << ',' << t.postprocess << ',' << t.total << ',' << dets.size() << '\n';
            if (r == 0) {
                write(output, static_cast<uint32_t>(dets.size()));
                for (auto& d : dets) {
                    write(output, d.box.x); write(output, d.box.y); write(output, d.box.width); write(output, d.box.height);
                    write(output, d.classId); write(output, d.conf);
                }
            }
            pre.push_back(t.preprocess); infer.push_back(t.inference); post.push_back(t.postprocess); total.push_back(t.total);
        }
        const auto batchEnd = Clock::now();
        const double cpuMs = processCpuMs() - cpuStart;
        const double batchMs = elapsed(batchStart, batchEnd);
        std::cout << std::fixed << std::setprecision(6);
        stats("total", total);
        std::cout << "model_fps=" << total.size()*1000.0/std::accumulate(total.begin(), total.end(), 0.0)
                  << " batch_wall_ms=" << batchMs << " cpu_ms=" << cpuMs
                  << " cpu_core_equivalents=" << cpuMs / batchMs << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
