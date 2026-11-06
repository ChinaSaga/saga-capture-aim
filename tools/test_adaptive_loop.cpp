// Exercise the production adaptive loop with deterministic frames; no mouse or hardware I/O.
#include "../src/Aim.cpp"
#include <cassert>
#include <iostream>
#pragma comment(lib, "user32.lib")
AppState g;
CRITICAL_SECTION g_moveLock, g_drawLock;
CRITICAL_SECTION g_frameLock;
CONDITION_VARIABLE g_publishedFrameReady = CONDITION_VARIABLE_INIT;
std::vector<unsigned char> g_frame;
long long g_frameSeq = 0;
std::atomic<long long> g_lastFrameTime{1000};
bool uiModelError(const std::wstring&) { std::abort(); }
void sleepMs(int) { throw std::runtime_error("Unexpected idle/failure path"); }
void optThread() {}
int truncToInt(double x) { return int(std::floor(x)); }
int roundToInt(double x) { return int(std::round(x)); }
int randInt(int, int) { return 0; }
bool fileExists(const std::filesystem::path& p) { return std::filesystem::is_regular_file(p); }
std::string utf8ToAnsi(const std::string& x) { return x; }
int presses = 0, releases = 0;
void makcuDown(int) { ++presses; }
void makcuUp(int) { ++releases; }
int frameIndex=0, crop=640, cropWidth=640, selected=0;
bool liveFrame=false;
std::vector<int> observed;
std::vector<int> warmed;
int failWarmSize=0;
bool fixedModelFixture=false;
bool alignedPointFixture=false;
std::vector<int> alignedMovesX,alignedMovesY;
bool wideFixture=false;
int tileCalls=0;
std::vector<int> observedWidths;
struct FrameSpec {
    int distance;
    bool key=true, target=true;
    float width=100, height=100;
    bool fail=false;
    int toggle=-1;
};
std::vector<FrameSpec> scenario;
bool captureSetCrop(int size) { crop=cropWidth=size; return true; }
int captureSetRegion(int width,int height) { crop=height;cropWidth=width;return width; }
void markPixels(unsigned char* pixels, int width, int height) {
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        auto* p=pixels+(size_t(y)*width+x)*3;
        p[0]=7;p[1]=x&255;p[2]=x>>8;
    }
}
bool captureAcquire(CaptureFrame& f, unsigned long long& seq, unsigned) {
    if(alignedPointFixture && frameIndex>0) {
        alignedMovesX.push_back(g.moveDx);alignedMovesY.push_back(g.moveDy);
        if(g.moveDx || g.moveDy) {
            ++g.moveDispatchSequence;g.moveDispatchedX+=g.moveDx;g.moveDispatchedY+=g.moveDy;
        } // Simulated consumption; never sends hardware input.
    }
    if(frameIndex == scenario.size()) { g.running=false; return false; }
    static std::vector<unsigned char> pixels(54+1920*640*3);
    f.bmp=pixels.data(); f.width=cropWidth; f.height=crop; f.stride=size_t(cropWidth)*3;
    f.timestampMs=1000+frameIndex*10.;
    if(wideFixture) markPixels(pixels.data()+54,cropWidth,crop);
    g.aimPhysicalDown = scenario[frameIndex].key;
    g.aimKeyDown=g.aimPhysicalDown.load();
    liveFrame=true; ++seq; return true;
}
int detectFrame(DetectObject* out) {
    if (!liveFrame) { warmed.push_back(selected); return selected==failWarmSize?-1:0; }
    liveFrame=false;
    assert(selected==crop && g.imgSize==crop);
    observed.push_back(crop);
    const auto spec=scenario[frameIndex++];
    if(spec.fail) return -1;
    if(!spec.target) return 0;
    if(alignedPointFixture) {
        const int index=frameIndex-1;
        const float center=crop*.5f-60+index*1.5f;
        const DetectObject head{center-15,crop*.5f-15,30,30,0,.95f};
        const float bodyHeight=120+20*float(std::sin(index*.5));
        const DetectObject body{center-40,crop*.5f-bodyHeight*.15f,80,bodyHeight,1,.95f};
        if(index>=20 && index<30) { out[0]=body;return 1; }
        if(index>=60 && index<70) { out[0]=head;return 1; }
        out[0]=head;out[1]=body;return 2;
    }
    // Match real detectors, which can clip a target larger than the input crop.
    const float left=(std::max)(0.f, crop/2+spec.distance-spec.width/2);
    const float top=(std::max)(0.f, crop/2-spec.height/2);
    const float right=(std::min)(float(crop), crop/2+spec.distance+spec.width/2);
    const float bottom=(std::min)(float(crop), crop/2+spec.height/2);
    out[0]={left,top,right-left,bottom-top,0,.95f};
    return 1;
}
int detectPixels(const unsigned char* pixels, int width, DetectObject* out) {
    if(wideFixture && pixels[0]==7) {
        assert(width==selected && width==g.imgSize);
        const auto spec=scenario[frameIndex];
        g.aimPhysicalDown=spec.key;g.aimKeyDown=spec.key;
        const int sourceWidth=g.captureRunning?cropWidth:1920;
        const int left=pixels[1]+256*pixels[2]-(sourceWidth-g.imgWidth)/2;
        const int frameWidth=g.imgWidth;
        const float x0=frameWidth/2+spec.distance-spec.width/2-left;
        const float x1=(std::min)(float(width),x0+spec.width);
        const float x=(std::max)(0.f,x0);
        const float top=(std::max)(0.f,width/2-spec.height/2);
        const float bottom=(std::min)(float(width),width/2+spec.height/2);
        const bool found=spec.target && x1>x && bottom>top;
        if(found) out[0]={x,top,x1-x,bottom-top,0,.95f};
        if(++tileCalls==(frameWidth==width?1:3) || spec.fail) {
            observed.push_back(width);observedWidths.push_back(frameWidth);
            ++frameIndex;tileCalls=0;liveFrame=false;
            if(spec.toggle>=0) { g.fullScreenSearch=bool(spec.toggle);++g.inferenceEpoch; }
            if(!g.captureRunning) { ++g_frameSeq;if(frameIndex==scenario.size()) g.running=false; }
        }
        return spec.fail?-1:found?1:0;
    }
    const bool remote=!g.captureRunning && pixels[0]==7; // Warm-up uses 114, fixture frames use 7.
    if(remote) {
        crop=width; liveFrame=true;
        g.aimPhysicalDown=scenario[frameIndex].key;
        g.aimKeyDown=g.aimPhysicalDown.load();
    }
    const int result=detectFrame(out);
    if(remote) {
        ++g_frameSeq;
        if(frameIndex==scenario.size()) g.running=false;
    }
    return result;
}
int ncnnDetectBgr(const unsigned char* pixels, int width, int, size_t, float, float, DetectObject* out) { return detectPixels(pixels,width,out); }
int onnxDetectBgr(const unsigned char* pixels, int width, int, size_t, float, float, DetectObject* out) { return detectPixels(pixels,width,out); }
int trtDetectBgr(const unsigned char* pixels, int width, int, size_t, float, float, DetectObject* out) { return detectPixels(pixels,width,out); }
int sagaNcnnCreate(const wchar_t*, const wchar_t*, int, int) { assert(fixedModelFixture);selected=g.manualCropSize;return 0; }
int sagaOnnxCreate(const wchar_t*, const wchar_t*, bool) { assert(fixedModelFixture);selected=g.manualCropSize;return 0; }
int sagaNcnnDetect(const unsigned char*, int, float, float, DetectObject*) { std::abort(); }
int sagaOnnxDetect(const unsigned char*, int, float, float, DetectObject*) { std::abort(); }
TrtTiming trtLastTiming() { std::abort(); }
namespace runtime_log { void write(const char*, ...) {} }
namespace saga {
int ncnn_select_cached(const wchar_t*, const wchar_t*, int, int, int size) {selected=size;return 0;}
int onnx_select_cached(const wchar_t*, const wchar_t*, bool, int size) {selected=size;return 0;}
int trt_select_cached(const wchar_t*, int size) {selected=size;return 0;}
void ncnn_destroy() {} void onnx_destroy() {} void trt_destroy() {}
const char* ncnn_last_error() { return "fixture failure"; } const char* onnx_last_error() { return "fixture failure"; } const char* trt_last_error() { return "fixture failure"; }
int trt_create(const wchar_t*) { assert(fixedModelFixture);selected=g.manualCropSize;return 0; }
int trt_detect(const unsigned char*, int, float, float, DetectObject*) { std::abort(); }
}
long long nowMs() { return 1000; }
void RunTimer::start() {}
double RunTimer::ms() const { return 0; }
double round2(double value) { return value; }
int sent = 0;
bool publishDuringMove = false;
void makcuMove(float x, float y) {
    ++sent;
    assert(x == 10 && y == 20);
    if (publishDuringMove) {
        EnterCriticalSection(&g_moveLock);
        g.moveDx = 30; g.moveDy = 40;
        LeaveCriticalSection(&g_moveLock);
        aimMoveOnce(); // A second consumer cannot send concurrently.
    }
}
void trajMove3(float x, float y) { makcuMove(x, y); }
void trajMove5(float x, float y) { makcuMove(x, y); }
void trajMove10(float x, float y) { makcuMove(x, y); }
static void publish() {
    g.moveDx = 10; g.moveDy = 20;
    g.moveEpoch = g.inferenceEpoch;
    g.moveFrameTime = nowMs();
    g.moveProfileToken = g.aimProfiles.token();
}

#include <fstream>
int main() {
    InitializeCriticalSection(&g_moveLock); InitializeCriticalSection(&g_drawLock); InitializeCriticalSection(&g_frameLock);
    const auto dir=std::filesystem::temp_directory_path()/(L"saga-adaptive-loop-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(dir));
    g.runDir=dir; g.modelName="group";
    auto settings=g.settings.snapshot(); settings.threadMove=true; settings.crossClass=0; g.settings.replace(settings);
    aim_profiles::Config config; config.profiles[0].lockRange=640; config.profiles[0].centerRange=0;
    g.aimProfiles.replace(config);
    g.classEnabled[0]=true;g.classColor[0]=1;g.showImg=true;g.captureRunning=true;g_frameSeq=1;
    for(bool dense : {false,true}) {
        g.modelName=dense?"dense":"sparse";
        const auto group=g.modelDirectory()/file_paths::wide(g.modelName);
        std::filesystem::create_directories(group);
        std::vector<int> expectedWarm;
        for(int size=160;size<=640;size+=32) {
            if(!dense && size!=256 && size!=320 && size!=416 && size!=640) continue;
            expectedWarm.push_back(size);
            for(auto ext:{L".onnx",L".param",L".bin",L".trt"}) std::ofstream(group/(std::to_wstring(size)+ext)).put('x');
        }
        std::vector<int> expected;
        if(!dense) {
            scenario={
                {0,false}, {0}, {0}, {0}, // Three confirmations to enter 256.
                {0,true,true,250,250}, {0,true,true,250,250}, // 256 -> 320.
                {0,true,true,320,320}, {0,true,true,320,320}, // Missing 384 -> 416.
                {0,true,true,410,410}, {0,true,true,410,410}, // Missing 512 -> 640.
                {0}, {0}, {0}, {0,true,false}, // Shrink then immediately recover after losing the target.
                {0}, {0}, {0}, {0,false}, {0,false}
            };
            expected={640,640,640,640,256,320,320,416,416,640,640,640,640,256,640,640,640,256,640};
        } else {
            scenario={{0,false}}; expected={640};
            int current=640;
            // Every cached stage is reached by target-size decisions, with three-frame shrink confirmation.
            for(int size=608;size>=160;size-=32) {
                const float extent=size==160?100.f:float(size/1.2-.1);
                for(int frame=0;frame<3;++frame) {
                    scenario.push_back({0,true,true,extent,extent}); expected.push_back(current);
                }
                current=size;
            }
            scenario.push_back({0}); expected.push_back(160);
            // A partly visible 500 px target must force repeated immediate growth.
            for(int size:{160,192,256,320,384,480,576,608,608}) {
                scenario.push_back({0,true,true,500,500}); expected.push_back(size);
            }
            scenario.push_back({0,true,false}); expected.push_back(608);
            scenario.push_back({0}); expected.push_back(640);
            scenario.push_back({0}); expected.push_back(640);
            scenario.push_back({0}); expected.push_back(640);
            scenario.push_back({0,false}); expected.push_back(160);
            scenario.push_back({0,false}); expected.push_back(640);
        }
        for(int engine:{1,2,3}) {
            frameIndex=0;observed.clear();warmed.clear();g.engine=engine;g.aimPhysicalDown=false;g.running=true;
            aimThread(engine);
            if(observed!=expected) { for(int size:observed) std::cerr<<size<<' '; std::abort(); }
            assert(warmed==expectedWarm); // No test inferences while selecting a cached stage.
            assert(sent==0);
        }
    }
    for (bool single : {false,true}) {
        g.modelName=single?"single":"max512";
        const auto group=g.modelDirectory()/file_paths::wide(g.modelName);
        std::filesystem::create_directories(group);
        const std::vector<int> expectedWarm=single?std::vector<int>{320}:std::vector<int>{320,416,512};
        for(int size:expectedWarm) for(auto ext:{L".onnx",L".param",L".bin",L".trt"})
            std::ofstream(group/(std::to_wstring(size)+ext)).put('x');
        scenario={{0,false},{0},{0},{0},{0,true,true,320,320},{0,true,true,410,410},
            {0,true,true,900,900},{0,true,false},{0},{0},{0},{0,false},{0,false}};
        auto expected=single?std::vector<int>(scenario.size(),320):
            std::vector<int>{512,512,512,512,320,416,512,512,512,512,512,320,512};
        auto run=[&](int engine, bool remote=false) {
            g.captureRunning=!remote;
            if(remote) {
                const int sourceSize=expectedWarm.back();
                g_frame.assign(54+sourceSize*sourceSize*3,7);
                std::fill_n(g_frame.data(),54,0);
                const auto field=[&](int offset,int value) { std::memcpy(g_frame.data()+offset,&value,4); };
                g_frame[0]='B';g_frame[1]='M';field(2,int(g_frame.size()));field(10,54);field(14,40);
                field(18,sourceSize);field(22,-sourceSize);g_frame[26]=1;g_frame[28]=24;
                ++g_frameSeq;
            }
            frameIndex=0;observed.clear();warmed.clear();g.engine=engine;g.aimPhysicalDown=false;g.running=true;
            aimThread(engine);
            if(observed!=expected) { for(int size:observed) std::cerr<<size<<' '; std::abort(); }
            assert(warmed==expectedWarm && sent==0);
            g.captureRunning=true;
        };
        for(int engine:{1,2,3}) for(bool remote:{false,true}) run(engine,remote);
        if(!single) {
            // The largest file can fail warm-up: use the largest successful model.
            failWarmSize=512;
            scenario={{0,false},{0,false},{0,true,true,900,900},{0,true,false},{0,false}};
            expected=std::vector<int>(scenario.size(),416);
            for(int engine:{1,2,3}) run(engine);
            failWarmSize=0;
            // A runtime failure removes that stage and resets the search maximum.
            scenario={{0,false,true,100,100,true},{0,false},{0,true,true,900,900},{0,false}};
            expected={512,416,416,416};
            for(int engine:{1,2,3}) run(engine);
        }
    }
    fixedModelFixture=true;
    for(auto ext:{L".onnx",L".param",L".bin",L".trt"}) std::ofstream(g.modelDirectory()/(std::wstring(L"fixed")+ext)).put('x');
    g.modelName="fixed"; g.manualCropSize=416;
    scenario={{0,false},{0,true,true,900,900},{0,true,false},{0,false}};
    for(int engine:{1,2,3}) {
        crop=512;g.imgSize=512;g.adaptiveCapture=true;
        frameIndex=0;observed.clear();warmed.clear();g.engine=engine;g.aimPhysicalDown=false;g.running=true;
        aimThread(engine);
        assert(observed==std::vector<int>(scenario.size(),416) && !g.adaptiveCapture);
        assert(g.manualCropSize==416 && warmed.empty() && sent==0);
    }
    // The maximum search becomes three native 640 tiles; approaching targets
    // still reach every smaller model. Losing/releasing restores all three.
    fixedModelFixture=false;wideFixture=true;g.modelName="dense";
    for(int engine:{1,2,3}) for(bool remote:{false,true}) {
        g.captureRunning=!remote;g.fullScreenSearch=true;
        if(remote) {
            g_frame.assign(54+1920*640*3,0);
            const auto field=[&](int offset,int value) { std::memcpy(g_frame.data()+offset,&value,4); };
            g_frame[0]='B';g_frame[1]='M';field(2,int(g_frame.size()));field(10,54);field(14,40);
            field(18,1920);field(22,-640);g_frame[26]=1;g_frame[28]=24;
            markPixels(g_frame.data()+54,1920,640);++g_frameSeq;
        }
        scenario={{-500,false},{500},{500},{0},{0},{0},{0},{0,true,false},{0},{0},{0},{0,false},{0,false}};
        // Switching from a distant person to the center also observes the existing lock hold frame.
        const std::vector<int> expectedSizes{640,640,640,640,640,640,640,160,640,640,640,160,640};
        const std::vector<int> expectedWidths{1920,1920,1920,1920,1920,1920,1920,160,1920,1920,1920,160,1920};
        frameIndex=0;tileCalls=0;observed.clear();observedWidths.clear();warmed.clear();liveFrame=false;
        g.engine=engine;g.aimPhysicalDown=false;g.running=true;
        aimThread(engine);
        if(observed!=expectedSizes || observedWidths!=expectedWidths) {
            std::cerr<<"wide engine="<<engine<<" remote="<<remote<<"\n";
            for(size_t i=0;i<observed.size();++i) std::cerr<<observedWidths[i]<<'x'<<observed[i]<<' ';
            std::cerr<<'\n';
        }
        assert(observed==expectedSizes && observedWidths==expectedWidths && sent==0);
        assert(g.drawWidth==1920 && g.drawSize==640 && g.boxes[0].x1==910 && g.nearestDist==0);
    }
    // Toggle while a frame is being processed: discard it and change capture
    // geometry on the next frame, including a switch from a small locked crop.
    g.captureRunning=true;g.fullScreenSearch=true;
    scenario={{-500},{-500},{500},{500,false,true,100,100,false,0},
        {0},{0},{0},{0,true,true,100,100,false,1},{500},{500}};
    frameIndex=0;observed.clear();observedWidths.clear();warmed.clear();liveFrame=false;g.engine=2;g.running=true;
    aimThread(2);
    assert((observedWidths==std::vector<int>{1920,1920,1920,1920,640,640,640,160,1920,1920}));
    assert(g.nearestDist==500 && g.boxDx==500 && g.moveDx==62 && g.moveDy==0);
    assert(g.boxes[0].x1==1410 && g.drawWidth==1920 && sent==0);
    for(int engine:{1,2,3}) {
        scenario={{0,false}};
        std::vector<int> expected{1920};
        int current=640;
        for(int size=608;size>=160;size-=32) {
            const float extent=size==160?100.f:float(size/1.2-.1);
            for(int frame=0;frame<3;++frame) {
                scenario.push_back({0,true,true,extent,extent});expected.push_back(current==640?1920:current);
            }
            current=size;
        }
        scenario.push_back({0});expected.push_back(160);
        frameIndex=0;observed.clear();observedWidths.clear();warmed.clear();liveFrame=false;
        g.engine=engine;g.aimPhysicalDown=false;g.running=true;
        aimThread(engine);
        assert(observedWidths==expected && sent==0);
    }
    scenario={{-500},{-500},{-500}};
    frameIndex=0;observed.clear();observedWidths.clear();liveFrame=false;g.engine=2;g.running=true;
    aimThread(2);
    assert(g.nearestDist==500 && g.boxDx==-500 && g.moveDx==-62 && g.moveDy==0);
    assert(g.boxes[0].x1==410 && g.drawWidth==1920 && sent==0);
    scenario={{0,false,true,100,100,true},{0,false},{0,false}};
    frameIndex=0;observed.clear();observedWidths.clear();liveFrame=false;g.running=true;
    aimThread(2);
    assert((observedWidths==std::vector<int>{1920,608,608}) && sent==0);
    g.fullScreenSearch=false;
    // Exercise actual production aim-point extraction with nested head (50%)
    // and body (15%) positions, varying body height, and missing head/body boxes.
    fixedModelFixture=true;wideFixture=false;alignedPointFixture=true;
    g.modelName="fixed";g.manualCropSize=320;g.classEnabled[1]=true;
    config.profiles[0].prioLockPos=50;config.profiles[0].secondLockPos=15;
    for(int amplitude:{0,1,15,49,50,51,100}) for(int engine:{1,2,3}) {
        config.profiles[0].speedCompensation=amplitude;
        config.profiles[0].centerRange=50;
        g.aimProfiles.replace(config);
        scenario=std::vector<FrameSpec>(90,FrameSpec{0});
        scenario.push_back({0,true,false});
        frameIndex=0;observed.clear();alignedMovesX.clear();alignedMovesY.clear();
        g.captureRunning=true;g.engine=engine;g.running=true;liveFrame=false;
        aimThread(engine);
        assert(alignedMovesX.size()==scenario.size());
        for(int y:alignedMovesY) assert(std::abs(y)<=1); // No vertical kick on part changes.
        assert(alignedMovesX.back()==0 && alignedMovesY.back()==0); // Full loss stops immediately.
        if(amplitude==0)assert(alignedMovesX[41]==0); // Disabled retains the original dead zone.
        else assert(alignedMovesX[41]!=0); // Every enabled strength can cross the observed center.
        const auto original=coefficient_move::calculate({-37.,0},config.profiles[0].moveFactor);
        if(amplitude==15) assert(alignedMovesX[15]<original.x);
        if(amplitude==0) assert(alignedMovesX[15]==original.x);
        for(int i:{20,30,60,70})
            assert(std::abs(alignedMovesY[i]-alignedMovesY[i-1])<=1);
        assert(sent==0);
    }
    alignedPointFixture=false;
    std::filesystem::remove_all(dir);
    std::cout<<"PASS production adaptive loop: all engines, target sizes, clipped targets, cached stages, shrink confirmation, lost/released recovery, matched model/frame, no device I/O\n";
}
