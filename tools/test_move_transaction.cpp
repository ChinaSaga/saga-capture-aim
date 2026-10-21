// Production movement consumer with in-memory device stubs only.
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
void sleepMs(int) { g.running = false; }
void optThread() {}
int truncToInt(double) { std::abort(); }
int roundToInt(double) { std::abort(); }
int randInt(int, int) { return 0; }
double pidPosition(PidCtx&, double, double, double) { std::abort(); }
bool fileExists(const std::filesystem::path&) { std::abort(); }
std::string utf8ToAnsi(const std::string&) { std::abort(); }
int presses = 0, releases = 0;
void makcuDown(int) { ++presses; }
void makcuUp(int) { ++releases; }
bool captureAcquire(CaptureFrame&, unsigned long long&, unsigned) { std::abort(); }
int ncnnDetectBgr(const unsigned char*, int, int, size_t, float, float, DetectObject*) { std::abort(); }
int onnxDetectBgr(const unsigned char*, int, int, size_t, float, float, DetectObject*) { std::abort(); }
int trtDetectBgr(const unsigned char*, int, int, size_t, float, float, DetectObject*) { std::abort(); }
int sagaNcnnCreate(const wchar_t*, const wchar_t*, int, int) { std::abort(); }
int sagaOnnxCreate(const wchar_t*, const wchar_t*, bool) { std::abort(); }
int sagaNcnnDetect(const unsigned char*, int, float, float, DetectObject*) { std::abort(); }
int sagaOnnxDetect(const unsigned char*, int, float, float, DetectObject*) { std::abort(); }
TrtTiming trtLastTiming() { std::abort(); }
namespace runtime_log { void write(const char*, ...) { std::abort(); } }
namespace saga {
void ncnn_destroy() { std::abort(); } void onnx_destroy() { std::abort(); } void trt_destroy() { std::abort(); }
const char* ncnn_last_error() { std::abort(); } const char* onnx_last_error() { std::abort(); } const char* trt_last_error() { std::abort(); }
int trt_create(const wchar_t*) { std::abort(); }
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
int main() {
    InitializeCriticalSection(&g_moveLock); InitializeCriticalSection(&g_drawLock);
    auto settings = g.settings.snapshot(); settings.delayFreq = 0; settings.moveDelay = 1;
    g.settings.replace(settings); g.aimKeyDown = true;
    publish(); publishDuringMove = true; aimMoveOnce();
    assert(sent == 1 && g.moveDx == 30 && g.moveDy == 40);
    publishDuringMove = false; publish(); g_lastFrameTime = 0; aimMoveOnce();
    assert(sent == 1 && g.moveDx == 0);
    g_lastFrameTime = 1000; publish(); ++g.inferenceEpoch; aimMoveOnce(); assert(sent == 1);
    publish(); g.moveFrameTime = 0; aimMoveOnce(); assert(sent == 1); // Capture live, inference stalled.
    publish(); inferPause(true); inferPause(false); aimMoveOnce(); assert(sent == 1);
    publish(); g.moveProfileToken = 0; aimMoveOnce(); assert(sent == 1);
    publish(); aimMoveOnce(); aimMoveOnce(); assert(sent == 2);
    g.triggerEpoch = g.inferenceEpoch.load(); g.triggerFire = g.triggerDown = true;
    g.resultFrameTime = 0; g.running = true; triggerThread();
    assert(presses == 0 && releases == 0);
    g.resultFrameTime = 1000; g.running = true; triggerThread();
    assert(presses == 1 && releases == 1);
    DeleteCriticalSection(&g_moveLock); DeleteCriticalSection(&g_drawLock);
    std::cout << "PASS movement consume-before-I/O, concurrent consumer, stale frame, pause and engine/profile invalidation\n";
}
