#include "Inference.h"
#include "../src/TensorRT.cpp"
#include "../src/TrtRuntime.cpp"
#include <windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        saga::trt_destroy();
        DetectObject result[100]{};
        result[99].prob = 123.0f;
        const unsigned char pixel[3]{0,0,0};
        require(trtDetectBgr(pixel,1,1,3,.25f,.45f,result)<0,"Uninitialized engine must fail");
        require(std::strlen(saga::trt_last_error())>0,"Failure must have a diagnostic");
        require(trtDetectBgr(pixel,1,1,2,.25f,.45f,result)<0,"Invalid stride must fail");
        require(trtDetectBgr(pixel,-1,1,3,.25f,.45f,result)<0,"Negative width must fail");
        require(saga::trt_detect(nullptr,1,.25f,.45f,result)<0,"Null encoded image must fail");
        require(saga::trt_detect(pixel,0,.25f,.45f,result)<0,"Empty encoded image must fail");
        std::array<unsigned char,58> bmp{}; bmp[0]='B'; bmp[1]='M';
        auto write = [&](int offset,int value) { std::memcpy(bmp.data()+offset,&value,4); };
        write(10,54);write(14,40);write(18,1);write(22,-1);bmp[26]=1;bmp[28]=24;
        require(saga::trt_detect(bmp.data(),int(bmp.size()),.25f,.45f,result)<0,"Valid BMP without an engine must fail safely");
        write(10,0x7fffffff);write(18,0x7fffffff);write(22,int(0x80000000u));
        require(saga::trt_detect(bmp.data(),int(bmp.size()),.25f,.45f,result)<0,"Malformed BMP must fail safely");
        require(saga::trt_detect(pixel,3,.25f,.45f,nullptr)<0,"Null result buffer must fail");
        require(result[99].prob==123.0f,"Error paths must not write results");
        require(!GetModuleHandleW(L"nvinfer_11.dll") && !GetModuleHandleW(L"cudart64_13.dll"),"Invalid API calls must not load NVIDIA libraries");
        saga::trt_destroy(); saga::trt_destroy();
        std::cout << "TensorRT API CPU-only error-path checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
