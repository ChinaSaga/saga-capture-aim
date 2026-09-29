#include "App.h"
#include "AppExit.h"
#include "HumanTrajectory.h"

#include <cstdlib>
#include <mmsystem.h>

// 接口_文件存在提示检测
static void requireFile(const std::string& name)
{
    if (!fileExists(g.runDir + "\\" + name))
    {
        MessageBoxA(nullptr, (name + "不存在!请下载").c_str(), "系统提示", MB_OK | MB_SETFOREGROUND);
        ExitProcess(0);
    }
}

static bool missingDeps()
{
    // 原版 接口_文件存在提示检测 的清单：DirectML/ncnn/onnxruntime/opencv/Saga.dll。
    // 现在：
    //   · Saga.dll 的源码已并入本 EXE（src\），运行时不再需要它 → 移除
    //   · HPSocket4C.dll 由手写 Winsock 替代 → 不需要
    static const char* need[] = { "DirectML.dll", "ncnn.dll", "onnxruntime.dll", "opencv_world4120.dll" };
    for (const char* n : need)
        if (!fileExists(g.runDir + "\\" + n)) return true;
    return false;
}

// ---- 启动日志（定位崩溃用，可随时移除）----
static FILE* g_log = nullptr;
static ULONGLONG g_startTick = 0;
#define LOG(...) do { if (g_log) { _lock_file(g_log); fprintf(g_log, "[+%llums] ", GetTickCount64() - g_startTick); fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); _unlock_file(g_log); } } while (0)

FILE* g_logFile() { return g_log; }

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    g_startTick = GetTickCount64();
    // Native pixel rendering prevents blurry bitmap scaling on high-DPI displays.
    SetProcessDPIAware();
    int toolResult = 0;
    if (runHumanTrajectoryCommandLine(hInst, toolResult)) return toolResult;
    // 易语言的随机数命令会在运行期初始化；C 运行库默认种子固定，必须显式播种。
    srand((unsigned)(GetTickCount64() ^ (unsigned long long)GetCurrentProcessId()));

    // ---------------- 运行目录 ----------------
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string exe = path;
    g.runDir = exe.substr(0, exe.find_last_of("\\/"));
    g.cfgPath = g.runDir + "\\圣人自用.js";

    g_log = fopen((g.runDir + "\\SagaApp_startup.log").c_str(), "wb");
    LOG("[01] runDir=%s", g.runDir.c_str());
    LOG("[build] CH343 async/skip-absent v4; main-close/IP-reprompt/Esc-ignore v5; %s %s", __DATE__, __TIME__);

    g.running = true;
    SetDllDirectoryA(g.runDir.c_str());

    // Saga.dll 依赖 COM（DirectShow 设备枚举），且它自己会 CoUninitialize 拆掉公寓，
    // 所以主线程先占住一个计数（MTA 模式与 DLL 内部一致）
    {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        LOG("[01b] CoInitializeEx hr=0x%08X", (unsigned)hrCom);
    }

    CreateDirectoryA((g.runDir + "\\配置保存").c_str(), nullptr);

    // ---------------- 依赖文件检测（原版：缺文件弹框后 进程_结束）----------------
    if (missingDeps())
    {
        MessageBoxA(nullptr, "缺少 DirectML.dll / ncnn.dll / onnxruntime.dll / opencv_world4120.dll 中的一项",
            "系统提示", MB_OK | MB_SETFOREGROUND);
        return 0;
    }

    // ---------------- S_优化_Main ----------------
    // Scope pairs the request with timeEndPeriod, including normal early exits.
    struct TimerResolution {
        bool active = timeBeginPeriod(1) == TIMERR_NOERROR;
        ~TimerResolution() { if (active) timeEndPeriod(1); }
    } timerResolution;
    optMain();
    LOG("[02a] optMain 完成");

    // ---------------- 临界区 ----------------
    InitializeCriticalSection(&g_frameLock);
    InitializeCriticalSection(&g_drawLock);
    InitializeCriticalSection(&g_moveLock);
    LOG("[02] 临界区已建立");

    // ---------------- 引擎函数绑定（原 Saga.dll 那一步）----------------
    // 引擎源码（采集卡枚举/取帧、NCNN、ONNX、Makcu 串口、模拟轨迹）已编进本 EXE，
    // 这里只是把函数地址填进 p_* 指针，恒成功。

    LOG("[03] 引擎已就绪（原 Saga.dll 已并入本 EXE）");

    // ---------------- 配置 ----------------
    configInit();
    LOG("[04] configInit 完成");
    if (!trajInit())
    {
        LOG("[05] 人手模型未就绪，仍可进入界面记录和训练，轨迹移动已禁用");
    }
    else LOG("[05] trajInit 完成");

    g.shotPort = 6666;
    g.localPort = 8888;

    g.hostIp = "127.0.0.1"; // Local default; listeners separately accept LAN connections.

    // ---------------- 类别颜色（集_识别类型_颜色_数组）----------------
    const int colors[10] = {
        RGB(0, 230, 255), RGB(255, 76, 108), RGB(94, 255, 99),
        RGB(255, 207, 64), RGB(205, 125, 255), RGB(255, 139, 45),
        RGB(255, 102, 221), RGB(80, 157, 255), RGB(71, 255, 190),
        RGB(255, 255, 255)
    };
    for (int i = 0; i < 10; ++i) g.classColor[i] = colors[i];
    g.boxCount = 99;

    // ---------------- 界面 ----------------
    LOG("[07] 准备创建窗口");
    if (!uiCreate(hInst)) { LOG("[ER] uiCreate 失败"); return 0; }
    LOG("[08] 窗口创建完成");
    // Driver installation and serial handshaking must never delay the window.
    // The serial API publishes its connection atomically; until ready, input
    // and motion calls safely return without touching an unfinished connection.
    std::thread(makcuStartupThread).detach();
    LOG("[hw] Makcu 检测与连接已交给后台线程");
    if (!trajReady())
        MessageBoxA(nullptr, "尚无可用的人手轨迹模型。\n请点击“点我开始记录人手数据”，记录后点击“点我开始训练人手模型”。\n训练成功后自动使用新模型，无需重启程序。",
            "人手轨迹", MB_OK | MB_SETFOREGROUND);
    soundInit();

    // ---------------- 命令行分支（原版 _启动子程序 里的 取命令行 判断）----------------
    // 无参数 → 直接显示画面；有参数 → 隐藏画面 + 用 监控双机.ini 自动开始采集
    uiStartupCommandLine();
    LOG("[08b] 命令行分支完成: ip=%s showImg=%d", g.hostIp.c_str(), (int)g.showImg.load());

    // ---------------- 变量对应 ----------------
    // 对齐原版「接口_初始化默认设置」：启动时本地 HTTP 端口固定为 8888，
    // 同时把配置哈希表中的旧值覆盖掉，再执行变量同步与保存。
    configApplyStartupDefaults();
    configSyncAll(true);
    LOG("[09] configSyncAll 完成, port=%d", g.localPort);

    // ---------------- HTTP ----------------
    std::thread(httpServerThread).detach();
    LOG("[10] HTTP 线程已启动");

    // ---------------- 启动线程 ----------------
    std::thread(frameThreadF1).detach();
    std::thread(frameThreadRender).detach();
    std::thread(frameThreadShotSave).detach();
    std::thread(hotkeyThread).detach();
    std::thread(campWatchThread).detach();
    std::thread(crosshairWatchThread).detach();
    std::thread(lockTypeUpdateThread).detach();

    // 窗口创建完毕里启动的四个
    LOG("[11] 启动推理线程");
    std::thread([] { aimThread(1); }).detach();
    std::thread([] { aimThread(2); }).detach();
    std::thread(mouseMoveThread).detach();
    std::thread(triggerThread).detach();

    // 双机 UDP（采集卡模式下 全_图片无需转换 = 真，则不启动）
    if (!g.noConvert)
    {
        std::thread(frameThreadUdp).detach();
    }
    LOG("[12] 全部线程已启动, 进入消息循环");

    uiThread();
    LOG("[13] 消息循环退出");

    g.running = false;
    exitApplicationNow();
}
