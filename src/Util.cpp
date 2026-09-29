#include "App.h"
#include "Inference.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

AppState g;

CRITICAL_SECTION g_frameLock;
CONDITION_VARIABLE g_publishedFrameReady = CONDITION_VARIABLE_INIT;
std::vector<unsigned char> g_frame;
long long g_frameSeq = 0;
long long g_lastFrameTime = 0;
long long g_nowMs = 0;

CRITICAL_SECTION g_drawLock;
CRITICAL_SECTION g_moveLock;

// ============================================================================
//  计时 / 延时
// ============================================================================
static double qpcFreqMs()
{
    static double f = 0.0;
    if (f == 0.0)
    {
        LARGE_INTEGER fr;
        QueryPerformanceFrequency(&fr);
        f = 1000.0 / (double)fr.QuadPart;
    }
    return f;
}

void RunTimer::start()
{
    QueryPerformanceCounter(&m_begin);
}

double RunTimer::ms() const
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - m_begin.QuadPart) * qpcFreqMs();
}

long long nowMs()
{
    return (long long)GetTickCount64();
}

void sleepMs(int ms)
{
    if (ms <= 0) { Sleep(0); return; }
    Sleep((DWORD)ms);
}

void hiSleep(double ms)
{
    if (ms <= 0) return;
    LARGE_INTEGER fr, a, b;
    QueryPerformanceFrequency(&fr);
    QueryPerformanceCounter(&a);
    double target = ms / (1000.0 / (double)fr.QuadPart);
    for (;;)
    {
        QueryPerformanceCounter(&b);
        if ((double)(b.QuadPart - a.QuadPart) >= target) break;
        SwitchToThread();
    }
}

double round2(double v)
{
    return std::round(v * 100.0) / 100.0;
}

// 易语言 取整()：向下取整（floor），-7.8 → -8
// 注意不是向零截断（那个是易语言的「绝对取整」/fix）
int truncToInt(double v)
{
    return (int)std::floor(v);
}

// 易语言把「小数型」赋给「整数型」变量时的隐式转换：四舍五入
int roundToInt(double v)
{
    return (int)std::llround(v);
}

int randInt(int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + (int)((double)rand() / ((double)RAND_MAX + 1.0) * (double)(hi - lo + 1));
}

double pidPosition(PidCtx& ctx, double err, double kp, double ki)
{
    // 对齐凌哥 L_运算_PID控制_位置（Kd=0，dt 默认 1，无输出限幅）：
    //   结构.输出 ＝ 0（含被 L_运算_PID控制_清空 后）→ 视为首次：
    //     积分累积 = 当前误差，但本次【积分项 = 0】，输出 = Kp·e
    //   否则：积分累积 += 当前误差 × dt，输出 = Kp·e + Ki·积分累积
    double out;
    if (ctx.prevOut == 0.0)
    {
        ctx.integral = err;
        out = kp * err;
    }
    else
    {
        ctx.integral += err;
        out = kp * err + ki * ctx.integral;
    }
    ctx.prevOut = out;
    return out;
}

// ============================================================================
//  文件 / 编码
// ============================================================================
bool fileExists(const std::string& path)
{
    DWORD a = GetFileAttributesA(path.c_str());
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

std::string trimStr(const std::string& s)
{
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

std::string ansiToUtf8(const std::string& s)
{
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    std::string u(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, &u[0], ulen, nullptr, nullptr);
    return u;
}

std::string utf8ToAnsi(const std::string& s)
{
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wlen);
    int alen = WideCharToMultiByte(CP_ACP, 0, w.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    std::string a(alen, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), wlen, &a[0], alen, nullptr, nullptr);
    return a;
}

std::string readFileAll(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return std::string();
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string s;
    if (n > 0)
    {
        s.resize((size_t)n);
        size_t rd = fread(&s[0], 1, (size_t)n, f);
        s.resize(rd);
    }
    fclose(f);
    return s;
}

bool writeFileAll(const std::string& path, const std::string& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = true;
    if (!data.empty()) ok = (fwrite(data.data(), 1, data.size(), f) == data.size());
    fclose(f);
    return ok;
}

// ============================================================================
// Internal direct calls.
bool sagaEnumDevices(std::string& out)
{
    const char* s = saga::enum_video_devices();
    out = s ? s : "";
    return true;
}

bool sagaEnumFormats(const char* dev, std::string& out)
{
    const char* s = saga::enum_device_formats(dev);
    out = s ? s : "";
    return true;
}

bool sagaSetCapture(const char* dev, int w, int h, const char* fourcc, int fps, int crop, std::string& out)
{
    const char* s = saga::setcjk_ex(dev, w, h, fourcc, fps, crop);
    out = s ? s : "";
    return true;
}


int sagaNcnnCreate(const char* p, const char* b, int gpu, int idx)
{
    return saga::ncnn_create(p, b, gpu, idx);
}
int sagaNcnnDetect(const unsigned char* d, int n, float c, float i, DetectObject* o)
{
    return saga::ncnn_detect(d, n, c, i, o);
}
int sagaOnnxCreate(const char* m, const char* l, bool gpu)
{
    return saga::onnx_create(m, l, gpu);
}
int sagaOnnxDetect(const unsigned char* d, int n, float c, float i, DetectObject* o)
{
    return saga::onnx_detect(d, n, c, i, o);
}

// ============================================================================
//  S_优化_Main / S_优化_Thread（精易模块「S_优化操作」）
//  S_优化_Main : 进程优先级 HIGH_PRIORITY_CLASS + 使用全部逻辑核心 + 启用 LFH 低碎片堆
//  S_优化_Thread: 当前线程 TIME_CRITICAL(15) + 关闭动态优先级提升
// ============================================================================
void optMain()
{
    HANDLE hp = GetCurrentProcess();
    SetPriorityClass(hp, HIGH_PRIORITY_CLASS);
    // Let Windows schedule workers without overriding user CPU affinity.
    ULONG lfh = 2;                                      // HeapCompatibilityInformation = 2
    HeapSetInformation(GetProcessHeap(), HeapCompatibilityInformation, &lfh, sizeof(lfh));
}

void optThread()
{
    HANDLE ht = GetCurrentThread();
    SetThreadPriority(ht, THREAD_PRIORITY_NORMAL);
    SetThreadPriorityBoost(ht, FALSE);                      // 匿名API_691(hThread, 假)
}

// ============================================================================
//  Makcu 封装
// ============================================================================
bool makcuConnect(int port)
{
    return (saga::makcuConnect(port) != 0);
}

void makcuMove(float x, float y)
{
    saga::makcuMove((int)std::round(x), (int)std::round(y));
}

void makcuDown(int key)
{
    saga::makcuMouseDown(key);
}

void makcuUp(int key)
{
    saga::makcuMouseUp(key);
}

void makcuState(int key, bool& out)
{
    out = (saga::makcuMouseButtonState(key) != 0);

}

// ============================================================================
//  模拟轨迹（对齐 模拟轨迹.txt）
// ============================================================================
static std::atomic<bool> g_trajReady{false};
static WIN32_FILE_ATTRIBUTE_DATA g_trajFileStamp{}; // Accessed only by the UI thread.

bool trajReady() { return g_trajReady; }

bool trajInit()
{
    const std::string trained = g.runDir + "\\人手数据\\mouse.bin";
    const std::string model = fileExists(trained) ? trained : g.runDir + "\\mouse.bin";
    const bool loaded = saga::mc_create(model.c_str()) != 0;
    if (loaded) g_trajReady = true;
    GetFileAttributesExA(trained.c_str(), GetFileExInfoStandard, &g_trajFileStamp);
    return loaded;
}

bool trajReloadIfChanged()
{
    const std::string trained = g.runDir + "\\人手数据\\mouse.bin";
    WIN32_FILE_ATTRIBUTE_DATA stamp{};
    if (!GetFileAttributesExA(trained.c_str(), GetFileExInfoStandard, &stamp) ||
        (stamp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (CompareFileTime(&stamp.ftLastWriteTime, &g_trajFileStamp.ftLastWriteTime) == 0 &&
        CompareFileTime(&stamp.ftCreationTime, &g_trajFileStamp.ftCreationTime) == 0 &&
        stamp.nFileSizeLow == g_trajFileStamp.nFileSizeLow &&
        stamp.nFileSizeHigh == g_trajFileStamp.nFileSizeHigh) return false;
    // Training publishes a complete file atomically. mc_create also swaps the
    // in-memory weights under a lock; a failed load preserves the running model.
    const bool loaded = saga::mc_create(trained.c_str()) != 0;
    g_trajFileStamp = stamp;
    if (loaded) g_trajReady = true;
    if (FILE* log = g_logFile()) {
        _lock_file(log);
        fprintf(log, "[traj] model hot reload: %s\n", loaded ? "success" : "failed; previous model retained");
        fflush(log);
        _unlock_file(log);
    }
    return loaded;
}

// out[20] = x1,y1,x2,y2,...,x10,y10 （0 基）
// mx[0..8] = x(i+2)-x(i+1) ; mx[9] = endX - x10
static void trajDelta(float endX, float endY, float* arr, float* mx, float* my)
{
    for (int j = 0; j < 9; ++j)
    {
        mx[j] = arr[2 * (j + 1)] - arr[2 * j];
        my[j] = arr[2 * (j + 1) + 1] - arr[2 * j + 1];
    }
    mx[9] = endX - arr[18];
    my[9] = endY - arr[19];
}

static bool needMove(float ax, float ay)
{
    return (ax * ax >= 1.0f) || (ay * ay >= 1.0f);
}

void trajMove10(float endX, float endY)
{
    if (!g_trajReady) return;
    float arr[20] = {}, mx[10] = {}, my[10] = {};
    saga::mc_calc(endX, endY, arr);
    trajDelta(endX, endY, arr, mx, my);

    float ax = mx[0], ay = my[0];
    if (needMove(ax, ay)) { makcuMove(ax, ay); hiSleep(0.885); ax = 0; ay = 0; }
    for (int j = 1; j < 10; ++j)
    {
        ax += mx[j]; ay += my[j];
        if (needMove(ax, ay))
        {
            makcuMove(ax, ay);
            if (j != 9) hiSleep(0.885);
            ax = 0; ay = 0;
        }
    }
}

void trajMove5(float endX, float endY)
{
    if (!g_trajReady) return;
    float arr[20] = {}, mx[10] = {}, my[10] = {};
    saga::mc_calc(endX, endY, arr);
    trajDelta(endX, endY, arr, mx, my);

    static const int grp[5][2] = { { 0, 1 },{ 2, 3 },{ 4, 5 },{ 6, 7 },{ 8, 9 } };
    float ax = 0, ay = 0;
    for (int k = 0; k < 5; ++k)
    {
        ax += mx[grp[k][0]] + mx[grp[k][1]];
        ay += my[grp[k][0]] + my[grp[k][1]];
        if (needMove(ax, ay))
        {
            makcuMove(ax, ay);
            if (k != 4) hiSleep(0.885);
            ax = 0; ay = 0;
        }
    }
}

void trajMove3(float endX, float endY)
{
    if (!g_trajReady) return;
    float arr[20] = {}, mx[10] = {}, my[10] = {};
    saga::mc_calc(endX, endY, arr);
    trajDelta(endX, endY, arr, mx, my);

    // 原版 局_累加像素 跨组保留残差，只有移动后才清零（与 10次/5次 一致）
    float ax = 0, ay = 0;
    for (int k = 0; k < 3; ++k)
    {
        if (k == 0)      { for (int j = 0; j <= 3; ++j) { ax += mx[j]; ay += my[j]; } }
        else if (k == 1) { for (int j = 4; j <= 6; ++j) { ax += mx[j]; ay += my[j]; } }
        else             { for (int j = 7; j <= 9; ++j) { ax += mx[j]; ay += my[j]; } }
        if (needMove(ax, ay))
        {
            makcuMove(ax, ay);
            if (k != 2) hiSleep(0.885);
            ax = 0; ay = 0;
        }
    }
}
