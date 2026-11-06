// Production inference loop; no frame/device is needed to report missing files.
#include "../src/Aim.cpp"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#pragma comment(lib,"user32.lib")
AppState g;
CRITICAL_SECTION g_moveLock,g_drawLock,g_frameLock;
CONDITION_VARIABLE g_publishedFrameReady=CONDITION_VARIABLE_INIT;
std::vector<unsigned char> g_frame;
long long g_frameSeq=0;
std::atomic<long long> g_lastFrameTime{1000};
int dialogs=0,creates=0,detects=0,acquires=0,warmups=0;
bool createFails=false,runtimeFails=false,warmFails=false;
bool throwOnExists=false;
int idleTicks=0;
std::wstring errorText;
std::function<bool()> answer;
bool uiModelError(const std::wstring& text) {
    ++dialogs;errorText=text;
    assert(g.modelStatus==model_status::State::Failed && !g.canDraw && !g.triggerFire);
    assert(g.moveDx==0 && g.moveDy==0 && g.inferFps==0 && g.inferMsPrecise==0);
    return answer?answer():false;
}
void sleepMs(int) { if(++idleTicks==3) g.running=false; }
void optThread() {}
int truncToInt(double v) { return int(std::floor(v)); }
int roundToInt(double v) { return int(std::round(v)); }
int randInt(int,int) { return 0; }
bool fileExists(const std::filesystem::path& p) {
    if(throwOnExists) throw std::runtime_error("fixture filesystem failure");
    return std::filesystem::is_regular_file(p);
}
std::string utf8ToAnsi(const std::string& value) { return value; }
void makcuDown(int) { std::abort(); } void makcuUp(int) { std::abort(); }
void makcuMove(float,float) { std::abort(); }
void trajMove3(float,float) { std::abort(); } void trajMove5(float,float) { std::abort(); } void trajMove10(float,float) { std::abort(); }
long long nowMs() { return 1000; }
void RunTimer::start() {} double RunTimer::ms() const { return 1; }
double round2(double v) { return v; }
namespace runtime_log { void write(const char*,...) {} }
bool captureSetCrop(int) { return true; } int captureSetRegion(int w,int) { return w; }
bool captureAcquire(CaptureFrame& frame,unsigned long long&,unsigned) {
    ++acquires;
    if(!runtimeFails) {g.running=false;return false;}
    static std::vector<unsigned char> pixels(54+640*640*3);
    frame.bmp=pixels.data();frame.width=frame.height=640;frame.stride=640*3;
    return true;
}
int detect(const unsigned char* pixels) {
    if(pixels[0]==114) {++warmups;return warmFails?-1:0;}
    ++detects;return runtimeFails?-1:0;
}
int ncnnDetectBgr(const unsigned char* p,int,int,size_t,float,float,DetectObject*) {return detect(p);}
int onnxDetectBgr(const unsigned char* p,int,int,size_t,float,float,DetectObject*) {return detect(p);}
int trtDetectBgr(const unsigned char* p,int,int,size_t,float,float,DetectObject*) {return detect(p);}
int create() {++creates;return createFails?-1:0;}
int sagaNcnnCreate(const wchar_t*,const wchar_t*,int,int) {return create();}
int sagaOnnxCreate(const wchar_t*,const wchar_t*,bool) {return create();}
int sagaNcnnDetect(const unsigned char*,int,float,float,DetectObject*) {std::abort();}
int sagaOnnxDetect(const unsigned char*,int,float,float,DetectObject*) {std::abort();}
TrtTiming trtLastTiming() {std::abort();}
namespace saga {
int ncnn_select_cached(const wchar_t*,const wchar_t*,int,int,int) {return create();}
int onnx_select_cached(const wchar_t*,const wchar_t*,bool,int) {return create();}
int trt_select_cached(const wchar_t*,int) {return create();}
int trt_create(const wchar_t*) {return create();}
void ncnn_destroy() {} void onnx_destroy() {} void trt_destroy() {}
const char* ncnn_last_error() {return "fixture model failure";}
const char* onnx_last_error() {return "fixture model failure";}
const char* trt_last_error() {return "fixture model failure";}
int trt_detect(const unsigned char*,int,float,float,DetectObject*) {std::abort();}
}
void run(int engine,const std::string& model,bool capture=false) {
    dialogs=creates=detects=acquires=warmups=idleTicks=0;errorText.clear();
    g.engine=engine;g.modelName=model;g.captureRunning=capture;g.manualCropSize=640;
    g.modelStatus=model_status::State::Ready;g.canDraw=true;g.triggerFire=true;g.moveDx=42;g.moveDy=42;
    g.inferFps=123;g.inferMsPrecise=1;g.running=true;g_frameSeq=0;g_frame.clear();
    aimThread(engine);
}
int main() {
    InitializeCriticalSection(&g_moveLock);InitializeCriticalSection(&g_drawLock);InitializeCriticalSection(&g_frameLock);
    const auto dir=std::filesystem::temp_directory_path()/(L"saga-model-failures-"+std::to_wstring(GetCurrentProcessId()));
    assert(std::filesystem::create_directory(dir));g.runDir=dir;
    std::filesystem::create_directories(g.modelDirectory()/L"empty-group");
    for(int engine:{1,2,3}) for(const auto& model:{"missing","empty-group",""}) {
        run(engine,model);
        assert(dialogs==1 && creates==0 && detects==0 && acquires==0 && idleTicks==3);
        assert(errorText.find(model_status::displayPath(g.modelDirectory()))!=std::wstring::npos);
        assert(g.modelStatus==model_status::State::Failed);
    }
    throwOnExists=true;run(2,"filesystem-error");throwOnExists=false;
    assert(dialogs==1 && creates==0 && errorText.find(L"fixture filesystem failure")!=std::wstring::npos);
    // Retry after copying the required files must load again, even with the
    // same model selection. A successful capture is not a model success.
    for(int engine:{1,2,3}) {
        const auto model="retry-"+std::to_string(engine);
        answer=[&] {
            const auto base=g.modelDirectory()/file_paths::wide(model);
            for(const auto* ext:{L".onnx",L".param",L".bin",L".trt"}) std::ofstream(base.wstring()+ext).put('x');
            return true;
        };
        run(engine,model,true);
        assert(dialogs==1 && creates==1 && acquires==1 && detects==0 && g.modelStatus==model_status::State::Ready);
        answer={};createFails=true;run(engine,model,true);createFails=false;
        assert(dialogs==1 && creates==1 && acquires==0 && detects==0 && g.modelStatus==model_status::State::Failed);
        runtimeFails=true;run(engine,model,true);runtimeFails=false;
        assert(dialogs==1 && detects==1 && g.modelStatus==model_status::State::Failed);
    }
    const auto group=g.modelDirectory()/L"failed-group";std::filesystem::create_directory(group);
    for(const auto* ext:{L".onnx",L".param",L".bin",L".trt"}) std::ofstream(group/(std::wstring(L"640")+ext)).put('x');
    for(int engine:{1,2,3}) {
        warmFails=true;run(engine,"failed-group");warmFails=false;
        assert(dialogs==1 && creates==1 && warmups==1 && acquires==0 && g.modelStatus==model_status::State::Failed);
    }
    std::filesystem::remove_all(dir);
    std::cout<<"PASS missing/empty models before first frame, one notice on cancel, exact paths, repair+retry, load/warm/runtime failures in all engines\n";
}
