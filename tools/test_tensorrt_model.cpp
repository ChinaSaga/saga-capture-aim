// CUDA-backed integration test for the production engine API. No mouse or UI.
// Build: tools/build_performance.ps1 -Source tools/test_tensorrt_model.cpp -Name test_tensorrt_model
// Run only when the GPU is free: test_tensorrt_model MODEL.trt IMAGE
// Copy this EXE beside the deployed DLLs to verify deployment resolution.
#include "../src/TensorRT.cpp"
#include "../src/TrtRuntime.cpp"
#include <array>
#include <iostream>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string utf8(const std::wstring& value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string modelPath(const std::filesystem::path& path) {
    const auto value = path.wstring();
    int count = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    require(count > 0, "Cannot represent the model path in the application character set");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    require(trt::widePath(result.c_str()) == value, "Model path would lose Unicode characters in the application API");
    return result;
}
using Objects = std::vector<DetectObject>;
struct Results {
    std::array<DetectObject, 101> objects;
    std::array<unsigned char, sizeof(DetectObject)> guard;
    Results() {
        std::memset(objects.data(), 0xa5, sizeof(objects));
        std::memcpy(guard.data(), &objects[99], guard.size());
    }
    Objects finish(int count) const {
        require(count >= 0, std::string("Detection failed: ") + saga::trt_last_error());
        require(count <= 99, "Production target limit of 99 was exceeded");
        require(!std::memcmp(&objects[99], guard.data(), guard.size()) &&
                !std::memcmp(&objects[100], guard.data(), guard.size()), "Result buffer guard after out[98] was overwritten");
        return Objects(objects.begin(), objects.begin() + count);
    }
};
Objects direct(const cv::Mat& image) {
    Results result;
    return result.finish(trtDetectBgr(image.data, image.cols, image.rows, image.step, .25f, .45f, result.objects.data()));
}
Objects encoded(const std::vector<unsigned char>& bytes) {
    Results result;
    return result.finish(saga::trt_detect(bytes.data(), static_cast<int>(bytes.size()), .25f, .45f, result.objects.data()));
}
void same(const Objects& expected, const Objects& actual, const char* stage) {
    require(expected.size() == actual.size(), std::string(stage) + ": detection count changed");
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto& a = expected[i]; const auto& b = actual[i];
        require(a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
                a.label == b.label && a.prob == b.prob, std::string(stage) + ": detection values changed at index " + std::to_string(i));
    }
}
std::vector<unsigned char> rawOutput() {
    require(detector && detector->cpuOutput && detector->outputBytes, "No production output buffer");
    const auto* data = static_cast<const unsigned char*>(detector->cpuOutput);
    return {data, data + detector->outputBytes};
}
std::vector<unsigned char> topDownBmp(const cv::Mat& image) {
    const size_t stride = (size_t(image.cols) * 3 + 3) & ~size_t(3);
    std::vector<unsigned char> bytes(54 + stride * image.rows, 0);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42; file.bfOffBits = 54; file.bfSize = static_cast<DWORD>(bytes.size());
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info); info.biWidth = image.cols; info.biHeight = -image.rows;
    info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = static_cast<DWORD>(stride * image.rows);
    static_assert(sizeof(file) == 14 && sizeof(info) == 40);
    std::memcpy(bytes.data(), &file, sizeof(file)); std::memcpy(bytes.data() + 14, &info, sizeof(info));
    for (int y = 0; y < image.rows; ++y) std::memcpy(bytes.data() + 54 + stride * y, image.ptr(y), size_t(image.cols) * 3);
    return bytes;
}
struct Environment {
    std::wstring previous;
    bool existed = false;
    Environment() {
        DWORD size = GetEnvironmentVariableW(L"SAGA_TRT_CUDA_GRAPH", nullptr, 0);
        if (size) { previous.resize(size); GetEnvironmentVariableW(L"SAGA_TRT_CUDA_GRAPH", previous.data(), size); previous.pop_back(); existed = true; }
    }
    ~Environment() { SetEnvironmentVariableW(L"SAGA_TRT_CUDA_GRAPH", existed ? previous.c_str() : nullptr); }
    void enabled(bool value) { require(SetEnvironmentVariableW(L"SAGA_TRT_CUDA_GRAPH", value ? L"1" : L"0") != FALSE, "Cannot configure the graph test environment"); }
};
struct Cleanup { ~Cleanup() { saga::trt_destroy(); } };
struct Fixtures {
    std::filesystem::path directory, missing, empty, corrupt;
    Fixtures() {
        wchar_t temporary[32768]{};
        require(GetTempPathW(32768, temporary) > 0, "Cannot locate temporary directory");
        directory = std::filesystem::path(temporary) / (L"SagaTrtTest_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(directory), "Cannot create a unique test fixture directory");
        missing = directory / L"missing.trt"; empty = directory / L"empty.trt"; corrupt = directory / L"corrupt.trt";
        std::ofstream(empty, std::ios::binary).close();
        std::ofstream file(corrupt, std::ios::binary); file << "not a TensorRT engine"; file.close();
        require(std::filesystem::file_size(empty) == 0 && std::filesystem::file_size(corrupt) > 0, "Fixture creation failed");
    }
    ~Fixtures() {
        // Delete only the two known files created here; never recursively delete.
        DeleteFileW(empty.c_str()); DeleteFileW(corrupt.c_str()); RemoveDirectoryW(directory.c_str());
    }
};
void load(const std::string& path) {
    const int status = saga::trt_create(path.c_str());
    require(status == 0, std::string("Cannot load the valid model: ") + saga::trt_last_error());
}
void module(const wchar_t* name) {
    const auto loaded = GetModuleHandleW(name);
    require(loaded != nullptr, "Expected NVIDIA runtime DLL was not loaded");
    wchar_t filename[32768]{};
    require(GetModuleFileNameW(loaded, filename, 32768) > 0, "Cannot retrieve loaded runtime path");
    std::cout << "loaded " << utf8(name) << "=" << utf8(filename) << '\n';
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::cerr << "Usage: test_tensorrt_model MODEL.trt IMAGE\n"; return 2; }
    Cleanup cleanup;
    try {
        const auto engine = std::filesystem::canonical(argv[1]);
        const auto originalSize = std::filesystem::file_size(engine);
        const auto originalTime = std::filesystem::last_write_time(engine);
        const std::string path = modelPath(engine);
        std::ifstream file(std::filesystem::path(argv[2]), std::ios::binary);
        std::vector<unsigned char> imageBytes((std::istreambuf_iterator<char>(file)), {});
        const cv::Mat image = cv::imdecode(imageBytes, cv::IMREAD_COLOR);
        require(!image.empty(), "Cannot decode the test image");
        cv::Mat storage(image.rows, image.cols + 23, CV_8UC3, cv::Scalar(17, 41, 63));
        cv::Mat padded = storage(cv::Rect(7, 0, image.cols, image.rows)); image.copyTo(padded);
        require(padded.step > size_t(padded.cols) * 3, "The padded image was unexpectedly contiguous");
        std::vector<unsigned char> bmp;
        require(cv::imencode(".bmp", image, bmp), "Could not encode the lossless BMP fixture");
        const auto topDown = topDownBmp(image);
        // A strongly different image with the same size verifies each graph
        // replay reads the current pinned input, even when detections are empty.
        const cv::Mat changed(image.size(), CV_8UC3, cv::Scalar(0, 0, 0));
        Environment graph;
        graph.enabled(false);
        load(path);
        const auto reference = direct(image); const auto originalRaw = rawOutput();
        const auto changedReference = direct(changed); const auto changedRaw = rawOutput();
        require(originalRaw != changedRaw, "Choose a non-black test image: raw outputs did not change");
        saga::trt_destroy();
        graph.enabled(true);
        for (int cycle = 0; cycle < 3; ++cycle) {
            load(path);
            same(reference, direct(image), "reload first frame");
            require(detector->graphExec != nullptr, "The validated model did not capture a CUDA graph; check driver/runtime support");
            same(reference, direct(padded), "noncontiguous stride / CUDA graph");
            require(rawOutput() == originalRaw, "Graph replay changed raw output for the original image");
            same(reference, encoded(bmp), "encoded BMP wrapper");
            same(reference, encoded(topDown), "top-down BMP wrapper fast path");
            same(changedReference, direct(changed), "changed content / CUDA graph");
            require(rawOutput() == changedRaw, "CUDA graph reused stale image data");
            same(reference, direct(image), "restore original content / CUDA graph");
            if (cycle == 0) { module(L"nvinfer_11.dll"); module(L"cudart64_13.dll"); }
            saga::trt_destroy();
        }
        std::cout << "PASS direct BGR, padded stride, encoded/top-down BMP, out[99] guard, fresh CUDA graph inputs and three reload/destroy cycles\n";
        Fixtures fixtures;
        for (const auto& invalid : {fixtures.missing, fixtures.empty, fixtures.corrupt}) {
            const auto badPath = modelPath(invalid);
            require(saga::trt_create(badPath.c_str()) != 0, "Invalid engine unexpectedly loaded");
            require(saga::trt_last_error() && *saga::trt_last_error(), "Invalid engine load did not return a diagnostic");
            std::cout << "expected failure " << utf8(invalid.filename().wstring()) << ": " << saga::trt_last_error() << '\n';
            load(path);
            same(reference, direct(image), "reload after failed create");
            saga::trt_destroy();
        }
        Results notReady;
        require(trtDetectBgr(image.data, image.cols, image.rows, image.step, .25f, .45f, notReady.objects.data()) < 0,
                "Detect after destroy unexpectedly succeeded");
        require(*saga::trt_last_error(), "Detect after destroy did not return a diagnostic");
        require(std::filesystem::file_size(engine) == originalSize && std::filesystem::last_write_time(engine) == originalTime,
                "The original model file changed during the test");
        std::cout << "PASS missing/empty/corrupt diagnostics, recovery, destroyed-state guard and read-only model; detections=" << reference.size() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
