#include "StartupSession.h"
#include "App.h"
#include "Inference.h"
#include "FrameWait.h"
#include "UdpFrameAssembler.h"
#include <limits>
#include "RuntimeLog.h"
#include "ModelFiles.h"
#include "Screenshot.h"


#include <chrono>
#include <opencv2/imgcodecs.hpp>


// ============================================================================
//  从字节集取图片宽度（BMP 直接读头，JPEG 扫 SOF 标记）
// ============================================================================
static int imageWidth(const std::vector<unsigned char>& d)
{
    if (d.size() >= 54 && d[0] == 'B' && d[1] == 'M')
    {
        int w = 0;
        std::memcpy(&w, &d[18], 4);
        return w > 0 && w <= 32768 ? w : 0;
    }
    if (d.size() > 4 && d[0] == 0xFF && d[1] == 0xD8)
    {
        size_t i = 2;
        while (i + 9 < d.size())
        {
            if (d[i] != 0xFF) { ++i; continue; }
            unsigned char m = d[i + 1];
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            {
                return ((int)d[i + 7] << 8) | d[i + 8];     // SOF: height(2) width(2)
            }
            if (len < 2) break;
            i += 2 + len;
        }
    }
    return 0;
}

// ============================================================================
//  线程_死循环采集图片（本机采集卡）
// ============================================================================
void frameThreadCapture()
{
    optThread();
    RunTimer timer;
    timer.start();
    int count = 0;
    unsigned long long sequence = 0;
    CaptureFrame snapshot;
    while (g.running.load())
    {
        if (captureAcquire(snapshot, sequence, 50))
        {
            // Preview and training screenshots acquire their own short-lived
            // leases only when needed. Publishing statistics needs no pixels.
            snapshot = {};
            EnterCriticalSection(&g_frameLock);
            ++g_frameSeq;
            g_lastFrameTime = nowMs();
            LeaveCriticalSection(&g_frameLock);
            WakeAllConditionVariable(&g_publishedFrameReady);
            ++count;
        }
        const double elapsed = timer.ms();
        if (elapsed >= 1000.0)
        {
            g.frameFps = (int)std::round(count * 1000.0 / elapsed);
            timer.start();
            count = 0;
        }
    }
}

// ============================================================================
//  程_udp接收（双机模式）
// ============================================================================
void frameThreadUdp()
{
    optThread();                 // S_优化_Thread
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) { WSACleanup(); return; }

    sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((u_short)g.shotPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0)
    {
        closesocket(s);
        WSACleanup();
        return;
    }

    std::vector<unsigned char> buf(4096);
    std::vector<unsigned char> acc;
    UdpFrameAssembler assembler;
    DWORD timeout = 200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    RunTimer t;
    t.start();
    int count = 0;

    while (g.running.load())
    {
        sockaddr_in from;
        int fromLen = sizeof(from);
        int len = recvfrom(s, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&from, &fromLen);
        if (len < 0) {
            const int error = WSAGetLastError();
            if (error != WSAETIMEDOUT && error != WSAEMSGSIZE && error != WSAECONNRESET) break;
            assembler.reset();
            continue;
        }
        // A late UDP stream must not overwrite local capture dimensions/statistics.
        if (g.captureRunning) { assembler.reset(); continue; }
        const std::uint64_t peer = (std::uint64_t(from.sin_addr.s_addr) << 16) | from.sin_port;
        if (assembler.push(buf.data(), static_cast<size_t>(len), peer, GetTickCount64(), acc)) {
            const int width = imageWidth(acc);
            EnterCriticalSection(&g_frameLock);
            g_frame.swap(acc);
            ++g_frameSeq;
            g_lastFrameTime = nowMs();
            LeaveCriticalSection(&g_frameLock);
            WakeAllConditionVariable(&g_publishedFrameReady);
            if (++count >= 100) {
                if (width > 0 && !g.adaptiveCapture) g.imgSize = width;
                const double ms = t.ms();
                if (ms > 0) g.frameFps = (int)std::round(100000.0 / ms);
                t.start(); count = 0;
            }
        }
    }

    closesocket(s);
    WSACleanup();
}

// ============================================================================
//  接口_开始采集
//  成功后启动采集线程，并保存到 配置保存.ini 的采集恢复段。
// ============================================================================
// 上一次成功启动采集的参数：切换「中心识别范围」时原样重启用（裁剪尺寸换掉）
static std::string g_lastDev, g_lastCodec;
static int  g_lastW = 0, g_lastH = 0, g_lastFps = 0;

bool startCapture(const char* dev, int w, int h, const char* codec, int fps)
{
    saga::set_capture_log([](const char* text) { runtime_log::write("[capture] %s", text); });
    std::string name = dev ? dev : "";
    std::string cc = codec ? codec : "";
    const auto ini = g.runDir / L"配置保存.ini";
    const auto modelName = file_paths::wide(g.selectedModelName());
    const bool adaptive = model_files::isGroup(g.modelDirectory(), modelName);

    if (name.empty() || cc.empty() || w <= 0 || h <= 0 || fps <= 0) return false;

    if (adaptive) {
        const int largest = adaptive_capture::largestSize(model_files::groupSizes(g.modelDirectory() / modelName, g.engine.load()));
        if (largest) {
            g.imgSize = largest;
        }
    } else g.imgSize = g.manualCropSize.load();
    // 引擎（原 Saga.dll）以 UTF-8 匹配设备名，所以设备名回传前要 ansiToUtf8。
    // setcjk_ex 现在自行配平 COM 初始化，并会对设备枚举失败返回错误文本。
    std::string nameUtf8 = ansiToUtf8(name);
    std::string res;
    runtime_log::write("[capture] 请求启动：设备=%s，%dx%d@%d，编码=%s，裁剪=%d", name.c_str(), w, h, fps, cc.c_str(), (int)g.imgSize);
    sagaSetCapture(nameUtf8.c_str(), w, h, cc.c_str(), fps, g.imgSize, res);
    runtime_log::write("[capture] 初始化返回：%s", res.c_str());
    // 引擎源码现在与本文件用同一套字符集（执行字符集 GBK）编译，返回值与字面量都是 GBK，
    // 直接比字面量即可（原来引擎是 DLL、用 /utf-8 编，返回值是 UTF-8，所以要 ansiToUtf8("成功")）。
    if (res != "成功") return false;

    g.noConvert = true;
    if (!g.captureRunning.exchange(true))
    {
        std::thread(frameThreadCapture).detach();
    }

    if (!startup_session::save(ini, {name, cc, w, h, fps}, g.manualCropSize.load()))
        runtime_log::write("[capture] 无法保存启动恢复状态，错误=%lu", GetLastError());

    // 记住这次成功启动的参数，供「切换中心识别范围」时按新裁剪尺寸原样重启
    g_lastDev = name;
    g_lastCodec = cc;
    g_lastW = w;
    g_lastH = h;
    g_lastFps = fps;
    return true;
}

// ----------------------------------------------------------------------------
//  切换「中心识别范围」（组合框_视频大小）
//  易语言原版在采集中直接忽略（集_死循环 ＝ 真 保护），只能靠再点一次「开始采集」；
//  这里改成立即生效：暂停推理 → 用新裁剪尺寸重建采集图 → 等新画面 → 恢复推理，
//  这样采集画面与识别范围一起对上（引擎内部 stop + 重建，裁剪尺寸随 configure 生效）。
// ----------------------------------------------------------------------------
static long long frameSequence()
{
    EnterCriticalSection(&g_frameLock);
    const long long seq = g_frameSeq;
    LeaveCriticalSection(&g_frameLock);
    return seq;
}

static bool waitFrameAfter(long long sequenceBefore, int timeoutMs)
{
    EnterCriticalSection(&g_frameLock);
    const bool arrived = waitForFrameSequence(g_publishedFrameReady, g_frameLock,
        g_frameSeq, sequenceBefore, static_cast<DWORD>(timeoutMs));
    LeaveCriticalSection(&g_frameLock);
    return arrived;
}

bool applyCropSize(int size)
{
    if (g.adaptiveCapture) return false;
    if (size < 160 || size > 640) return false;
    if (size == g.imgSize) { g.manualCropSize = size; return true; }

    // 没在采集中（或还没有可用的采集参数）：只改数值，不用重建
    if (!g.captureRunning.load() || g_lastDev.empty())
    {
        g.imgSize = size;
        g.manualCropSize = size;
        return true;
    }

    const int previous = g.imgSize;
    const int previousManual = g.manualCropSize;

    // 1) 先暂停推理：重启期间不要把旧裁剪的画面配着新宽高去推理
    //    （inferPause 同时清掉画框与移动意图，避免旧坐标继续留在画面上）
    inferPause(true);

    // 2) 用新裁剪尺寸重建采集图（引擎内部 stop + 重建，裁剪随 configure 一起生效）
    g.imgSize = size;
    g.manualCropSize = size;
    bool ok = startCapture(g_lastDev.c_str(), g_lastW, g_lastH, g_lastCodec.c_str(), g_lastFps);
    if (!ok)
    {
        // 建图失败：退回原尺寸再建一次，至少把画面救回来
        g.imgSize = previous;
        g.manualCropSize = previousManual;
        startCapture(g_lastDev.c_str(), g_lastW, g_lastH, g_lastCodec.c_str(), g_lastFps);
        inferPause(false);
        return false;
    }

    // 3) 等采集线程收到重建后的帧再放开推理。
    //    序号在这里才取：重建成功后引擎里已经是新裁剪的帧，之后任何一次拷贝都必定是新画面
    waitFrameAfter(frameSequence(), 2000);
    inferPause(false);
    return true;
}

// ============================================================================
//  线程_死循环转换图片（画板绘制）
// ============================================================================
void frameThreadRender()
{
    long long renderedSequence = -1;
    unsigned long long renderedCaptureSequence = 0;
    std::vector<unsigned char> local;

    while (g.running.load())
    {
        if (g.showImg.load()) {
            if (g.captureRunning.load()) {
                CaptureFrame frame;
                if (captureAcquire(frame, renderedCaptureSequence, 0))
                    canvasDrawCapture(frame);
            } else {
                local.clear();
                EnterCriticalSection(&g_frameLock);
                if (renderedSequence != g_frameSeq) {
                    local = g_frame;
                    renderedSequence = g_frameSeq;
                }
                LeaveCriticalSection(&g_frameLock);
                if (!local.empty()) canvasDraw(local);
            }
        } else {
            renderedSequence = -1;
            renderedCaptureSequence = 0;
        }
        sleepMs(16);
    }
}

// ============================================================================
//  线程_截图保存（退格键自动截图）
// ============================================================================
void frameThreadShotSave()
{
    long long lastWrite = 0;
    unsigned long long captureSequence = 0;
    long long udpSequence = -1;
    std::vector<unsigned char> local, jpeg;
    screenshot::Writer writer;
    while (g.running.load())
    {
        long long now = nowMs();
        g_nowMs = now;

        if (g.settings.snapshot().autoShot && g.aimKeyDown.load())
        {
            if (now - g.lastDetectTime < 125 && now - g_lastFrameTime < 125 && now - lastWrite > 80)
            {
                CaptureFrame frame;
                if (g.captureRunning.load()) {
                    captureAcquire(frame, captureSequence, 0);
                    local.clear();
                } else {
                    local.clear();
                    EnterCriticalSection(&g_frameLock);
                    if (udpSequence != g_frameSeq) {
                        local = g_frame;
                        udpSequence = g_frameSeq;
                    }
                    LeaveCriticalSection(&g_frameLock);
                }

                if (frame.owner || !local.empty())
                {
                    // 原版文件名是 L_时间_取现行时间戳() = 13 位【毫秒】时间戳；
                    // 截图节流是 80ms（每秒最多 ~12 张），秒级文件名会互相覆盖
                    long long ts = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const auto model = g.selectedModelName();
                    // The capture lease already contains top-down BGR; preserve
                    // its stride and encode directly, without copying/decoding BMP.
                    try {
                        const bool encoded = screenshot::encode(frame, local, jpeg);
                        // Release the capture slot before disk I/O.
                        frame = {};
                        if (encoded && !writer.save(g.runDir, model, ts, jpeg))
                            OutputDebugStringA("Automatic screenshot: file write failed.\n");
                    } catch (const cv::Exception&) {
                        OutputDebugStringA("Automatic screenshot: JPEG encoding failed.\n");
                    }
                    lastWrite = now;
                }
            }
        }

        sleepMs(10);
    }
}

// ============================================================================
//  线程_截图显示（F1 切换）
// ============================================================================
void frameThreadF1()
{
    bool wasDown = false;
    while (g.running.load())
    {
        const SHORT state = GetAsyncKeyState(VK_F1);
        const bool down = (state & 0x8000) != 0;
        if ((down && !wasDown) || (!down && (state & 1))) uiSetShowImg(!g.showImg.load());
        wasDown = down;
        sleepMs(20);
    }
}
