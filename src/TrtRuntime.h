#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cuda_runtime_api.h>
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <mutex>
#include <string>

namespace trt {
// Lazily loaded native libraries. Merely starting the application does not
// load TensorRT or CUDA. Handles intentionally live until process exit, after
// all builder/runtime/context objects have been released.
class RuntimeLibrary {
public:
    void ensure(bool parser = false);
    nvinfer1::IRuntime* createRuntime(nvinfer1::ILogger& logger);
    nvinfer1::IBuilder* createBuilder(nvinfer1::ILogger& logger);
    nvonnxparser::IParser* createParser(nvinfer1::INetworkDefinition& network, nvinfer1::ILogger& logger);
    decltype(&::cudaMalloc) cudaMalloc = nullptr;
    decltype(&::cudaFree) cudaFree = nullptr;
    decltype(&::cudaHostAlloc) cudaHostAlloc = nullptr;
    decltype(&::cudaFreeHost) cudaFreeHost = nullptr;
    decltype(&::cudaMemcpyAsync) cudaMemcpyAsync = nullptr;
    decltype(&::cudaStreamCreateWithFlags) cudaStreamCreateWithFlags = nullptr;
    decltype(&::cudaStreamSynchronize) cudaStreamSynchronize = nullptr;
    decltype(&::cudaStreamDestroy) cudaStreamDestroy = nullptr;
    decltype(&::cudaSetDevice) cudaSetDevice = nullptr;
    decltype(&::cudaGetDeviceCount) cudaGetDeviceCount = nullptr;
    decltype(&::cudaGetErrorString) cudaGetErrorString = nullptr;
    decltype(&::cudaStreamBeginCapture) cudaStreamBeginCapture = nullptr;
    decltype(&::cudaStreamEndCapture) cudaStreamEndCapture = nullptr;
    decltype(&::cudaGraphInstantiate) cudaGraphInstantiate = nullptr;
    decltype(&::cudaGraphLaunch) cudaGraphLaunch = nullptr;
    decltype(&::cudaGraphDestroy) cudaGraphDestroy = nullptr;
    decltype(&::cudaGraphExecDestroy) cudaGraphExecDestroy = nullptr;
    decltype(&::cudaEventCreateWithFlags) cudaEventCreateWithFlags = nullptr;
    decltype(&::cudaEventRecord) cudaEventRecord = nullptr;
    decltype(&::cudaEventSynchronize) cudaEventSynchronize = nullptr;
    decltype(&::cudaEventDestroy) cudaEventDestroy = nullptr;
    decltype(&::cudaEventElapsedTime) cudaEventElapsedTime = nullptr;
private:
    std::mutex lock_;
    HMODULE cuda_ = nullptr, infer_ = nullptr, parser_ = nullptr, plugin_ = nullptr;
    using Factory = void* (*)(void*, int32_t) noexcept;
    using ParserFactory = void* (*)(void*, void*, int32_t) noexcept;
    Factory runtimeFactory_ = nullptr, builderFactory_ = nullptr;
    ParserFactory parserFactory_ = nullptr;
};
RuntimeLibrary& library();
void checkCuda(cudaError_t result, const char* operation);
std::wstring widePath(const char* path);
}
