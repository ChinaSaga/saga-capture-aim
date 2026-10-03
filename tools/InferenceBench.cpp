#include "Inference.h"
#include <windows.h>
#include <mmsystem.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static std::string ansi(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_ACP,0,s,-1,nullptr,0,nullptr,nullptr);
    std::string result(n,'\0'); WideCharToMultiByte(CP_ACP,0,s,-1,result.data(),n,nullptr,nullptr);
    result.pop_back(); return result;
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 6) { std::cerr << "Usage: InferenceBench trt|onnx model seconds fps output.csv [capture|image_path]\n"; return 2; }
    const bool trt = std::wstring(argv[1]) == L"trt";
    const double seconds = _wtof(argv[3]), fps = _wtof(argv[4]);
    if (seconds <= 0 || fps < 0) return 2;
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    Microsoft::WRL::ComPtr<ID3D11Device> powerDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> powerContext;
    if (GetEnvironmentVariableW(L"SAGA_TRT_D3D_PROFILE",nullptr,0)) {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for(UINT index=0;;++index) {
                Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
                if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
                if(desc.VendorId!=0x10de) continue;
                const HRESULT hr=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&powerDevice,nullptr,&powerContext);
                std::cerr<<"D3D power profile device hr="<<std::hex<<hr<<std::dec<<'\n'; break;
            }
        }
    }
    const auto model = ansi(argv[2]);
    const int rc = trt ? saga::trt_create(model.c_str()) : saga::onnx_create(model.c_str(),"",true);
    if (rc != 0) { std::cerr << (trt ? saga::trt_last_error() : saga::onnx_last_error()) << '\n'; timeEndPeriod(1); return 3; }
    const bool capture = argc > 6 && std::wstring(argv[6]) == L"capture";
    cv::Mat image(416,416,CV_8UC3,cv::Scalar(114,114,114));
    if (argc > 6 && !capture) {
        std::ifstream input(std::filesystem::path(argv[6]),std::ios::binary);
        std::vector<uchar> bytes((std::istreambuf_iterator<char>(input)),{});
        image = cv::imdecode(bytes,cv::IMREAD_COLOR); if (image.empty()) return 4;
    }
    if (capture) {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        const char* result = saga::setcjk_ex("Live Gamer Ultra 2.1-Video",1920,1080,"NV12",240,416);
        std::cerr << (result ? result : "No capture result") << '\n';
    }
    std::ofstream csv{std::filesystem::path(argv[5])};
    if (!csv) return 5;
    csv << "second,frames,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,pre_ms,submit_ms,wait_ms,post_ms,gpu_ms,graph,blocking,detections\n";
    std::vector<double> times;
    double pre=0,submit=0,wait=0,post=0,gpu=0; int detections=0;
    TrtTiming timing;
    bool savedFrame = false;
    bool dumpedObjects = false;
    const bool saveFrame = GetEnvironmentVariableW(L"SAGA_BENCH_SAVE_FRAME",nullptr,0) != 0;
    unsigned long long seq=0;
    const auto start=Clock::now(); auto interval=start, next=start;
    while (std::chrono::duration<double>(Clock::now()-start).count()<seconds) {
        CaptureFrame frame;
        if (capture && !captureAcquire(frame,seq,1000)) continue;
        DetectObject objects[99]; const auto t0=Clock::now();
        int n = capture ? (trt ? trtDetectBgr : onnxDetectBgr)(frame.bmp+54,frame.width,frame.height,frame.stride,.42f,.25f,objects)
                        : (trt ? trtDetectBgr : onnxDetectBgr)(image.data,image.cols,image.rows,image.step,.42f,.25f,objects);
        const auto end=Clock::now();
        if (n<0) { std::cerr << (trt ? saga::trt_last_error() : saga::onnx_last_error()) << '\n'; return 6; }
        if (!capture && !dumpedObjects && times.size() >= 10) {
            std::ofstream objectsFile{std::filesystem::path(std::wstring(argv[5])+L".objects.csv")};
            objectsFile << "label,confidence,x,y,width,height\n" << std::setprecision(9);
            for(int i=0;i<n;++i) objectsFile << objects[i].label << ',' << objects[i].prob << ',' << objects[i].x << ',' << objects[i].y << ',' << objects[i].width << ',' << objects[i].height << '\n';
            dumpedObjects = true;
        }
        if (capture && saveFrame && !savedFrame && n>0) {
            cv::Mat sample(frame.height,frame.width,CV_8UC3,const_cast<unsigned char*>(frame.bmp+54),frame.stride);
            std::vector<uchar> encoded; cv::imencode(".png",sample,encoded);
            std::ofstream sampleFile{std::filesystem::path(std::wstring(argv[5])+L".png"),std::ios::binary};
            sampleFile.write(reinterpret_cast<const char*>(encoded.data()),encoded.size()); savedFrame = true;
        }
        times.push_back(std::chrono::duration<double,std::milli>(end-t0).count()); detections+=n;
        if(trt) { timing=trtLastTiming(); pre+=timing.preprocessMs; submit+=timing.submitMs; wait+=timing.waitMs; post+=timing.postprocessMs; gpu+=timing.gpuMs; }
        if (std::chrono::duration<double>(end-interval).count()>=1) {
            std::sort(times.begin(),times.end()); double sum=0; for(double t:times) sum+=t;
            const double count=double(times.size());
            auto percentile=[&](double p){return times[size_t((times.size()-1)*p)];};
            csv << std::chrono::duration<double>(end-start).count() << ',' << times.size() << ',' << sum/count << ',' << percentile(.5) << ',' << percentile(.95) << ',' << percentile(.99) << ',' << times.back() << ',' << pre/count << ',' << submit/count << ',' << wait/count << ',' << post/count << ',' << gpu/count << ',' << timing.graphActive << ',' << timing.blockingSync << ',' << detections << '\n'; csv.flush();
            times.clear(); pre=submit=wait=post=gpu=0; detections=0; interval=end;
        }
        if(!capture && fps>0) { next+=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1/fps)); std::this_thread::sleep_until(next); if(Clock::now()>next+std::chrono::milliseconds(100)) next=Clock::now(); }
    }
    if(trt) saga::trt_destroy(); else saga::onnx_destroy();
    timeEndPeriod(1); return 0;
}
