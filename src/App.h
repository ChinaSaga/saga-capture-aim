// ============================================================================
//  SagaApp —— 易语言「圣人自用黑月」的 C++ 复刻版
//  所有命名 / 数值语义严格对齐易语言源码，详见 docs/易语言转C++规划.md
// ============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Shared detection result.
#include "Inference.h"
#include "ModelPaths.h"
#include "FilePaths.h"
#include "AimProfiles.h"
#include "RuntimeSettings.h"


// ============================================================================
//  数据类型
// ============================================================================
struct SRect
{
    float x = 0, y = 0, width = 0, height = 0;
};

// 易语言 锁定目标类
struct LockTarget
{
    int    lockX = 0;          // 锁定位置x
    int    lockY = 0;          // 锁定位置y
    double normRatio = 0;      // 归一化比例
    double distance = 0;       // 目前距离
    int    lockType = 0;       // 1优先 2其次 3再次 99其它 0空
    SRect  rect;
    float  conf = 0;
};

// 易语言 二维画框_数组[99][5]，下标 [i][1..5]，1 基
struct DrawBox
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, color = 0;
    int category = 0;
    float confidence = 0.0f;
};

// ============================================================================
//  全局变量（对应易语言 .全局变量.txt + 程序集变量）
// ============================================================================
struct AppState
{
    aim_profiles::Store aimProfiles;
    RuntimeSettingsStore settings;
    std::atomic<std::uint64_t> inferenceEpoch{1};
    std::atomic<std::uint64_t> triggerEpoch{0};
    std::atomic<long long> resultFrameTime{0};
    std::uint64_t moveEpoch = 0; // Protected by g_moveLock.
    long long moveFrameTime = 0; // Protected by g_moveLock.
    std::atomic<std::uint64_t> moveProfileToken{0};
    // ---- 运行控制 ----
    std::atomic<bool> running{ false };        // 全_死循环
    std::atomic<bool> inferPaused{ false };    // 切换识别范围期间暂停推理（等新画面到位）
    std::filesystem::path runDir;                  // 全_运行目录
    std::filesystem::path modelDirectory() const { return file_paths::io(model_paths::directory(runDir)); }
    std::filesystem::path cfgPath;                 // 全_配置文件
    std::string       hostIp = "127.0.0.1";    // 文本_推理机ip
    int               shotPort = 6666;         // 整数_截图端口
    int               localPort = 8888;        // 整数_本地端口

    // ---- 帧率统计 ----
    std::atomic<int>  inferFps{0};                          // 整数_推理帧率
    std::atomic<int>  frameFps{0};                          // 整数_图片帧率
    std::atomic<int>  inferMs{0};                          // 整数_推理毫秒
    std::atomic<double> inferMsPrecise{0.0};               // 亚毫秒精度的单次推理耗时（仅界面显示用）

    // ---- 图片 ----
    std::atomic<int>   imgSize{320};                        // 全_图片宽高
    std::atomic<int>   manualCropSize{320};                 // 单模型手动范围，不随自适应切换
    std::atomic<bool> adaptiveCapture{false};
    std::atomic<bool> noConvert{false};        // 全_图片无需转换（后台采集初始化会写入）
    std::atomic<bool> showImg{ false };        // 全_是否显示图片

    // ---- 识别参数 ----
    int   lockRange = 256;                     // 整数_锁定范围
    int   centerRange = 50;                    // 仅优先锁定类别的中心点停移范围 (内部 /100.0)
    mutable std::mutex modelMutex;
    std::string modelName = "三角洲行动";      // 文本_模型名称，读写须持 modelMutex
    std::string selectedModelName() const {
        std::lock_guard<std::mutex> guard(modelMutex);
        return modelName;
    }
    std::atomic<int> engine{1};                 // 1=NCNN 2=ONNX 3=TensorRT

    // ---- 锁定类别与位置 ----
    int prioLock = 1, secondLock = 2, thirdLock = 3;              // 整数_优先/其次/再次锁定
    int prioLockPos = 50, secondLockPos = 50, thirdLockPos = 50;  // 整数_*锁定位置

    // ---- 热键 ----
    int aimMainKey = 2;                        // 整数_自瞄主键
    int aimSubKey1 = 0, aimSubKey2 = 0, aimSubKey3 = 0;
    std::atomic<int> rangePressKey{0};
    std::atomic<bool> rangePressActive{false};

    // ---- 开关（文本型 "true"/"false"）----
    std::string moveInBox     = "false";       // 文本_框内移动
    std::string alwaysOn      = "false";       // 文本_吸附常驻

    // ---- 状态标志 ----
    std::atomic<bool> aimKeyDown{ false };     // 逻辑_自瞄热键是否按下
    std::atomic<bool> aimPhysicalDown{false};
    std::atomic<bool> crossTrack{ false };     // 逻辑_准星追踪
    std::atomic<bool> triggerDown{ false };    // 逻辑_扳机按下
    std::atomic<bool> triggerFire{ false };    // 逻辑_扳机开火

    // ---- 阵容 ----
    std::atomic<int> camp{ 0 };                // 整数_目前阵容 0/1/2

    // ---- 程序集变量 ----
    std::atomic<bool> classEnabled[10]{};                // 集_识别类型_数组[1..10]
    int  classColor[10]   = {};                // 集_识别类型_颜色_数组
    DrawBox boxes[99];                         // 二维画框_数组[99]
    int  boxCount = 99;                        // 集_数组成员数
    bool canDraw = false;                      // 集_可以画框
    int drawSize = 0; // Protected by g_drawLock, dimensions of the published boxes.
    int  moveDx = 0, moveDy = 0;               // 集_移动距离x/y
    std::atomic<int> boxDx{0}, boxDy{0};               // 集_画框距离x/y
    std::atomic<int> nearestDist{ 0 };         // 集_最近距离
    std::atomic<double> prioRatio{0};                      // 集_优先锁定比例
    std::atomic<long long> lastDetectTime{0};              // 集_上一次识别到时间

    bool delayStarted = false;                 // 集_延时启动
    bool aimStarted   = false;                 // 集_自瞄开始

    std::atomic<bool> captureRunning{ false }; // 集_死循环（采集线程是否已启动）
};

extern AppState g;

// 帧缓冲（全_图片字节集）
extern CRITICAL_SECTION g_frameLock;
extern CONDITION_VARIABLE g_publishedFrameReady;
extern std::vector<unsigned char> g_frame;
extern long long g_frameSeq;                  // 全_图片序号
extern std::atomic<long long> g_lastFrameTime;             // 全_图片上一次接收时间
extern std::atomic<long long> g_nowMs;                     // 全_现在时间

extern CRITICAL_SECTION g_drawLock;           // L_画框线程许可区
extern CRITICAL_SECTION g_moveLock;           // L_移动线程许可区

// ============================================================================
//  Util
// ============================================================================
class RunTimer                                 // L_运行计时
{
public:
    void start();
    double ms() const;
private:
    LARGE_INTEGER m_begin{};
};

long long nowMs();                             // L_系统_取启动时间 / 系统_取启动时间
void      sleepMs(int ms);                     // 延时()
void      hiSleep(double ms);                  // L_延时_高精度()
double    round2(double v);                    // 四舍五入(v, 2)
int       truncToInt(double v);                // 取整() —— 向下取整 floor
int       roundToInt(double v);                // 小数→整数 的隐式赋值转换：四舍五入
int       randInt(int lo, int hi);             // L_运算_取随机数()
bool      fileExists(const std::filesystem::path& path);
// Legacy driver installer paths originate in Win32 A APIs. Application
// resources use the Unicode filesystem::path overload above.
inline bool fileExists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
std::string trimStr(const std::string& s);
std::string ansiToUtf8(const std::string& s);
std::string utf8ToAnsi(const std::string& s);
std::string readFileAll(const std::filesystem::path& path);
bool        writeFileAll(const std::filesystem::path& path, const std::string& data);

// 精易模块 S_优化_Main / S_优化_Thread
void optMain();                                // 进程级：HIGH 优先级 + LFH
void optThread();                              // 线程级：NORMAL 优先级 + 关动态提升

// 位置式 PID（移动方式 2），对齐凌哥 L_PID控制结构：积分累积 + 上次输出
struct PidCtx { double integral = 0; double prevOut = 0; };
double pidPosition(PidCtx& ctx, double err, double kp, double ki);

// ============================================================================
//  引擎薄封装（原 Saga.dll 的调用层，现在直接调本 EXE 内的引擎函数）
// ============================================================================
bool          sagaEnumDevices(std::string& out);
bool          sagaEnumFormats(const char* dev, std::string& out);
bool          sagaSetCapture(const char* dev, int w, int h, const char* fourcc,
                             int fps, int crop, std::string& out);
int           sagaNcnnCreate(const wchar_t* param, const wchar_t* bin, int useGpu, int gpuIdx);
int           sagaNcnnDetect(const unsigned char* d, int n, float conf, float nms, DetectObject* o);
int           sagaOnnxCreate(const wchar_t* model, const wchar_t* labels, bool useGpu);
int           sagaOnnxDetect(const unsigned char* d, int n, float conf, float nms, DetectObject* o);

// ============================================================================
//  Makcu / 轨迹
// ============================================================================

// CH343（Makcu 盒子）驱动：驱动已打进 EXE 资源，缺了就静默装（DriverSetup.cpp）
bool makcuDriverReady();                       // 只检测，不起安装
bool ensureMakcuDriver(bool* installedNow = nullptr);   // 设备在场且缺驱动时从资源安装
void makcuStartupThread();                     // 窗口显示后在后台检测与连接
const char* makcuStartupStatus();               // UI 读取线程安全的连接状态

bool makcuConnect(int port);
void makcuMove(float x, float y);
void makcuDown(int key);
void makcuUp(int key);
void makcuState(int key, bool& out);

bool trajInit();
bool trajReady();
bool trajReloadIfChanged();
void trajMove10(float endX, float endY);
void trajMove5 (float endX, float endY);
void trajMove3 (float endX, float endY);

// ============================================================================
//  取帧
// ============================================================================
void frameThreadCapture();      // 线程_死循环采集图片
void frameThreadUdp();          // 程_udp接收
void frameThreadRender();       // 线程_死循环转换图片
void frameThreadShotSave();     // 线程_截图保存
void frameThreadF1();           // 线程_截图显示

// 接口_开始采集：使用明确的设备和格式参数，成功后保存启动恢复状态。
bool startCapture(const char* dev, int w, int h, const char* codec, int fps);

// 组合框_视频大小：切换中心识别范围（采集中也立即生效）
bool applyCropSize(int size);

// ============================================================================
//  推理 + 瞄准
// ============================================================================
double normRatioRel(int px, int py, int x1, int y1, int x2, int y2);
void   aimThread(int engineWanted);            // NCNN(1) / ONNX(2) / TensorRT(3)
void   inferPause(bool paused);                // 暂停/恢复推理线程（切换识别范围用）
void   mouseMoveThread();                      // 线程_鼠标移动
void   aimMoveOnce();                          // 接口_鼠标移动
void   triggerThread();                        // 线程_扳机线程

// ============================================================================
//  输入 / 状态
// ============================================================================
void hotkeyThread();                           // 线程_死循环热键监控
void crosshairWatchThread();                   // 线程_死循环准星监控
void campWatchThread();                        // 线程_死循环阵容监控
void lockTypeUpdateThread();                   // 线程_锁定类型更新

// ============================================================================
//  配置 / 网络
// ============================================================================
void configInit();                             // 接口_配置初始化
void configApplyStartupDefaults();             // 接口_初始化默认设置
void configSyncAll(bool save);                 // 变量全部对应一次
void configSaveFile();                         // 配置保存
void httpServerThread();                       // 接口_初始化_HTTP + HandleFun

// ============================================================================
//  界面
// ============================================================================
bool uiCreate(HINSTANCE hInst);
void uiThread();                               // 消息循环
HWND uiGetHwnd();
void uiSetShowImg(bool v);                     // 同步 选择框_实时刷新
void uiSetComboSizeIndex(int idx);             // 同步 组合框_视频大小
bool uiInputBox(const char* prompt, const char* title, std::string& text);
void uiRestoreCapture();                       // 恢复上次成功采集的设备和格式
void canvasDraw(const std::vector<unsigned char>& frame);   // 画板绘制
void soundInit();
void soundPlayCamp(int camp);
