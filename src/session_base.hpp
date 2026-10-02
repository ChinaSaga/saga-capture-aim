#pragma once

// ============================================================================
// YOLO ONNX Session Base
// ============================================================================
// Common ONNX Runtime session setup and management for all YOLO detectors.
//
// Author: YOLOs-CPP Team, https://github.com/Geekgineer/YOLOs-CPP
// ============================================================================

#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>
#include <dml_provider_factory.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "onnx_metadata.hpp"
#include "utils.hpp"
#include "version.hpp"

namespace yolos {

// ============================================================================
// OrtSessionBase - Common ONNX Runtime session management
// ============================================================================

/// @brief Base class for ONNX Runtime session management
/// Handles model loading, session configuration, and common inference setup
class OrtSessionBase {
public:
    /// @brief Constructor - loads and initializes the ONNX model
    /// @param modelPath Path to the ONNX model file
    /// @param useGPU Whether to use GPU (CUDA or DirectML) for inference
    /// @param numThreads Number of intra-op threads (0 = auto)
    OrtSessionBase(const std::string& modelPath, bool useGPU = false, int numThreads = 0)
        : env_(ORT_LOGGING_LEVEL_WARNING, "YOLOS") {
        
        initSession(modelPath, useGPU, numThreads);
    }

    virtual ~OrtSessionBase() = default;

    // Prevent copying
    OrtSessionBase(const OrtSessionBase&) = delete;
    OrtSessionBase& operator=(const OrtSessionBase&) = delete;

    // Allow moving
    OrtSessionBase(OrtSessionBase&&) = default;
    OrtSessionBase& operator=(OrtSessionBase&&) = default;

    /// @brief Get the input image shape expected by the model
    [[nodiscard]] cv::Size getInputShape() const noexcept { return inputShape_; }

    /// @brief Check if input shape is dynamic
    [[nodiscard]] bool isDynamicInputShape() const noexcept { return isDynamicInputShape_; }

    /// @brief Check if batch size is dynamic
    [[nodiscard]] bool isDynamicBatchSize() const noexcept { return isDynamicBatchSize_; }

    /// @brief Get the device being used for inference
    [[nodiscard]] const std::string& getDevice() const noexcept { return device_; }

    /// @brief Get the number of input nodes
    [[nodiscard]] size_t getNumInputNodes() const noexcept { return numInputNodes_; }

    /// @brief Get the number of output nodes
    [[nodiscard]] size_t getNumOutputNodes() const noexcept { return numOutputNodes_; }

    /// @brief Class names from ONNX custom metadata `names` (Ultralytics), if present.
    [[nodiscard]] const std::vector<std::string>& getExportedClassNamesFromMetadata() const noexcept {
        return exportedClassNamesFromMetadata_;
    }

protected:
    Ort::Env env_{nullptr};
    Ort::SessionOptions sessionOptions_{nullptr};
    Ort::Session session_{nullptr};

    // Input/output node names
    std::vector<Ort::AllocatedStringPtr> inputNameAllocs_;
    std::vector<const char*> inputNames_;
    std::vector<Ort::AllocatedStringPtr> outputNameAllocs_;
    std::vector<const char*> outputNames_;

    size_t numInputNodes_{0};
    size_t numOutputNodes_{0};

    int inputChannels_{3};
    cv::Size inputShape_;
    bool isDynamicInputShape_{false};
    bool isDynamicBatchSize_{false};
    std::string device_{"cpu"};

    /// Ultralytics-exported `names` dict parsed from ONNX metadata (empty if missing).
    std::vector<std::string> exportedClassNamesFromMetadata_;

    /// @brief Run inference with the given input tensor
    /// @param inputTensor Input tensor
    /// @return Vector of output tensors
    std::vector<Ort::Value> outputBuffers_;
    bool reusableOutputs_ = false;
    const std::vector<Ort::Value>& runInference(Ort::Value& inputTensor) {
        if (reusableOutputs_) {
            session_.Run(Ort::RunOptions{nullptr}, inputNames_.data(), &inputTensor,
                numInputNodes_, outputNames_.data(), outputBuffers_.data(), numOutputNodes_);
            return outputBuffers_;
        }
        outputBuffers_ = session_.Run(
            Ort::RunOptions{nullptr},
            inputNames_.data(),
            &inputTensor,
            numInputNodes_,
            outputNames_.data(),
            numOutputNodes_
        );
        return outputBuffers_;
    }

    /// @brief Create an input tensor from a blob
    /// @param blob Pointer to the input data
    /// @param inputTensorShape Shape of the input tensor
    /// @return ONNX Runtime input tensor
    Ort::Value createInputTensor(float* blob, const std::vector<int64_t>& inputTensorShape) {
        static Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        size_t inputTensorSize = utils::vectorProduct(inputTensorShape);
        
        return Ort::Value::CreateTensor<float>(
            memoryInfo,
            blob,
            inputTensorSize,
            inputTensorShape.data(),
            inputTensorShape.size()
        );
    }

private:
    void initSession(const std::string& modelPath, bool useGPU, int numThreads) {
        sessionOptions_ = Ort::SessionOptions();

        sessionOptions_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Configure execution provider
        bool directML = false;
        if (useGPU) {
            const auto availableProviders = Ort::GetAvailableProviders();

            // 1) 优先尝试 CUDA
            auto cudaIt = std::find(availableProviders.begin(), availableProviders.end(), "CUDAExecutionProvider");
            if (cudaIt != availableProviders.end()) {
                OrtCUDAProviderOptions cudaOptions{};
                sessionOptions_.AppendExecutionProvider_CUDA(cudaOptions);
                device_ = "gpu";
                std::cout << "[INFO] Inference device: GPU (CUDA)" << std::endl;
            }
            // 2) 其次尝试 DirectML
            else {
                auto dmlIt = std::find(availableProviders.begin(), availableProviders.end(), "DmlExecutionProvider");
                if (dmlIt != availableProviders.end()) {
                    // DirectML 使用 C API 注册（兼容性最好）
                    sessionOptions_.DisableMemPattern();
                    sessionOptions_.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
                    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions_, 0));
                    directML = true;
                    device_ = "gpu";
                    std::cout << "[INFO] Inference device: GPU (DirectML)" << std::endl;
                }
                // 3) 都没有 → 回退 CPU
                else {
                    std::cout << "[WARNING] GPU requested but neither CUDA nor DirectML is available. "
                        << "Falling back to CPU." << std::endl;
                    device_ = "cpu";
                }
            }
        }
        else {
            device_ = "cpu";
            std::cout << "[INFO] Inference device: CPU" << std::endl;
        }

        // DirectML's sequential GPU path needs no idle CPU worker pool for
        // the supplied model. Keep CPU/CUDA defaults and explicit overrides.
        const int cpuThreads = (std::max)(1, (std::min)(6, static_cast<int>(std::thread::hardware_concurrency())));
        const int threads = numThreads > 0 ? numThreads : (directML ? 1 : cpuThreads);
        sessionOptions_.SetIntraOpNumThreads(threads);
        sessionOptions_.AddConfigEntry("session.intra_op.allow_spinning", directML ? "0" : "1");
        sessionOptions_.AddConfigEntry("session.inter_op.allow_spinning", directML ? "0" : "1");

        // Load model
#ifdef _WIN32
        const int pathLength = MultiByteToWideChar(CP_ACP, 0, modelPath.c_str(), -1, nullptr, 0);
        if (pathLength <= 0) throw std::runtime_error("Invalid model path");
        std::wstring wModelPath(pathLength, L'\0');
        MultiByteToWideChar(CP_ACP, 0, modelPath.c_str(), -1, wModelPath.data(), pathLength);
        session_ = Ort::Session(env_, wModelPath.c_str(), sessionOptions_);
#else
        session_ = Ort::Session(env_, modelPath.c_str(), sessionOptions_);
#endif

        // Get node counts
        numInputNodes_ = session_.GetInputCount();
        numOutputNodes_ = session_.GetOutputCount();

        Ort::AllocatorWithDefaultOptions allocator;

        // Get input node names
        for (size_t i = 0; i < numInputNodes_; ++i) {
            auto inputName = session_.GetInputNameAllocated(i, allocator);
            inputNameAllocs_.push_back(std::move(inputName));
            inputNames_.push_back(inputNameAllocs_.back().get());
        }

        // Get output node names
        for (size_t i = 0; i < numOutputNodes_; ++i) {
            auto outputName = session_.GetOutputNameAllocated(i, allocator);
            outputNameAllocs_.push_back(std::move(outputName));
            outputNames_.push_back(outputNameAllocs_.back().get());
        }

        // Fixed output tensors can be reused; dynamic outputs remain runtime-owned.
        reusableOutputs_ = true;
        for (size_t i = 0; i < numOutputNodes_; ++i) {
            auto type = session_.GetOutputTypeInfo(i);
            if (type.GetONNXType() != ONNX_TYPE_TENSOR) { reusableOutputs_ = false; break; }
            auto info = type.GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                std::any_of(shape.begin(), shape.end(), [](int64_t d) { return d <= 0; })) {
                reusableOutputs_ = false; break;
            }
            outputBuffers_.push_back(Ort::Value::CreateTensor<float>(allocator, shape.data(), shape.size()));
        }
        if (!reusableOutputs_) outputBuffers_.clear();

        // Get input shape
        Ort::TypeInfo inputTypeInfo = session_.GetInputTypeInfo(0);
        std::vector<int64_t> inputTensorShape = inputTypeInfo.GetTensorTypeAndShapeInfo().GetShape();

        if (inputTensorShape.size() >= 4) {
            isDynamicBatchSize_ = (inputTensorShape[0] == -1);
            isDynamicInputShape_ = (inputTensorShape[2] == -1 || inputTensorShape[3] == -1);

            inputChannels_ = (inputTensorShape[1] == -1) ? 3 : static_cast<int>(inputTensorShape[1]);
            int height = (inputTensorShape[2] == -1) ? 640 : static_cast<int>(inputTensorShape[2]);
            int width = (inputTensorShape[3] == -1) ? 640 : static_cast<int>(inputTensorShape[3]);
            inputShape_ = cv::Size(width, height);
        } else {
            throw std::runtime_error("Invalid input tensor shape. Expected 4D tensor [N, C, H, W].");
        }

        std::cout << "[INFO] Model loaded: " << modelPath << std::endl;
        std::cout << "[INFO] Input shape: " << inputShape_.width << "x" << inputShape_.height
                  << (isDynamicInputShape_ ? " (dynamic)" : "") << std::endl;
        std::cout << "[INFO] Inputs: " << numInputNodes_ << ", Outputs: " << numOutputNodes_ << std::endl;

        try {
            exportedClassNamesFromMetadata_ = onnxmeta::tryGetExportedClassNames(session_);
        } catch (...) {
            exportedClassNamesFromMetadata_.clear();
        }
    }
};

} // namespace yolos
