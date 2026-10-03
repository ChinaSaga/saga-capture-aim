#include "TrtRuntime.h"
#include <filesystem>
#include <vector>
#include <stdexcept>

namespace trt {
std::wstring widePath(const char* path) {
    if (!path) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (n < 1) throw std::runtime_error("Invalid model path");
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path, -1, w.data(), n); w.pop_back(); return w;
}
static std::vector<std::filesystem::path> directories() {
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    auto base = std::filesystem::path(exe).parent_path();
    std::vector<std::filesystem::path> paths{base, base / L"TensorRT", base / L"TensorRT" / L"bin"};
    for (auto root = base; !root.empty();) {
        paths.push_back(root / L".deps" / L"tensorrt-11.3.0.99" / L"bin");
        auto parent = root.parent_path(); if (parent == root) break; root = parent;
    }
    wchar_t cuda[32768]{};
    if (GetEnvironmentVariableW(L"CUDA_PATH", cuda, 32768)) paths.push_back(std::filesystem::path(cuda) / L"bin");
    paths.emplace_back(L"C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.4/bin");
    // Recent CUDA Windows packages put runtime DLLs in bin/x64.
    const size_t n = paths.size(); for (size_t i = 0; i < n; ++i) paths.push_back(paths[i] / L"x64");
    return paths;
}
static HMODULE load(const wchar_t* name, bool required = true) {
    const auto paths = directories();
    // Retain directory cookies: TensorRT loads architecture-specific builder
    // resources by name later, and CUDA DLLs load their own siblings.
    static std::vector<DLL_DIRECTORY_COOKIE> cookies;
    static std::once_flag once;
    std::call_once(once, [&] { for (auto& p : paths) if (std::filesystem::is_directory(p)) {
        auto cookie = AddDllDirectory(p.c_str()); if (cookie) cookies.push_back(cookie);
    }});
    for (auto& p : paths) if (std::filesystem::is_regular_file(p / name)) {
        if (auto m = LoadLibraryExW((p / name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return m;
    }
    if (auto m = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return m;
    if (!required) return nullptr;
    std::string s; for (auto p = name; *p; ++p) s.push_back(static_cast<char>(*p)); // DLL names above are ASCII.
    throw std::runtime_error("Missing or incompatible NVIDIA library " + s + ". Install TensorRT 11.3 (CUDA 13.4) or put its DLLs beside the EXE. Windows error=" + std::to_string(GetLastError()));
}
template<class T> static T symbol(HMODULE module, const char* name) {
    auto p = GetProcAddress(module, name); if (!p) throw std::runtime_error(std::string("NVIDIA library lacks symbol ") + name);
    return reinterpret_cast<T>(p);
}
void RuntimeLibrary::ensure(bool parser) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!cudaEventDestroy) {
        cuda_ = load(L"cudart64_13.dll");
#define TRT_CUDA_BIND(name) name = symbol<decltype(name)>(cuda_, #name)
        TRT_CUDA_BIND(cudaMalloc); TRT_CUDA_BIND(cudaFree); TRT_CUDA_BIND(cudaHostAlloc); TRT_CUDA_BIND(cudaFreeHost);
        TRT_CUDA_BIND(cudaMemcpyAsync); TRT_CUDA_BIND(cudaStreamCreateWithFlags); TRT_CUDA_BIND(cudaStreamSynchronize);
        TRT_CUDA_BIND(cudaStreamDestroy); TRT_CUDA_BIND(cudaSetDevice); TRT_CUDA_BIND(cudaGetDeviceCount); TRT_CUDA_BIND(cudaGetErrorString);
        TRT_CUDA_BIND(cudaStreamBeginCapture); TRT_CUDA_BIND(cudaStreamEndCapture); TRT_CUDA_BIND(cudaGraphInstantiate);
        TRT_CUDA_BIND(cudaGraphLaunch); TRT_CUDA_BIND(cudaGraphDestroy); TRT_CUDA_BIND(cudaGraphExecDestroy);
        TRT_CUDA_BIND(cudaEventCreateWithFlags); TRT_CUDA_BIND(cudaEventRecord); TRT_CUDA_BIND(cudaEventSynchronize); TRT_CUDA_BIND(cudaEventDestroy);
#undef TRT_CUDA_BIND
    }
    if (!runtimeFactory_ || !builderFactory_) {
        infer_ = load(L"nvinfer_11.dll");
        runtimeFactory_ = symbol<Factory>(infer_, "createInferRuntime_INTERNAL");
        builderFactory_ = symbol<Factory>(infer_, "createInferBuilder_INTERNAL");
        plugin_ = load(L"nvinfer_plugin_11.dll", false);
    }
    if (parser && !parserFactory_) {
        parser_ = load(L"nvonnxparser_11.dll");
        parserFactory_ = symbol<ParserFactory>(parser_, "createNvOnnxParser_INTERNAL");
    }
}
nvinfer1::IRuntime* RuntimeLibrary::createRuntime(nvinfer1::ILogger& logger) { ensure();
    if (plugin_) { using Init = bool (*)(void*, char const*); if (auto init = reinterpret_cast<Init>(GetProcAddress(plugin_, "initLibNvInferPlugins"))) init(&logger, ""); }
    return static_cast<nvinfer1::IRuntime*>(runtimeFactory_(&logger, NV_TENSORRT_VERSION)); }
nvinfer1::IBuilder* RuntimeLibrary::createBuilder(nvinfer1::ILogger& logger) { ensure();
    if (plugin_) { using Init = bool (*)(void*, char const*); if (auto init = reinterpret_cast<Init>(GetProcAddress(plugin_, "initLibNvInferPlugins"))) init(&logger, ""); }
    return static_cast<nvinfer1::IBuilder*>(builderFactory_(&logger, NV_TENSORRT_VERSION)); }
nvonnxparser::IParser* RuntimeLibrary::createParser(nvinfer1::INetworkDefinition& network, nvinfer1::ILogger& logger) { ensure(true); return static_cast<nvonnxparser::IParser*>(parserFactory_(&network, &logger, NV_ONNX_PARSER_VERSION)); }
RuntimeLibrary& library() { static RuntimeLibrary value; return value; }
void checkCuda(cudaError_t result, const char* operation) { if (result != cudaSuccess) {
    auto& api = library(); const char* msg = api.cudaGetErrorString ? api.cudaGetErrorString(result) : "CUDA error";
    throw std::runtime_error(std::string(operation) + ": " + msg);
}}
}
