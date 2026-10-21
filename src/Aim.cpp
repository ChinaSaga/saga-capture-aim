#include "Inference.h"
#include "App.h"
#include "AimLock.h"
#include "FrameWait.h"
#include "RuntimeLog.h"

#include <cstdarg>

// ============================================================================
//  接口_相对矩形中心的最大归一化比例
//  小于 1 说明在框内，大于 1 说明在框外
// ============================================================================
double normRatioRel(int px, int py, int x1, int y1, int x2, int y2)
{
    double cx = ((double)x1 + (double)x2) * 0.5;
    double cy = ((double)y1 + (double)y2) * 0.5;
    double hw = std::fabs((double)x2 - (double)x1) * 0.5;
    double hh = std::fabs((double)y2 - (double)y1) * 0.5;
    if (hw < 1e-6) hw = 1e-6;
    if (hh < 1e-6) hh = 1e-6;

    double dx = std::fabs((double)px - cx) / hw;
    double dy = std::fabs((double)py - cy) / hh;

    return (dx > dy) ? round2(dx) : round2(dy);
}

// ============================================================================
//  接口_鼠标移动
// ============================================================================
static RunTimer g_delayTimer;     // 集_延时时间
static RunTimer g_durTimer;       // 集_持续时间

void aimMoveOnce()
{
    static std::mutex consumerMutex;
    std::unique_lock consumer(consumerMutex, std::try_to_lock);
    if (!consumer.owns_lock()) return;
    const auto settings = g.settings.snapshot();
    // ---- 锁定频率 / 延时频率 门控 ----
    if (settings.delayFreq != 0)
    {
        if (g.delayStarted)
        {
            double dms = g_delayTimer.ms();
            if (dms < (double)settings.delayFreq) return;    // 延时期间不移动
            g.delayStarted = false;
            g.aimStarted = true;
        }

        if (g.aimStarted)
        {
            g_durTimer.start();
            g.aimStarted = false;
            g.delayStarted = false;
        }
        else
        {
            double ams = g_durTimer.ms();
            if (ams >= (double)settings.lockFreq && !g.delayStarted)
            {
                g.delayStarted = true;
                g_delayTimer.start();
            }
        }
    }

    // Consume before device I/O: a new frame published during the trajectory
    // belongs to the next call and must never be erased after this move.
    EnterCriticalSection(&g_moveLock);
    const bool valid = g.aimKeyDown && !g.inferPaused &&
        g.moveProfileToken == g.aimProfiles.token() && g.moveEpoch == g.inferenceEpoch &&
        nowMs() - g_lastFrameTime < 125 && nowMs() - g.moveFrameTime < 125;
    const float fx = valid ? (float)g.moveDx : 0.f;
    const float fy = valid ? (float)g.moveDy : 0.f;
    g.moveDx = g.moveDy = 0;
    LeaveCriticalSection(&g_moveLock);
    if (fx == 0 && fy == 0) return;

    switch (settings.moveDelay)
    {
    case 10: trajMove10(fx, fy); break;
    case 5:  trajMove5 (fx, fy); break;
    case 3:  trajMove3 (fx, fy); break;
    case 1:  makcuMove (fx, fy); break;
    default: trajMove10(fx, fy); break;   // 没选择移动延时默认 10 次
    }

}

// ============================================================================
//  线程_鼠标移动
// ============================================================================
void mouseMoveThread()
{
    optThread();                 // S_优化_Thread
    while (g.running.load())
    {
        const auto settings = g.settings.snapshot();
        if (settings.threadMove) aimMoveOnce();
        sleepMs(1);
    }
}

// ============================================================================
//  线程_扳机线程
// ============================================================================
void triggerThread()
{
    optThread();                 // S_优化_Thread
    long long lastShot = 0;
    while (g.running.load())
    {
        const auto settings = g.settings.snapshot();
        if (g.triggerFire && g.triggerDown && !g.inferPaused &&
            g.triggerEpoch == g.inferenceEpoch && nowMs() - g_lastFrameTime < 125 &&
            nowMs() - g.resultFrameTime < 125)
        {
            long long t = nowMs();
            if (t - lastShot > settings.shotInterval)
            {
                makcuDown(1);
                sleepMs(randInt(88, 138));
                makcuUp(1);
                lastShot = t;
            }
        }
        sleepMs(1);
    }
}

// ============================================================================
//  线程_死循环推理（NCNN = 1 / ONNX = 2 / TensorRT = 3 共用）
// ============================================================================
static void msgBox(const std::string& text)
{
    MessageBoxA(nullptr, text.c_str(), "系统提示", MB_OK | MB_SETFOREGROUND);
}

// ============================================================================
//  暂停 / 恢复推理线程（切换「中心识别范围」时用）
//  暂停期间推理线程空转，并且清掉画框与移动意图 —— 否则旧裁剪尺寸算出来的
//  坐标会配着新的 全_图片宽高 用，中心和框都会错位。
// ============================================================================
static void clearInferenceResult()
{
    EnterCriticalSection(&g_drawLock);
    g.canDraw = false;
    std::memset(g.boxes, 0, sizeof(g.boxes));
    LeaveCriticalSection(&g_drawLock);
    g.nearestDist = 0;
    g.triggerFire = false;
    EnterCriticalSection(&g_moveLock);
    g.moveDx = 0;
    g.moveDy = 0;
    LeaveCriticalSection(&g_moveLock);
}
void inferPause(bool paused)
{
    g.inferPaused = paused;
    if (paused) {
        ++g.inferenceEpoch;
        clearInferenceResult();
    }
}

// ============================================================================
//  选靶：同人物聚类 + 锁定粘滞 —— 逻辑全部在 AimLock.h（纯函数，可单测）
//  这里只负责把 targets[] 喂进去、把选中结果落到 最近距离/十字线 上。
//  为什么需要它见 AimLock.h 顶部的说明（头被折叠就跳人 + 头身互相抢锁 = 疯狂摇晃）。
// ============================================================================
void aimThread(int engineWanted)
{
    optThread();                 // S_优化_Thread
    // 注意：不能声明成 static，两个推理线程共用同一个 aimThread，
    // 静态数组会被两线程共享导致数据竞争（易语言里是两个独立子程序）
    DetectObject objs[99];
    LockTarget   targets[99];
    aimlock::Box cand[99];
    DrawBox drawSnapshot[99];

    PidCtx pidX, pidY;

    // 锁定记忆：上一帧锁定那个「人」的各部位框。必须是本线程的局部变量
    // （两个推理线程共用同一个 aimThread，写成 static 会互相打架）
    aimlock::Memory lockMem;
    std::uint64_t lastProfileToken = 0;
    std::uint64_t lastInferenceEpoch = 0;

    RunTimer runTimer, inferTimer;
    runTimer.start();
    char profileSetting[16]{};
    const bool profileTrt = engineWanted == 3 &&
        GetEnvironmentVariableA("SAGA_TRT_PROFILE", profileSetting, sizeof(profileSetting)) &&
        !strcmp(profileSetting, "1");
    ULONGLONG profileTick = GetTickCount64();
    unsigned profileFrames = 0;
    double profileTotal = 0, profilePre = 0, profileSubmit = 0,
        profileWait = 0, profilePost = 0, profileGpu = 0;

    std::vector<unsigned char> localFrame;
    long long localSeq = -1;
    unsigned long long captureSequence = 0;
    CaptureFrame captureFrame;
    std::string loadedModel;
    bool modelReady = false;
    bool firstInference = true;
    auto settings = g.settings.snapshot();
    float accuracy = settings.conf;
    if (accuracy < 0.1f) accuracy = 0.1f;
    int   detectCount = 0;
    int   prevNearestRange = 0;        // 局_画框最近范围（上一帧距离）
    // 以下三个跨迭代保持，对应原版子程序级的 局_最近距离x/y 与 局_最近矩阵
    int   nearestX = 0, nearestY = 0;
    SRect nearestRect;
    bool  keepGoing = true;

    // 等到第一帧图片
    while (g.running.load())
    {
        EnterCriticalSection(&g_frameLock);
        const bool ready = waitForFrameSequence(g_publishedFrameReady, g_frameLock,
            g_frameSeq, 0, 50);
        LeaveCriticalSection(&g_frameLock);
        if (ready) break;
    }

    while (g.running.load())
    {
        if (g.engine != engineWanted) {
            // Keep only the selected engine's GPU allocations resident. Returning
            // to an engine also retries a model whose previous load failed.
            if (!loadedModel.empty()) {
                if (engineWanted == 1) saga::ncnn_destroy();
                else if (engineWanted == 2) saga::onnx_destroy();
                else saga::trt_destroy();
                loadedModel.clear();
                modelReady = false;
            }
            sleepMs(5);
            continue;
        }
        if (g.inferPaused.load()) { sleepMs(2); continue; }   // 切换识别范围中：等新画面
        const bool directCapture = g.captureRunning.load();
        long long inputFrameTime = 0;
        if (!directCapture) {
            EnterCriticalSection(&g_frameLock);
            const bool freshFrame = waitForFrameSequence(g_publishedFrameReady,
                g_frameLock, g_frameSeq, localSeq, 50);
            if (freshFrame) { localFrame = g_frame; localSeq = g_frameSeq; inputFrameTime = g_lastFrameTime.load(); }
            LeaveCriticalSection(&g_frameLock);
            if (!freshFrame || g.inferPaused.load() || g.engine != engineWanted) continue;
        }

        settings = g.settings.snapshot();
        const auto inferenceEpoch = g.inferenceEpoch.load();
        if (lastInferenceEpoch != inferenceEpoch) {
            lockMem = {}; pidX = {}; pidY = {};
            prevNearestRange = nearestX = nearestY = 0;
            nearestRect = {};
            detectCount = 0; runTimer.start();
            lastInferenceEpoch = inferenceEpoch;
        }
        // ---------------- 模型加载 ----------------
        const std::string req = g.selectedModelName();
        if (g.engine != engineWanted) continue;
        if (loadedModel != req)
        {
            runtime_log::write("[infer] 开始加载模型：引擎=%d，模型=%s", engineWanted, req.c_str());
            firstInference = true;
            clearInferenceResult();
            modelReady = false;
            if (engineWanted == 1)
            {
                const auto par = g.modelDirectory() / file_paths::wide(req + ".param");
                const auto bin = g.modelDirectory() / file_paths::wide(req + ".bin");
                if (fileExists(bin) && fileExists(par))
                {
                    // 原版先用模型名"上锁"再加载：加载失败也不会反复重试
                    loadedModel = req;
                    int rc = sagaNcnnCreate(par.c_str(), bin.c_str(), 1, 0);
                    if (rc != 0)
                    {
                        const auto detail = utf8ToAnsi(saga::ncnn_last_error());
                        runtime_log::write("[infer] NCNN 加载失败：%s", detail.c_str());
                        msgBox("模型加载失败，错误码：" + std::to_string(rc) + "\n" + detail);
                        sleepMs(100);
                        continue;
                    }
                    modelReady = true;
                }
                else
                {
                    msgBox("模型:" + req + "的param或bin文件不存在!请放入程序同目录的模型数据文件夹后点击确定");
                    continue;
                }
            }
            else if (engineWanted == 2)
            {
                const auto onnx = g.modelDirectory() / file_paths::wide(req + ".onnx");
                if (fileExists(onnx))
                {
                    // The model and labels share the dedicated model directory.
                    const auto txt = g.modelDirectory() / L"空类别.txt";
                    // Labels are optional; loading a model must not require write permission.

                    loadedModel = req;                                // 同上，先上锁
                    int rc = sagaOnnxCreate(onnx.c_str(), txt.c_str(), true);
                    if (rc != 0)
                    {
                        const std::string detail = utf8ToAnsi(saga::onnx_last_error());
                        runtime_log::write("[infer] ONNX 加载失败：%s", detail.c_str());
                        msgBox("ONNX 模型加载失败：\n" + detail +
                            "\n\n若提示 onnxruntime.dll 不兼容，请完整替换新版程序及配套 DLL，不能只替换 EXE。");
                        sleepMs(100);
                        continue;
                    }
                    modelReady = true;
                }
                else
                {
                    msgBox("模型:" + req + "的onnx文件不存在!请放入程序同目录的模型数据文件夹后点击确定");
                    continue;
                }
            }
            else if (engineWanted == 3)
            {
                const auto enginePath = g.modelDirectory() / file_paths::wide(req + ".trt");
                if (!fileExists(enginePath)) {
                    msgBox("模型:" + req + "的trt文件不存在!请先用ONNX转TRT工具转换，再放入程序同目录的模型数据文件夹。");
                    continue;
                }
                loadedModel = req;
                if (saga::trt_create(enginePath.c_str()) != 0) {
                    runtime_log::write("[infer] TensorRT load failed: %s", utf8ToAnsi(saga::trt_last_error()).c_str());
                    msgBox("N卡专用模型加载失败：\n" + utf8ToAnsi(saga::trt_last_error()) +
                        "\n修复后切换一次推理引擎即可重试。");
                    continue;
                }
                runtime_log::write("[infer] TensorRT model ready: %s", req.c_str());
                modelReady = true;
            }
        }
        // Never invoke a failed or incomplete model. Existing ONNX/NCNN models
        // previously marked a failed load as loaded and could use a null object.
        if (!modelReady) { sleepMs(50); continue; }

        // ---------------- 帧率统计（每 100 次）----------------
        // Timeouts are not inference frames; acquire before counting a sample.
        if (directCapture && !captureAcquire(captureFrame, captureSequence, 50)) continue;
        if (directCapture) inputFrameTime = g_lastFrameTime.load();
        if (g.engine != engineWanted || g.inferPaused.load()) {
            captureFrame = {};
            continue;
        }
        detectCount++;
        if (detectCount >= 100)
        {
            accuracy = (settings.crossClass == 0) ? settings.conf : settings.crossConf;
            if (accuracy < 0.1f) accuracy = 0.1f;

            double statMs = runTimer.ms();
            if (statMs > 0) g.inferFps = roundToInt(100000.0 / statMs);
            detectCount = 0;
            runTimer.start();
        }

        // ---------------- 推理 ----------------
        inferTimer.start();
        if (firstInference) runtime_log::write("[infer] 模型已就绪，开始首次推理：引擎=%d", engineWanted);
        int n;
        if (directCapture) {
            const auto detect = engineWanted == 1 ? ncnnDetectBgr :
                engineWanted == 2 ? onnxDetectBgr : trtDetectBgr;
            n = detect(captureFrame.bmp + 54, captureFrame.width, captureFrame.height,
                captureFrame.stride, accuracy, settings.nms, objs);
        } else if (engineWanted == 1) {
            n = sagaNcnnDetect(localFrame.data(), (int)localFrame.size(), accuracy, settings.nms, objs);
        } else if (engineWanted == 2) {
            n = sagaOnnxDetect(localFrame.data(), (int)localFrame.size(), accuracy, settings.nms, objs);
        } else {
            n = saga::trt_detect(localFrame.data(), (int)localFrame.size(), accuracy, settings.nms, objs);
        }
        captureFrame = {};
        const double inferCost = inferTimer.ms();      // 亚毫秒精度，界面换算显示用帧率
        if (firstInference) {
            runtime_log::write("[infer] 首次推理返回：引擎=%d，结果=%d，耗时=%.3fms", engineWanted, n, inferCost);
            firstInference = false;
        }
        if (profileTrt && n >= 0) {
            const auto timing = trtLastTiming();
            ++profileFrames; profileTotal += inferCost;
            profilePre += timing.preprocessMs; profileSubmit += timing.submitMs;
            profileWait += timing.waitMs; profilePost += timing.postprocessMs;
            profileGpu += timing.gpuMs;
            const auto tick = GetTickCount64();
            if (tick - profileTick >= 5000) {
                const double count = double(profileFrames);
                runtime_log::write("[trt-perf] frames=%u total=%.3f pre=%.3f submit=%.3f wait=%.3f post=%.3f gpu=%.3f ms graph=%d blocking=%d",
                    profileFrames, profileTotal/count, profilePre/count, profileSubmit/count,
                    profileWait/count, profilePost/count, profileGpu/count,
                    int(timing.graphActive), int(timing.blockingSync));
                profileFrames = 0; profileTotal = profilePre = profileSubmit =
                    profileWait = profilePost = profileGpu = 0; profileTick = tick;
            }
        }
        // A model/engine change during inference must not publish the old result.
        if (g.engine != engineWanted || g.inferPaused.load() ||
            g.inferenceEpoch != inferenceEpoch || g.selectedModelName() != req) continue;
        g.inferMs = (int)inferCost;
        g.inferMsPrecise = inferCost;
        g.resultFrameTime = inputFrameTime;
        if (n < 0 && engineWanted == 3) {
            modelReady = false;
            clearInferenceResult();
            runtime_log::write("[infer] TensorRT inference failed: %s", utf8ToAnsi(saga::trt_last_error()).c_str());
            msgBox("N卡专用推理失败：\n" + utf8ToAnsi(saga::trt_last_error()) +
                "\n请重新转换模型，或切换推理引擎后重试。");
            continue;
        }
        if (n < 0) n = 0;
        if (n > 99) n = 99;

        // ---------------- 画框数据 ----------------
        if (g.showImg.load())
        {
            int bi = 0;
            for (int i = 0; i < n; ++i)
            {
                int cls = objs[i].label + 1;
                if (cls < 1 || cls > 10) continue;
                if (!g.classEnabled[cls - 1]) continue;
                drawSnapshot[bi].color = g.classColor[cls - 1];
                drawSnapshot[bi].category = cls;
                drawSnapshot[bi].confidence = objs[i].prob;
                // 原版 二维画框_数组 是整数型数组，写入小数型矩形会四舍五入
                drawSnapshot[bi].x1 = roundToInt(objs[i].x);
                drawSnapshot[bi].y1 = roundToInt(objs[i].y);
                drawSnapshot[bi].x2 = roundToInt(objs[i].x + objs[i].width);
                drawSnapshot[bi].y2 = roundToInt(objs[i].y + objs[i].height);
                ++bi;
                if (bi >= 99) break;
            }
            EnterCriticalSection(&g_drawLock);
            std::memcpy(g.boxes, drawSnapshot, (size_t)bi * sizeof(DrawBox));
            if (bi < 99) g.boxes[bi] = {}; // Renderer stops at the first empty entry.
            g.canDraw = true;
            LeaveCriticalSection(&g_drawLock);
        }

        // Read one complete profile for this frame. Clear sticky target/PID memory
        // when keys or settings change so the previous ordering cannot hold the lock.
        const auto profileSnapshot = g.aimProfiles.active();
        const auto& profile = profileSnapshot.profile;
        if (lastProfileToken != profileSnapshot.token) {
            lockMem = {};
            pidX = {}; pidY = {};
            prevNearestRange = 0;
            nearestX = nearestY = 0;
            nearestRect = {};
            lastProfileToken = profileSnapshot.token;
        }

        // ---------------- 鼠标中心点（准星）----------------
        int idx = 0;
        int crossCount = 0;
        int centerX = roundToInt(g.imgSize * 0.5);
        int centerY = roundToInt(g.imgSize * 0.5);

        if (settings.crossClass > 0 && g.crossTrack.load())
        {
            for (int i = 0; i < n; ++i)
            {
                int cls = objs[i].label + 1;
                if (cls < 1 || cls > 10) continue;
                if (cls == settings.crossClass)
                {
                    crossCount++;
                    if (crossCount >= 2)          // 有且只有 1 个才采用
                    {
                        centerX = roundToInt(g.imgSize * 0.5);
                        centerY = roundToInt(g.imgSize * 0.5);
                        break;
                    }
                    // 原版：局_鼠标中心点X ＝ (x ＋ x ＋ 宽度) × 0.5，小数赋给整数型 → 四舍五入
                    centerX = roundToInt(((double)objs[i].x + objs[i].x + objs[i].width) * 0.5);
                    centerY = roundToInt(((double)objs[i].y + objs[i].y + objs[i].height) * 0.5);
                }
            }
        }

        // ---------------- 构建锁定目标数组 ----------------
        idx = 0;
        for (int i = 0; i < n; ++i)
        {
            int cls = objs[i].label + 1;
            if (cls < 1 || cls > 10) continue;
            if (objs[i].prob < settings.conf) continue;
            if (!g.classEnabled[cls - 1]) continue;

            int tx = roundToInt(((double)objs[i].x + objs[i].x + objs[i].width) * 0.5) - centerX;
            int ty = roundToInt(((double)objs[i].y + objs[i].y + objs[i].height) * 0.5) - centerY;

            double dist = std::sqrt((double)tx * tx + (double)ty * ty);

            float rx1 = objs[i].x, ry1 = objs[i].y;
            float rx2 = objs[i].x + objs[i].width, ry2 = objs[i].y + objs[i].height;
            float rh = objs[i].height;

            LockTarget t;
            if (cls == profile.prioLock)
            {
                if (profile.prioLockPos > 50) ty = roundToInt(ty + rh * std::fabs((double)(profile.prioLockPos - 50)) * 0.01);
                if (profile.prioLockPos < 50) ty = roundToInt(ty - rh * std::fabs((double)(profile.prioLockPos - 50)) * 0.01);
                t.lockType = 1;
                t.normRatio = normRatioRel(centerX, centerY, roundToInt(rx1), roundToInt(ry1), roundToInt(rx2), roundToInt(ry2));
            }
            else if (cls == profile.secondLock)
            {
                if (profile.secondLockPos > 50) ty = roundToInt(ty + rh * std::fabs((double)(profile.secondLockPos - 50)) * 0.01);
                if (profile.secondLockPos < 50) ty = roundToInt(ty - rh * std::fabs((double)(profile.secondLockPos - 50)) * 0.01);
                t.lockType = 2;
                t.normRatio = normRatioRel(centerX, centerY, roundToInt(rx1), roundToInt(ry1), roundToInt(rx2), roundToInt(ry2));
            }
            else if (cls == profile.thirdLock)
            {
                if (profile.thirdLockPos > 50) ty = roundToInt(ty + rh * std::fabs((double)(profile.thirdLockPos - 50)) * 0.01);
                if (profile.thirdLockPos < 50) ty = roundToInt(ty - rh * std::fabs((double)(profile.thirdLockPos - 50)) * 0.01);
                t.lockType = 3;
                t.normRatio = normRatioRel(centerX, centerY, roundToInt(rx1), roundToInt(ry1), roundToInt(rx2), roundToInt(ry2));
            }
            else
            {
                t.lockType = 99;
                t.normRatio = normRatioRel(centerX, centerY, roundToInt(rx1), roundToInt(ry1), roundToInt(rx2), roundToInt(ry2));
            }

            t.distance = dist;
            t.lockX = tx;
            t.lockY = ty;
            t.rect.x = rx1; t.rect.y = ry1; t.rect.width = objs[i].width; t.rect.height = rh;
            t.conf = objs[i].prob;

            targets[idx++] = t;
            if (idx >= 99) break;
        }

        // ---------------- 没人时清空 PID ----------------
        if (!g.aimKeyDown.load() || idx == 0)
        {
            if (settings.moveMode == 2) { pidX = PidCtx(); pidY = PidCtx(); }
        }
        else
        {
            g.lastDetectTime = nowMs();
        }

        // ---------------- 选靶：同人物聚类 + 锁定粘滞（逻辑在 AimLock.h）----------------
        for (int i = 0; i < idx; ++i)
        {
            const LockTarget& t = targets[i];
            cand[i].x = t.rect.x;
            cand[i].y = t.rect.y;
            cand[i].w = t.rect.width;
            cand[i].h = t.rect.height;
            cand[i].dx = t.lockX;
            cand[i].dy = t.lockY;
            cand[i].distance = t.distance;
            cand[i].normRatio = t.normRatio;
            cand[i].conf = t.conf;
            cand[i].lockType = t.lockType;
        }
        aimlock::Params lockParam;
        lockParam.lockRange = profile.lockRange;
        lockParam.prevNearestRange = prevNearestRange;
        lockParam.moveInBox = profile.moveInBox;
        const aimlock::Result picked = aimlock::select(cand, idx, lockParam, lockMem);

        // 注意：nearestX/nearestY 必须跨迭代保持（原版 局_最近距离x/y 是子程序级局部变量）。
        // 没选到目标时不清零，"上一次最近范围" 与画板上的锁定十字线才不会缩回中心。
        keepGoing = true;
        if (picked.index >= 0)
        {
            const LockTarget& t = targets[picked.index];
            nearestX = t.lockX;
            nearestY = t.lockY;
            g.nearestDist = roundToInt(t.distance);
            g.prioRatio = t.normRatio;
            nearestRect = t.rect;
            keepGoing = false;
        }
        else
        {
            // No current target: revoke any unconsumed movement from an older frame.
            EnterCriticalSection(&g_moveLock);
            g.moveDx = 0;
            g.moveDy = 0;
            LeaveCriticalSection(&g_moveLock);
            g.nearestDist = 0;
        }

        g.boxDx = nearestX;
        g.boxDy = nearestY;
        prevNearestRange = roundToInt(std::sqrt((double)nearestX * nearestX + (double)nearestY * nearestY));

        // ---------------- 扳机 + 移动 ----------------
        // 【修复点】原版只在"选到目标"分支里清 扳机开火，没目标时会残留上一帧
        // 的真值导致持续开火；这里统一先清零。
        g.triggerFire = false;
        g.triggerEpoch = inferenceEpoch;

        if (!keepGoing)
        {
            double crossRatio = normRatioRel(centerX, centerY,
                roundToInt(nearestRect.x), roundToInt(nearestRect.y),
                roundToInt(nearestRect.x + nearestRect.width),
                roundToInt(nearestRect.y + nearestRect.height));

            if (crossCount == 1 && g.crossTrack.load() && settings.crossTrigger)
            {
                g.prioRatio = crossRatio;
                if (crossRatio < (double)settings.triggerRange && crossRatio > 0) g.triggerFire = true;
            }

            if (settings.centerTrigger)
            {
                g.prioRatio = crossRatio;
                if (crossRatio < (double)settings.triggerRange && crossRatio > 0) g.triggerFire = true;
            }

            // 中心点范围仅限制优先锁定类别；其它类别继续追向锁定位置 XY。
            if (targets[picked.index].lockType == 1 &&
                crossRatio > 0 && crossRatio <= (double)profile.centerRange / 100.0)
            {
                EnterCriticalSection(&g_moveLock);
                g.moveDx = g.moveDy = 0;
                LeaveCriticalSection(&g_moveLock);
                continue;
            }

            // ---- 移动量计算 ----
            if (settings.moveMode == 1)
            {
                if (g.nearestDist.load() <= 100)
                {
                    if (nearestX < 0) nearestX = truncToInt(nearestX * settings.moveFactor);
                    if (nearestY < 0) nearestY = truncToInt(nearestY * settings.moveFactor);
                    if (nearestX > 0) nearestX = truncToInt(nearestX * settings.moveFactor + 0.9999);
                    if (nearestY > 0) nearestY = truncToInt(nearestY * settings.moveFactor + 0.9999);
                }
                else
                {
                    double scale = (double)g.nearestDist.load() / 100.0;
                    if (nearestX < 0) nearestX = truncToInt(nearestX * settings.moveFactor / scale);
                    if (nearestY < 0) nearestY = truncToInt(nearestY * settings.moveFactor / scale);
                    if (nearestX > 0) nearestX = truncToInt(nearestX * settings.moveFactor / scale + 0.9999);
                    if (nearestY > 0) nearestY = truncToInt(nearestY * settings.moveFactor / scale + 0.9999);
                }
            }
            else if (settings.moveMode == 2)
            {
                nearestX = roundToInt(pidPosition(pidX, (double)nearestX, settings.kp, settings.ki));
                nearestY = roundToInt(pidPosition(pidY, (double)nearestY, settings.kp, settings.ki));
            }

            EnterCriticalSection(&g_moveLock);
            g.moveDx = nearestX;
            g.moveDy = nearestY;
            g.moveProfileToken = profileSnapshot.token;
            g.moveEpoch = inferenceEpoch;
            g.moveFrameTime = inputFrameTime;
            LeaveCriticalSection(&g_moveLock);
            if (!settings.threadMove) aimMoveOnce();
        }

        // The next iteration waits for a fresh frame; no fixed per-frame delay.
    }
}
