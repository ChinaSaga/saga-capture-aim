#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/session_base.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const bool oldRuntime = argc > 1 && std::strcmp(argv[1], "--old") == 0;
    try {
        requireCompatibleOrtRuntime(nullptr);
        return 2;
    } catch (const std::runtime_error&) {}
    try {
        // The real constructor must reject the old DLL BEFORE Ort::Env can
        // dereference its null API table. A compatible DLL reaches model load.
        yolos::OrtSessionBase session("__missing_compatibility_test_model__.onnx", false, 1);
        return 3;
    } catch (const std::exception& e) {
        const bool incompatible = std::strstr(e.what(), "Incompatible onnxruntime.dll") != nullptr;
        if (oldRuntime != incompatible) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
        std::printf("PASS: runtime=%s; %s\n", OrtGetApiBase()->GetVersionString(), e.what());
    }
    return 0;
}
