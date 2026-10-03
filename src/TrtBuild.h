#pragma once
#include <filesystem>
#include <functional>
#include <string>

namespace trt {
struct ConversionOptions {
    std::filesystem::path input;
    std::filesystem::path output;
    bool overwrite = false;
    // Preserve ONNX tensor precision. TensorRT 11 uses strongly typed networks.
    bool fp16 = false;
    bool allowTf32 = false;
    int dynamicWidth = 0;
    int dynamicHeight = 0;
};
using BuildLog = std::function<void(const std::string&)>;
// Shared by the GUI and CLI. Writes only after a complete engine was built.
// A false result leaves the original output untouched.
bool convertOnnx(const ConversionOptions& options, const BuildLog& log, std::string& error);
}
