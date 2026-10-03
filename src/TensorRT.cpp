#include "Inference.h"
#include "TrtRuntime.h"
#include "preprocessing.hpp"
#include "nms.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <immintrin.h>
#include <chrono>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
class TrtLogger : public nvinfer1::ILogger {
public:
    std::string error() { std::lock_guard<std::mutex> guard(lock_); return error_; }
    void log(Severity severity, const char* message) noexcept override {
        if (severity <= Severity::kERROR && message) try { std::lock_guard<std::mutex> guard(lock_); error_ = message; } catch (...) {}
    }
private:
    std::mutex lock_;
    std::string error_;
};
static size_t elements(const nvinfer1::Dims& dims) {
    if (dims.nbDims < 1 || dims.nbDims > 8) throw std::runtime_error("Invalid TensorRT tensor dimensions");
    size_t count = 1;
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] <= 0 || size_t(dims.d[i]) > 536870912 / count) throw std::runtime_error("Unresolved or oversized TensorRT tensor. Reconvert with a fixed input size.");
        count *= size_t(dims.d[i]);
    }
    return count;
}
static size_t bytesPer(nvinfer1::DataType type) {
    if (type == nvinfer1::DataType::kFLOAT) return 4;
    if (type == nvinfer1::DataType::kHALF) return 2;
    throw std::runtime_error("TensorRT engine I/O must be FP32 or FP16. Reconvert the original detection ONNX model.");
}
class Detector {
public:
    TrtLogger logger;
    std::unique_ptr<nvinfer1::IRuntime> runtime;
    std::unique_ptr<nvinfer1::ICudaEngine> engine;
    std::unique_ptr<nvinfer1::IExecutionContext> context;
    void* gpuInput = nullptr; void* gpuOutput = nullptr;
    void* cpuInput = nullptr; void* cpuOutput = nullptr;
    cudaStream_t stream = nullptr;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t graphExec = nullptr;
    bool graphAttempted = false;
    cudaEvent_t completion = nullptr;
    cudaEvent_t profileStart = nullptr, profileEnd = nullptr;
    TrtTiming timing;
    bool blockingSync = false;
    Microsoft::WRL::ComPtr<ID3D11Device> powerDevice;

    void attachPowerProfile(int device) noexcept {
        // On this WDDM laptop the driver's per-application power policy is
        // ignored by a pure CUDA process. A D3D device on the SAME adapter lets
        // the driver recognize its graphics application profile. It submits no
        // draws, owns no swap chain and never changes global clocks/settings.
        // A profile must be configured separately; this does not force P0 on
        // other users' machines. Failure must not disable TensorRT inference.
        try {
            char disabled[16]{};
            if (GetEnvironmentVariableA("SAGA_TRT_POWER_PROFILE", disabled, sizeof(disabled)) && !strcmp(disabled,"0")) return;
            // Public CUDA driver API: CUdevice/CUresult are integers. CUDA has
            // already loaded nvcuda.dll through cudaSetDevice; do not load an
            // extra driver module or require a CUDA Toolkit driver import lib.
            const auto driver = GetModuleHandleW(L"nvcuda.dll");
            if (!driver) return;
            using GetDevice = int (WINAPI*)(int*, int);
            using GetLuid = int (WINAPI*)(char*, unsigned*, int);
            const auto getDevice = reinterpret_cast<GetDevice>(GetProcAddress(driver, "cuDeviceGet"));
            const auto getLuid = reinterpret_cast<GetLuid>(GetProcAddress(driver, "cuDeviceGetLuid"));
            if (!getDevice || !getLuid) return;
            char luid[8]{}; unsigned nodeMask = 0;
            int cudaDevice = 0;
            if (getDevice(&cudaDevice, device) != 0 || getLuid(luid, &nodeMask, cudaDevice) != 0) return;
            Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
            for (UINT i = 0;; ++i) {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                if (FAILED(factory->EnumAdapters1(i, &adapter))) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc)) || memcmp(&desc.AdapterLuid, luid, sizeof(luid))) continue;
                D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                    nullptr, 0, D3D11_SDK_VERSION, &powerDevice, nullptr, nullptr);
                break;
            }
        } catch (...) {}
    }
    nvinfer1::DataType inputType{}, outputType{};
    size_t inputCount = 0, outputCount = 0, inputBytes = 0, outputBytes = 0;
    cv::Size shape;
    int features = 0, candidates = 0;
    yolos::preprocessing::InferenceBuffer preprocessing;
    std::vector<float> decoded;
    std::vector<yolos::BoundingBox> boxes;
    std::vector<float> scores;
    std::vector<int> labels, indices;
    yolos::nms::NMSWorkspace nms;

    ~Detector() {
        auto& api = trt::library();
        if (stream) api.cudaStreamSynchronize(stream);
        if (graphExec) api.cudaGraphExecDestroy(graphExec);
        if (graph) api.cudaGraphDestroy(graph);
        if (completion) api.cudaEventDestroy(completion);
        if (profileStart) api.cudaEventDestroy(profileStart);
        if (profileEnd) api.cudaEventDestroy(profileEnd);
        context.reset(); engine.reset(); runtime.reset();
        if (gpuInput) api.cudaFree(gpuInput);
        if (gpuOutput) api.cudaFree(gpuOutput);
        if (cpuInput) api.cudaFreeHost(cpuInput);
        if (cpuOutput) api.cudaFreeHost(cpuOutput);
        if (stream) api.cudaStreamDestroy(stream);
    }
    void load(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Cannot open TensorRT engine file");
        const auto length = input.tellg();
        if (length < 1 || length > std::streamoff(2ull * 1024 * 1024 * 1024)) throw std::runtime_error("Invalid TensorRT engine file size");
        std::vector<char> data(static_cast<size_t>(length)); input.seekg(0);
        if (!input.read(data.data(), data.size())) throw std::runtime_error("Cannot read TensorRT engine file");
        auto& api = trt::library(); api.ensure();
        int devices = 0; trt::checkCuda(api.cudaGetDeviceCount(&devices), "CUDA device discovery");
        if (!devices) throw std::runtime_error("TensorRT requires a supported NVIDIA GPU");
        trt::checkCuda(api.cudaSetDevice(0), "Select NVIDIA GPU");
        attachPowerProfile(0);
        runtime.reset(api.createRuntime(logger));
        if (!runtime) throw std::runtime_error("Cannot create TensorRT runtime: " + logger.error());
        engine.reset(runtime->deserializeCudaEngine(data.data(), data.size()));
        if (!engine) throw std::runtime_error("Engine load failed. Reconvert ONNX on this GPU with TensorRT 11.3: " + logger.error());
        if (engine->getNbIOTensors() != 2) throw std::runtime_error("Only one-input, one-output raw YOLO detection engines are supported; export without embedded NMS, segmentation or pose outputs.");
        std::string inName, outName;
        for (int i = 0; i < 2; ++i) {
            const char* name = engine->getIOTensorName(i);
            if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) { if (!inName.empty()) throw std::runtime_error("Engine has multiple inputs"); inName = name; }
            else { if (!outName.empty()) throw std::runtime_error("Engine has multiple outputs"); outName = name; }
            if (engine->getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR || engine->getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE) throw std::runtime_error("TensorRT I/O must use linear device tensors");
        }
        if (inName.empty() || outName.empty()) throw std::runtime_error("Missing YOLO input/output");
        context.reset(engine->createExecutionContext());
        if (!context) throw std::runtime_error("Cannot create TensorRT execution context: " + logger.error());
        auto inputShape = engine->getTensorShape(inName.c_str());
        bool dynamic = false; for (int i = 0; i < inputShape.nbDims; ++i) dynamic |= inputShape.d[i] < 0;
        if (dynamic) inputShape = engine->getProfileShape(inName.c_str(), 0, nvinfer1::OptProfileSelector::kOPT);
        if (inputShape.nbDims != 4 || inputShape.d[0] != 1 || inputShape.d[1] != 3 || inputShape.d[2] < 1 || inputShape.d[3] < 1 || inputShape.d[2] > 4096 || inputShape.d[3] > 4096) throw std::runtime_error("Input must be NCHW [1,3,H,W], H/W <=4096. Reconvert with a fixed size.");
        if (dynamic && !context->setInputShape(inName.c_str(), inputShape)) throw std::runtime_error("Cannot set the engine optimization-profile input size");
        const auto outputShape = context->getTensorShape(outName.c_str());
        if (outputShape.nbDims != 3 || outputShape.d[0] != 1 || outputShape.d[1] < 5 || outputShape.d[1] > 4100 || outputShape.d[2] <= outputShape.d[1]) throw std::runtime_error("Output must be raw YOLOv8/v11 channels-first [1,4+classes,boxes]. Legacy objectness, row-major, embedded NMS, pose and segmentation outputs require a different export.");
        inputType = engine->getTensorDataType(inName.c_str()); outputType = engine->getTensorDataType(outName.c_str());
        inputCount = elements(inputShape); outputCount = elements(outputShape);
        inputBytes = inputCount * bytesPer(inputType); outputBytes = outputCount * bytesPer(outputType);
        shape = cv::Size(int(inputShape.d[3]), int(inputShape.d[2])); features = int(outputShape.d[1]); candidates = int(outputShape.d[2]);
        trt::checkCuda(api.cudaMalloc(&gpuInput, inputBytes), "Allocate input GPU buffer");
        trt::checkCuda(api.cudaMalloc(&gpuOutput, outputBytes), "Allocate output GPU buffer");
        trt::checkCuda(api.cudaHostAlloc(&cpuInput, inputBytes, cudaHostAllocDefault), "Allocate pinned input buffer");
        trt::checkCuda(api.cudaHostAlloc(&cpuOutput, outputBytes, cudaHostAllocDefault), "Allocate pinned output buffer");
        trt::checkCuda(api.cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), "Create inference stream");
        if (!context->setTensorAddress(inName.c_str(), gpuInput) || !context->setTensorAddress(outName.c_str(), gpuOutput)) throw std::runtime_error("Cannot bind persistent TensorRT buffers");
        preprocessing.ensureCapacity(shape.height, shape.width, 3);
        if (outputType == nvinfer1::DataType::kHALF) decoded.resize(outputCount);
        boxes.reserve(256); scores.reserve(256); labels.reserve(256); indices.reserve(99);
        char graphSetting[16]{};
        graphAttempted = GetEnvironmentVariableA("SAGA_TRT_CUDA_GRAPH", graphSetting, sizeof(graphSetting)) && !strcmp(graphSetting, "0");
        char blockingSetting[16]{};
        blockingSync = GetEnvironmentVariableA("SAGA_TRT_BLOCKING_SYNC", blockingSetting, sizeof(blockingSetting)) && !strcmp(blockingSetting, "1");
        // Default event synchronization spins for short, latency-sensitive
        // inference. Explicit =1 preserves the lower-CPU blocking option.
        const unsigned completionFlags = cudaEventDisableTiming | (blockingSync ? cudaEventBlockingSync : 0);
        trt::checkCuda(api.cudaEventCreateWithFlags(&completion, completionFlags), "Create inference completion event");
        char profileSetting[16]{};
        if (GetEnvironmentVariableA("SAGA_TRT_PROFILE", profileSetting, sizeof(profileSetting)) && !strcmp(profileSetting, "1")) {
            trt::checkCuda(api.cudaEventCreateWithFlags(&profileStart, cudaEventDefault), "Create profiling start event");
            trt::checkCuda(api.cudaEventCreateWithFlags(&profileEnd, cudaEventDefault), "Create profiling end event");
        }
    }
    void enqueueCopies() {
        auto& api = trt::library();
        trt::checkCuda(api.cudaMemcpyAsync(gpuInput, cpuInput, inputBytes, cudaMemcpyHostToDevice, stream), "Upload YOLO input");
        if (!context->enqueueV3(stream)) throw std::runtime_error("TensorRT inference failed: " + logger.error());
        trt::checkCuda(api.cudaMemcpyAsync(cpuOutput, gpuOutput, outputBytes, cudaMemcpyDeviceToHost, stream), "Read YOLO output");
    }
    void captureGraph() {
        graphAttempted = true;
        auto& api = trt::library();
        bool capturing = false;
        try {
            trt::checkCuda(api.cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal), "Begin CUDA graph capture");
            capturing = true;
            enqueueCopies();
            const auto ended = api.cudaStreamEndCapture(stream, &graph); capturing = false;
            trt::checkCuda(ended, "End CUDA graph capture");
            trt::checkCuda(api.cudaGraphInstantiate(&graphExec, graph, 0), "Instantiate CUDA graph");
        } catch (const std::exception& e) {
            // The first ordinary inference already succeeded. Unsupported capture
            // falls back to ordinary enqueue; future inference errors still throw.
            if (capturing) { cudaGraph_t abandoned = nullptr; api.cudaStreamEndCapture(stream, &abandoned); if (abandoned) api.cudaGraphDestroy(abandoned); }
            if (graphExec) { api.cudaGraphExecDestroy(graphExec); graphExec = nullptr; }
            if (graph) { api.cudaGraphDestroy(graph); graph = nullptr; }
            const std::string warning = std::string("TensorRT CUDA graph unavailable; ordinary inference retained: ") + e.what() + "\n";
            OutputDebugStringA(warning.c_str());
        }
    }
    int detect(const cv::Mat& image, float conf, float iou, DetectObject* out) {
        using Clock = std::chrono::steady_clock;
        const bool profiling = profileStart != nullptr;
        const auto t0 = profiling ? Clock::now() : Clock::time_point{};
        cv::Size actual;
        yolos::preprocessing::letterBoxToBlob(image, preprocessing, 3, shape, actual, false);
        if (inputType == nvinfer1::DataType::kFLOAT) memcpy(cpuInput, preprocessing.blob.data(), inputBytes);
        else { auto p = static_cast<unsigned short*>(cpuInput); size_t i = 0;
            for (; i + 8 <= inputCount; i += 8) _mm_storeu_si128(reinterpret_cast<__m128i*>(p + i), _mm256_cvtps_ph(_mm256_loadu_ps(preprocessing.blob.data() + i), 0));
            for (; i < inputCount; ++i) p[i] = static_cast<uint16_t>(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(preprocessing.blob[i]), 0), 0)); }
        auto& api = trt::library();
        const auto t1 = profiling ? Clock::now() : Clock::time_point{};
        if (profiling) trt::checkCuda(api.cudaEventRecord(profileStart, stream), "Record profiling start");
        if (graphExec) trt::checkCuda(api.cudaGraphLaunch(graphExec, stream), "Launch YOLO CUDA graph");
        else enqueueCopies();
        if (profiling) trt::checkCuda(api.cudaEventRecord(profileEnd, stream), "Record profiling end");
        const auto t2 = profiling ? Clock::now() : Clock::time_point{};
        if (completion) {
            // Record outside capture, after all copies/kernels (or graph replay).
            // Event flags select spin or blocking without device-wide flags.
            trt::checkCuda(api.cudaEventRecord(completion, stream), "Record YOLO inference completion");
            trt::checkCuda(api.cudaEventSynchronize(completion), "Wait for YOLO completion");
        } else trt::checkCuda(api.cudaStreamSynchronize(stream), "Wait for YOLO inference");
        const auto t3 = profiling ? Clock::now() : Clock::time_point{};
        if (!graphAttempted) captureGraph();
        const float* values = static_cast<const float*>(cpuOutput);
        if (outputType == nvinfer1::DataType::kHALF) { const auto p = static_cast<const unsigned short*>(cpuOutput); size_t i = 0;
            for (; i + 8 <= outputCount; i += 8) _mm256_storeu_ps(decoded.data() + i, _mm256_cvtph_ps(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i))));
            for (; i < outputCount; ++i) decoded[i] = _mm_cvtss_f32(_mm_cvtph_ps(_mm_cvtsi32_si128(p[i]))); values = decoded.data(); }
        float scale, padX, padY; yolos::preprocessing::getScalePad(image.size(), shape, scale, padX, padY); const float inv = 1.f / scale;
        boxes.clear(); scores.clear(); labels.clear();
        for (int d = 0; d < candidates; ++d) {
            float score = values[4 * candidates + d]; int label = 0;
            for (int c = 5; c < features; ++c) if (values[c * candidates + d] > score) { score = values[c * candidates + d]; label = c - 4; }
            if (!std::isfinite(score) || score <= conf) continue;
            const float cx = values[d], cy = values[candidates + d], w = values[2 * candidates + d], h = values[3 * candidates + d];
            if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) continue;
            const float x = (cx - w * .5f - padX) * inv, y = (cy - h * .5f - padY) * inv;
            // Bounds before integer conversion avoid undefined conversion of
            // malformed engine output while preserving normal YOLO rounding.
            if (std::fabs(x) > 1e8f || std::fabs(y) > 1e8f || w * inv > 1e8f || h * inv > 1e8f) continue;
            yolos::BoundingBox box;
            box.x = std::clamp(int(x), 0, image.cols - 1); box.y = std::clamp(int(y), 0, image.rows - 1);
            box.width = std::clamp(int(w * inv), 1, image.cols - box.x); box.height = std::clamp(int(h * inv), 1, image.rows - box.y);
            boxes.push_back(box); scores.push_back(score); labels.push_back(label);
        }
        yolos::nms::NMSBoxesBatched(boxes, scores, labels, conf, iou, indices, nms, 99);
        int count = 0; for (int i : indices) { if (count == 99) break; const auto& box = boxes[i]; out[count++] = {float(box.x), float(box.y), float(box.width), float(box.height), labels[i], scores[i]}; }
        if (profiling) {
            const auto t4 = Clock::now();
            auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b-a).count(); };
            timing.preprocessMs = ms(t0,t1); timing.submitMs = ms(t1,t2);
            timing.waitMs = ms(t2,t3); timing.postprocessMs = ms(t3,t4);
            timing.graphActive = graphExec != nullptr; timing.blockingSync = blockingSync;
            trt::checkCuda(api.cudaEventElapsedTime(&timing.gpuMs, profileStart, profileEnd), "Read GPU profiling time");
        }
        return count;
    }
};
std::unique_ptr<Detector> detector;
std::string lastError;
}
TrtTiming trtLastTiming() { return detector ? detector->timing : TrtTiming{}; }
namespace saga {
int trt_create(const char* path) { detector.reset(); try { auto value = std::make_unique<Detector>(); value->load(trt::widePath(path)); detector = std::move(value); lastError.clear(); return 0; } catch (const std::exception& e) { lastError = e.what(); return -1; } }
void trt_destroy() { detector.reset(); }
const char* trt_last_error() { return lastError.c_str(); }
int trt_detect(const unsigned char* data, int size, float conf, float iou, DetectObject* out) {
    if (!data || size < 1 || !out) { lastError = "Invalid TensorRT encoded image or result buffer"; return -1; }
    try { cv::Mat image;
        if (size >= 54 && data[0] == 'B' && data[1] == 'M') {
            int off, w, h, compression; short bpp; memcpy(&off, data+10, 4); memcpy(&w, data+18, 4); memcpy(&h, data+22, 4); memcpy(&bpp, data+28, 2); memcpy(&compression, data+30, 4);
            if (off >= 54 && w > 0 && w <= 32768 && h < 0 && h >= -32768 && bpp == 24 && compression == 0) {
                const size_t stride = (size_t(w) * 3 + 3) & ~size_t(3);
                if (size_t(off) + stride * size_t(-h) <= size_t(size)) image = cv::Mat(-h, w, CV_8UC3, const_cast<unsigned char*>(data + off), stride);
            }
        }
        if (image.empty()) image = cv::imdecode(cv::Mat(1, size, CV_8UC1, const_cast<unsigned char*>(data)), cv::IMREAD_COLOR);
        if (image.empty()) { lastError = "Cannot decode TensorRT input image"; return -2; }
        return trtDetectBgr(image.data, image.cols, image.rows, image.step, conf, iou, out);
    } catch (const std::exception& e) { lastError = e.what(); return -1; }
}
}
int trtDetectBgr(const unsigned char* pixels, int w, int h, size_t stride, float conf, float iou, DetectObject* out) {
    if (!detector || !pixels || !out || w < 1 || h < 1 || stride < size_t(w) * 3) { lastError = "TensorRT is not ready or the input frame is invalid"; return -1; }
    try { return detector->detect(cv::Mat(h, w, CV_8UC3, const_cast<unsigned char*>(pixels), stride), conf, iou, out); }
    catch (const std::exception& e) { lastError = e.what(); return -1; }
}
