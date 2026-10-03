#include "TrtBuild.h"
#include "TrtRuntime.h"
#include "OnnxFp16.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace trt {
namespace {
std::string utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
class Logger final : public nvinfer1::ILogger {
public:
    explicit Logger(const BuildLog& callback) : callback_(callback) {}
    void log(Severity severity, const char* message) noexcept override {
        if (severity > Severity::kINFO || !message) return;
        try {
            std::lock_guard<std::mutex> guard(lock_);
            if (severity <= Severity::kERROR) lastError_ = message;
            if (callback_) callback_(message);
        } catch (...) {} // Exceptions must not cross the TensorRT logger boundary.
    }
    std::string lastError() const {
        std::lock_guard<std::mutex> guard(lock_);
        return lastError_;
    }
private:
    BuildLog callback_;
    mutable std::mutex lock_;
    std::string lastError_;
};
void emit(const BuildLog& log, const std::string& message) noexcept {
    // A failed UI/log callback must not turn a successful atomic save into a
    // reported conversion failure or throw again while handling an error.
    try { if (log) log(message); } catch (...) {}
}
std::runtime_error fail(const std::string& stage, const Logger& logger) {
    const auto detail = logger.lastError();
    return std::runtime_error(stage + (detail.empty() ? "" : ": " + detail));
}
std::string shapeText(const nvinfer1::Dims& shape) {
    std::ostringstream text;
    for (int i = 0; i < shape.nbDims; ++i) { if (i) text << 'x'; text << shape.d[i]; }
    return text.str();
}
void boundedElements(const nvinfer1::Dims& shape) {
    if (shape.nbDims < 1 || shape.nbDims > 8) throw std::runtime_error("Invalid TensorRT tensor dimensions.");
    size_t count = 1;
    for (int i = 0; i < shape.nbDims; ++i) {
        if (shape.d[i] <= 0 || size_t(shape.d[i]) > 536870912 / count)
            throw std::runtime_error("Unresolved or oversized TensorRT tensor; use a smaller fixed image size.");
        count *= size_t(shape.d[i]);
    }
}
std::vector<char> readModel(const std::filesystem::path& input) {
    std::ifstream file(input, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open the ONNX model: " + utf8(input.wstring()));
    const auto size = file.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > size_t(2) * 1024 * 1024 * 1024)
        throw std::runtime_error("The ONNX model is empty or exceeds the supported 2 GiB file size.");
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Failed to read the complete ONNX model.");
    return bytes;
}
// Read only ModelProto.metadata_props (field 14) and StringStringEntryProto
// strings. TensorRT remains responsible for parsing the graph. The bounded
// scanner avoids another runtime dependency just to reject non-detection tasks.
uint64_t varint(std::string_view data, size_t& offset) {
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (offset == data.size()) throw std::runtime_error("Truncated ONNX protobuf data.");
        const auto byte = static_cast<unsigned char>(data[offset++]);
        if (shift == 63 && (byte & 0xfe)) throw std::runtime_error("Invalid ONNX protobuf integer.");
        value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return value;
    }
    throw std::runtime_error("Invalid ONNX protobuf integer.");
}
uint32_t field(std::string_view data, size_t& offset, std::string_view& payload) {
    const uint64_t tag = varint(data, offset);
    if (!(tag >> 3) || (tag >> 3) > 0x1fffffff) throw std::runtime_error("Invalid ONNX protobuf field.");
    payload = {};
    uint64_t length = 0;
    switch (tag & 7) {
    case 0: varint(data, offset); break;
    case 1: length = 8; break;
    case 2: length = varint(data, offset); break;
    case 5: length = 4; break;
    default: throw std::runtime_error("Unsupported ONNX protobuf wire type.");
    }
    if (length > data.size() - offset) throw std::runtime_error("Truncated ONNX protobuf field.");
    if ((tag & 7) == 2) payload = data.substr(offset, static_cast<size_t>(length));
    offset += static_cast<size_t>(length);
    return static_cast<uint32_t>(tag >> 3);
}
std::map<std::string, std::string> metadata(const std::vector<char>& bytes) {
    const std::string_view data(bytes.data(), bytes.size());
    std::map<std::string, std::string> result;
    for (size_t offset = 0; offset < data.size();) {
        std::string_view entry;
        if (field(data, offset, entry) != 14 || entry.empty()) continue;
        std::string key, value;
        for (size_t item = 0; item < entry.size();) {
            std::string_view text;
            const uint32_t number = field(entry, item, text);
            if (number == 1) key = text;
            else if (number == 2) value = text;
        }
        if (!key.empty()) result[key] = value;
    }
    return result;
}
size_t classCount(const std::map<std::string, std::string>& values) {
    const auto found = values.find("names");
    if (found == values.end()) return 0;
    std::set<std::string> indices;
    const std::regex keys(R"((?:^|[,{])\s*(\d+)\s*:)");
    for (std::sregex_iterator item(found->second.begin(), found->second.end(), keys), end; item != end; ++item)
        indices.insert((*item)[1].str());
    return indices.size();
}
class AtomicOutput {
public:
    explicit AtomicOutput(const std::filesystem::path& output) : output_(output) {
        temporary_ = output;
        temporary_ += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L".tmp";
        handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create a temporary output file (Windows error " + std::to_string(GetLastError()) + ").");
    }
    ~AtomicOutput() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (!committed_) DeleteFileW(temporary_.c_str());
    }
    void write(const void* data, size_t size) {
        auto* bytes = static_cast<const char*>(data);
        while (size) {
            const DWORD block = static_cast<DWORD>((std::min)(size, size_t(1024 * 1024)));
            DWORD written = 0;
            if (!WriteFile(handle_, bytes, block, &written, nullptr) || written != block)
                throw std::runtime_error("Failed to write the TensorRT engine (Windows error " + std::to_string(GetLastError()) + ").");
            bytes += block; size -= block;
        }
    }
    void commit(bool overwrite) {
        if (!FlushFileBuffers(handle_)) throw std::runtime_error("Could not flush the TensorRT output file.");
        CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary_.c_str(), output_.c_str(), MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
            throw std::runtime_error("Could not save the TensorRT model; the destination may exist or be in use (Windows error " + std::to_string(GetLastError()) + ").");
        committed_ = true;
    }
private:
    std::filesystem::path output_, temporary_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool committed_ = false;
};
void validateEngine(nvinfer1::ICudaEngine& engine, const nvinfer1::Dims& inputShape, size_t classes) {
    std::unique_ptr<nvinfer1::IExecutionContext> context(engine.createExecutionContext());
    if (!context) throw std::runtime_error("Could not create a validation execution context.");
    for (int i = 0; i < engine.getNbIOTensors(); ++i) {
        const char* name = engine.getIOTensorName(i);
        if (engine.getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT && !context->setInputShape(name, inputShape))
            throw std::runtime_error("Could not apply the engine image-size profile.");
    }
    int inputCount = 0, outputCount = 0;
    for (int i = 0; i < engine.getNbIOTensors(); ++i) {
        const char* name = engine.getIOTensorName(i);
        const auto shape = context->getTensorShape(name);
        const auto type = engine.getTensorDataType(name);
        boundedElements(shape);
        if (engine.getTensorFormat(name) != nvinfer1::TensorFormat::kLINEAR ||
            engine.getTensorLocation(name) != nvinfer1::TensorLocation::kDEVICE)
            throw std::runtime_error("TensorRT input/output must use linear device tensors.");
        if (type != nvinfer1::DataType::kHALF)
            throw std::runtime_error("The converted engine must have FP16 input and output tensors.");
        if (engine.getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
            ++inputCount;
            if (shape.nbDims != 4 || shape.d[0] != 1 || shape.d[1] != 3 || shape.d[2] > 4096 || shape.d[3] > 4096)
                throw std::runtime_error("Expected one RGB input [1,3,height,width].");
        } else {
            ++outputCount;
            if (shape.nbDims != 3 || shape.d[0] != 1 || shape.d[1] < 5 || shape.d[1] > 4100 || shape.d[2] <= shape.d[1])
                throw std::runtime_error("Unsupported detection output. This backend requires YOLOv8-style [1,4+classes,boxes].");
            if (classes && shape.d[1] != static_cast<int64_t>(classes + 4))
                throw std::runtime_error("Detection output does not match the class metadata; pose, segmentation and objectness outputs are unsupported.");
        }
    }
    if (inputCount != 1 || outputCount != 1)
        throw std::runtime_error("Only YOLO models with one image input and one detection output are supported.");
}
}

bool convertOnnx(const ConversionOptions& options, const BuildLog& log, std::string& error) {
    error.clear();
    try {
        if (options.input.empty() || options.output.empty()) throw std::runtime_error("Select an input model and an output file.");
        const auto input = std::filesystem::weakly_canonical(std::filesystem::absolute(options.input));
        const auto output = std::filesystem::weakly_canonical(std::filesystem::absolute(options.output));
        if (_wcsicmp(input.c_str(), output.c_str()) == 0 ||
            (std::filesystem::exists(output) && std::filesystem::equivalent(input, output)))
            throw std::runtime_error("The output must differ from the input ONNX model; the source model will not be overwritten.");
        if (_wcsicmp(output.extension().c_str(), L".trt") != 0)
            throw std::runtime_error("The output filename must end in .trt so the application can discover the model.");
        if (!std::filesystem::is_regular_file(input)) throw std::runtime_error("The selected ONNX model does not exist.");
        if (!std::filesystem::is_directory(output.parent_path())) throw std::runtime_error("The output folder does not exist.");
        if (std::filesystem::exists(output) && !options.overwrite) throw std::runtime_error("The output already exists. Choose another file or explicitly allow overwrite.");
        if (std::filesystem::exists(output) && !std::filesystem::is_regular_file(output))
            throw std::runtime_error("The output path must be a file, not a folder.");
        if (options.dynamicWidth < 0 || options.dynamicHeight < 0 || options.dynamicWidth > 4096 || options.dynamicHeight > 4096 ||
            ((options.dynamicWidth == 0) != (options.dynamicHeight == 0)))
            throw std::runtime_error("Dynamic input size requires both a positive width and height, each at most 4096.");
        auto bytes = readModel(input);
        const auto values = metadata(bytes);
        const auto task = values.find("task");
        if (task != values.end() && !task->second.empty() && task->second != "detect")
            throw std::runtime_error("Unsupported ONNX task: " + task->second + ". Only YOLO detection models are supported.");
        const size_t classes = classCount(values);
        emit(log, "Converting floating ONNX tensors and model I/O to FP16 in memory...");
        bytes = onnx_fp16::convert(bytes);
        emit(log, "Loading TensorRT and checking the NVIDIA device...");
        auto& native = library(); native.ensure(true);
        int devices = 0;
        checkCuda(native.cudaGetDeviceCount(&devices), "CUDA device discovery");
        if (!devices) throw std::runtime_error("No supported NVIDIA CUDA device was found.");
        checkCuda(native.cudaSetDevice(0), "Select NVIDIA GPU 0");
        Logger logger(log);
        std::unique_ptr<nvinfer1::IBuilder> builder(native.createBuilder(logger));
        if (!builder) throw fail("TensorRT could not create a builder", logger);
        std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
        if (!network) throw fail("TensorRT could not create a network", logger);
        std::unique_ptr<nvonnxparser::IParser> parser(native.createParser(*network, logger));
        if (!parser) throw fail("TensorRT could not create an ONNX parser", logger);
        emit(log, "Parsing ONNX model: " + utf8(input.wstring()));
        const auto modelPath = utf8(input.wstring());
        if (!parser->parse(bytes.data(), bytes.size(), modelPath.c_str())) {
            std::ostringstream message;
            message << "TensorRT could not parse the ONNX model.";
            for (int i = 0; i < parser->getNbErrors(); ++i) {
                if (const auto* issue = parser->getError(i)) message << "\n" << issue->desc();
            }
            throw std::runtime_error(message.str());
        }
        if (network->getNbInputs() != 1 || network->getNbOutputs() != 1)
            throw std::runtime_error("Expected one YOLO image input and one detection output.");
        auto* tensor = network->getInput(0);
        auto shape = tensor->getDimensions();
        if (shape.nbDims != 4 || (shape.d[0] != 1 && shape.d[0] != -1) || shape.d[1] != 3)
            throw std::runtime_error("Expected RGB NCHW input [1,3,height,width].");
        std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
        if (!config) throw fail("TensorRT could not create a build configuration", logger);
        config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, size_t(1) << 30);
        // HALF is encoded in the transformed graph, as required by TensorRT 11.
        config->clearFlag(nvinfer1::BuilderFlag::kTF32);
        const bool dynamic = std::any_of(shape.d, shape.d + shape.nbDims, [](int64_t dimension) { return dimension < 0; });
        nvinfer1::IOptimizationProfile* profile = nullptr; // Owned by IBuilder.
        if (dynamic) {
            if ((shape.d[2] < 0 || shape.d[3] < 0) && !options.dynamicWidth)
                throw std::runtime_error("The ONNX image size is dynamic. Enter the intended width x height using the GUI or --size.");
            if (shape.d[0] < 0) shape.d[0] = 1;
            if (shape.d[2] < 0) shape.d[2] = options.dynamicHeight;
            if (shape.d[3] < 0) shape.d[3] = options.dynamicWidth;
            profile = builder->createOptimizationProfile();
            if (!profile || !profile->setDimensions(tensor->getName(), nvinfer1::OptProfileSelector::kMIN, shape) ||
                !profile->setDimensions(tensor->getName(), nvinfer1::OptProfileSelector::kOPT, shape) ||
                !profile->setDimensions(tensor->getName(), nvinfer1::OptProfileSelector::kMAX, shape) || !profile->isValid())
                throw std::runtime_error("The dynamic image profile is invalid.");
            if (config->addOptimizationProfile(profile) < 0) throw std::runtime_error("Could not register the image-size profile.");
        }
        if (shape.d[2] <= 0 || shape.d[3] <= 0 || shape.d[2] > 4096 || shape.d[3] > 4096)
            throw std::runtime_error("The model image size must be between 1 and 4096 in each dimension.");
        if (options.dynamicWidth && (shape.d[3] != options.dynamicWidth || shape.d[2] != options.dynamicHeight))
            throw std::runtime_error("The requested --size conflicts with the ONNX model's static dimensions.");
        emit(log, "Input: " + std::string(tensor->getName()) + " [" + shapeText(shape) + "]. FP16 engine.");
        emit(log, "Building and optimizing the TensorRT engine. The window remains usable; this may take several minutes.");
        std::unique_ptr<nvinfer1::IHostMemory> serialized(builder->buildSerializedNetwork(*network, *config));
        if (!serialized || !serialized->size()) throw fail("TensorRT engine build failed", logger);
        std::unique_ptr<nvinfer1::IRuntime> runtime(native.createRuntime(logger));
        if (!runtime) throw fail("TensorRT could not validate the engine", logger);
        std::unique_ptr<nvinfer1::ICudaEngine> engine(runtime->deserializeCudaEngine(serialized->data(), serialized->size()));
        if (!engine) throw fail("TensorRT could not load the built engine", logger);
        validateEngine(*engine, shape, classes);
        emit(log, "Saving validated engine (" + std::to_string(serialized->size()) + " bytes)...");
        AtomicOutput writer(output);
        writer.write(serialized->data(), serialized->size()); writer.commit(options.overwrite);
        emit(log, "Saved: " + utf8(output.wstring()));
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        emit(log, "Error: " + error);
        return false;
    }
}
}
