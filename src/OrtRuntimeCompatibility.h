#pragma once
#include <onnxruntime_c_api.h>
#include <stdexcept>
#include <string>

inline void requireCompatibleOrtRuntime(const OrtApiBase* base) {
    // An older DLL returns nullptr for a newer API version. The C++ wrapper
    // assumes that pointer is valid and otherwise crashes before its try/catch.
    if (base && base->GetApi && base->GetApi(ORT_API_VERSION)) return;
    const char* version = base && base->GetVersionString ? base->GetVersionString() : nullptr;
    throw std::runtime_error("Incompatible onnxruntime.dll: loaded version " +
        std::string(version ? version : "unknown") + ", required API " +
        std::to_string(ORT_API_VERSION) +
        ". Replace onnxruntime.dll and DirectML.dll with the files bundled with this EXE.");
}
