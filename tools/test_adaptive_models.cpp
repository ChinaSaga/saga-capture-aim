// Real model cache/inference validation. No capture, application, or mouse I/O.
#include "Inference.h"
#include "FilePaths.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path fixture;
    try {
        require(argc == 2 || argc == 3, "Supply model group directory [--gpu]");
        const bool gpu = argc == 3 && std::wstring(argv[2]) == L"--gpu";
        fixture = std::filesystem::temp_directory_path() /
            (L"saga-adaptive-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(fixture), "Create unique fixture");
        for (int size : {320,416,640}) for (auto ext : {L".onnx",L".param",L".bin",L".trt"}) {
            const auto name = std::to_wstring(size) + ext;
            std::filesystem::copy_file(std::filesystem::path(argv[1])/name, fixture/name);
        }
        DetectObject out[99]{};
        for (int engine : {1,2,3}) {
            if (gpu && engine == 3) continue; // The first pass already tested TensorRT on the GPU.
            auto select = [&](int size) {
                const auto base = (fixture/std::to_wstring(size)).wstring();
                if (engine == 1) return saga::ncnn_select_cached((base+L".param").c_str(),(base+L".bin").c_str(),gpu?1:0,0,size);
                if (engine == 2) return saga::onnx_select_cached((base+L".onnx").c_str(),L"",gpu,size);
                return saga::trt_select_cached((base+L".trt").c_str(),size);
            };
            auto error = [&] { return std::string(engine==1?saga::ncnn_last_error():engine==2?saga::onnx_last_error():saga::trt_last_error()); };
            const auto detect = engine==1?ncnnDetectBgr:engine==2?onnxDetectBgr:trtDetectBgr;
            for (int size : {320,416,640}) {
                const int rc = select(size); require(rc==0, "load: "+error());
                std::vector<unsigned char> pixels(size_t(size)*size*3,114);
                require(detect(pixels.data(),size,size,size_t(size)*3,.55f,.25f,out)>=0, "warm: "+error());
                // Hide only these test-owned copies; cached selection must not re-open the files.
                const std::vector<std::wstring> extensions = engine==1 ? std::vector<std::wstring>{L".param",L".bin"} :
                    std::vector<std::wstring>{engine==2?L".onnx":L".trt"};
                for (const auto& ext : extensions) {
                    const auto file=fixture/(std::to_wstring(size)+ext);
                    std::filesystem::rename(file,file.wstring()+L".hidden");
                }
            }
            require(select(999)<0, "Invalid load must fail");
            for (int size : {640,416,320,416,640,320,640}) {
                require(select(size)==0, "cached select after failed load: "+error());
                std::vector<unsigned char> pixels(size_t(size)*size*3,114);
                require(detect(pixels.data(),size,size,size_t(size)*3,.55f,.25f,out)>=0, "cached inference: "+error());
            }
            saga::ncnn_destroy(); saga::onnx_destroy(); saga::trt_destroy();
            std::cout << "PASS engine " << engine << ": all sizes, cache reuse without files, failed load recovery, inference and teardown\n";
        }
        std::filesystem::remove_all(fixture);
        return 0;
    } catch (const std::exception& e) {
        saga::ncnn_destroy(); saga::onnx_destroy(); saga::trt_destroy();
        if (!fixture.empty()) { std::error_code ignored; std::filesystem::remove_all(fixture,ignored); }
        std::cerr << e.what() << '\n'; return 1;
    }
}
