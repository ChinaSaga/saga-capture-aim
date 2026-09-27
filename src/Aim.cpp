#include "Inference.h"
#include "App.h"
#include "AimLock.h"
#include "FrameWait.h"

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
    // ---- 锁定频率 / 延时频率 门控 ----
    if (g.delayFreq != 0)
    {
        if (g.delayStarted)
        {
            double dms = g_delayTimer.ms();
            if (dms < (double)g.delayFreq) return;    // 延时期间不移动
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
            if (ams >= (double)g.lockFreq && !g.delayStarted)
            {
                g.delayStarted = true;
                g_delayTimer.start();
            }
        }
    }

    if (!g.aimKeyDown.load()) return;
    if (g.moveDx == 0 && g.moveDy == 0) return;

    float fx, fy;
    if (g.threadMove == "true")
    {
        EnterCriticalSection(&g_moveLock);
        fx = (float)g.moveDx; fy = (float)g.moveDy;
        LeaveCriticalSection(&g_moveLock);
    }
    else
    {
        fx = (float)g.moveDx; fy = (float)g.moveDy;
    }

    switch (g.moveDelay)
    {
    case 10: trajMove10(fx, fy); break;
    case 5:  trajMove5 (fx, fy); break;
    case 3:  trajMove3 (fx, fy); break;
    case 1:  makcuMove (fx, fy); break;
    default: trajMove10(fx, fy); break;   // 没选择移动延时默认 10 次
    }

    g.moveDx = 0;
    g.moveDy = 0;
}

// ============================================================================
//  线程_鼠标移动
// ============================================================================
void mouseMoveThread()
{
    optThread();                 // S_优化_Thread
    while (g.running.load())
    {
        if (g.threadMove == "true")
        {
            if (g.aimKeyDown.load())
            {
                if (g.nearestDist.load() != 0) aimMoveOnce();
            }
            g.nearestDist = 0;               // 移动完清除一次
        }
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
        if (g.triggerFire.load() && g.triggerDown.load())
        {
            long long t = nowMs();
            if (t - lastShot > g.shotInterval)
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
//  线程_死循环推理（NCNN = 1 / ONNX = 2 共用）
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
void inferPause(bool paused)
{
    if (paused)
    {
        EnterCriticalSection(&g_drawLock);
        g.canDraw = false;
        std::memset(g.boxes, 0, sizeof(g.boxes));
        LeaveCriticalSection(&g_drawLock);
        g.nearestDist = 0;
        g.moveDx = 0;
        g.moveDy = 0;
    }
    g.inferPaused = paused;
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

    RunTimer runTimer, inferTimer;
    runTimer.start();

    std::vector<unsigned char> localFrame;
    long long localSeq = -1;
    unsigned long long captureSequence = 0;
    CaptureFrame captureFrame;
    std::string loadedModel;
    float accuracy = g.conf;
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
        if (g.engine != engineWanted) { sleepMs(5); continue; }
        if (g.inferPaused.load()) { sleepMs(2); continue; }   // 切换识别范围中：等新画面
        const bool directCapture = g.captureRunning.load();
        if (!directCapture) {
            EnterCriticalSection(&g_frameLock);
            const bool freshFrame = waitForFrameSequence(g_publishedFrameReady,
                g_frameLock, g_frameSeq, localSeq, 50);
            if (freshFrame) { localFrame = g_frame; localSeq = g_frameSeq; }
            LeaveCriticalSection(&g_frameLock);
            if (!freshFrame || g.inferPaused.load() || g.engine != engineWanted) continue;
        }

        // ---------------- 模型加载 ----------------
        const std::string& req = g.modelName;
        if (loadedModel != req)
        {
            if (engineWanted == 1)
            {
                std::string par = g.runDir + "\\" + req + ".param";
                std::string bin = g.runDir + "\\" + req + ".bin";
                if (fileExists(bin) && fileExists(par))
                {
                    // 原版先用模型名"上锁"再加载：加载失败也不会反复重试
                    loadedModel = req;
                    int rc = sagaNcnnCreate(par.c_str(), bin.c_str(), 1, 0);
                    if (rc != 0)
                    {
                        msgBox("模型加载失败，错误码：" + std::to_string(rc));
                        sleepMs(100);
                        continue;
                    }
                }
                else
                {
                    msgBox("模型:" + req + "的param或bin文件不存在!请导入后点击确定");
                    continue;
                }
            }
            else
            {
                std::string onnx = g.runDir + "\\" + req + ".onnx";
                if (fileExists(onnx))
                {
                    // x64 wrapper supports Chinese paths; load beside the EXE directly.
                    std::string txt = g.runDir + "\\空类别.txt";
                    if (!fileExists(txt)) writeFileAll(txt, "");

                    loadedModel = req;                                // 同上，先上锁
                    int rc = sagaOnnxCreate(onnx.c_str(), txt.c_str(), true);
                    if (rc != 0)
                    {
                        msgBox("模型加载失败，错误码：" + std::to_string(rc));
                        sleepMs(100);
                        continue;
                    }
                }
                else
                {
                    msgBox("模型:" + req + "的onnx文件不存在!请导入后点击确定");
                    continue;
                }
            }
        }

        // ---------------- 帧率统计（每 100 次）----------------
        // Timeouts are not inference frames; acquire before counting a sample.
        if (directCapture && !captureAcquire(captureFrame, captureSequence, 50)) continue;
        detectCount++;
        if (detectCount >= 100)
        {
            accuracy = (g.crossClass == 0) ? g.conf : g.crossConf;
            if (accuracy < 0.1f) accuracy = 0.1f;

            double statMs = runTimer.ms();
            if (statMs > 0) g.inferFps = roundToInt(100000.0 / statMs);
            detectCount = 0;
            runTimer.start();
        }

        // ---------------- 推理 ----------------
        inferTimer.start();
        int n = directCapture
            ? (engineWanted == 1
                ? ncnnDetectBgr(captureFrame.bmp + 54, captureFrame.width, captureFrame.height, captureFrame.stride, accuracy, g.nms, objs)
                : onnxDetectBgr(captureFrame.bmp + 54, captureFrame.width, captureFrame.height, captureFrame.stride, accuracy, g.nms, objs))
            : (engineWanted == 1)
            ? sagaNcnnDetect(localFrame.data(), (int)localFrame.size(), accuracy, g.nms, objs)
            : sagaOnnxDetect(localFrame.data(), (int)localFrame.size(), accuracy, g.nms, objs);
        captureFrame = {};
        const double inferCost = inferTimer.ms();      // 亚毫秒精度，界面换算显示用帧率
        g.inferMs = (int)inferCost;
        g.inferMsPrecise = inferCost;
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

        // ---------------- 鼠标中心点（准星）----------------
        int idx = 0;
        int crossCount = 0;
        int centerX = roundToInt(g.imgSize * 0.5);
        int centerY = roundToInt(g.imgSize * 0.5);

        if (g.crossClass > 0 && g.crossTrack.load())
        {
            for (int i = 0; i < n; ++i)
            {
                int cls = objs[i].label + 1;
                if (cls < 1 || cls > 10) continue;
                if (cls == g.crossClass)
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
            if (objs[i].prob < g.conf) continue;
            if (!g.classEnabled[cls - 1]) continue;

            int tx = roundToInt(((double)objs[i].x + objs[i].x + objs[i].width) * 0.5) - centerX;
            int ty = roundToInt(((double)objs[i].y + objs[i].y + objs[i].height) * 0.5) - centerY;

            double dist = std::sqrt((double)tx * tx + (double)ty * ty);

            float rx1 = objs[i].x, ry1 = objs[i].y;
            float rx2 = objs[i].x + objs[i].width, ry2 = objs[i].y + objs[i].height;
            float rh = objs[i].height;

            LockTarget t;
            if (cls == g.prioLock)
            {
                if (g.prioLockPos > 50) ty = roundToInt(ty + rh * std::fabs((double)(g.prioLockPos - 50)) * 0.01);
                if (g.prioLockPos < 50) ty = roundToInt(ty - rh * std::fabs((double)(g.prioLockPos - 50)) * 0.01);
                t.lockType = 1;
                t.normRatio = normRatioRel(centerX, centerY, roundToInt(rx1), roundToInt(ry1), roundToInt(rx2), roundToInt(ry2));
            }
            else if (cls == g.secondLock)
            {
                if (g.secondLockPos > 50) ty = roundToInt(ty + rh * std::fabs((double)(g.secondLockPos - 50)) * 0.01);
                if (g.secondLockPos < 50) ty = roundToInt(ty - rh * std::fabs((double)(g.secondLockPos - 50)) * 0.01);
                t.lockType = 2;
                t.normRatio = normRatioRel(centerX, centerY, roundToInt(rx1), roundToInt(ry1), roundToInt(rx2), roundToInt(ry2));
            }
            else if (cls == g.thirdLock)
            {
                if (g.thirdLockPos > 50) ty = roundToInt(ty + rh * std::fabs((double)(g.thirdLockPos - 50)) * 0.01);
                if (g.thirdLockPos < 50) ty = roundToInt(ty - rh * std::fabs((double)(g.thirdLockPos - 50)) * 0.01);
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
            if (g.moveMode == 2) { pidX = PidCtx(); pidY = PidCtx(); }
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
        lockParam.lockRange = g.lockRange;
        lockParam.prevNearestRange = prevNearestRange;
        lockParam.moveInBox = (g.moveInBox == "true");
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
        else if (picked.held)
        {
            // 失锁确认中：这一帧不移动，避免瞬间跳到旁边那个人身上
            g.moveDx = 0;
            g.moveDy = 0;
        }

        g.boxDx = nearestX;
        g.boxDy = nearestY;
        prevNearestRange = roundToInt(std::sqrt((double)nearestX * nearestX + (double)nearestY * nearestY));

        // ---------------- 扳机 + 移动 ----------------
        // 【修复点】原版只在"选到目标"分支里清 扳机开火，没目标时会残留上一帧
        // 的真值导致持续开火；这里统一先清零。
        g.triggerFire = false;

        if (!keepGoing)
        {
            double crossRatio = normRatioRel(centerX, centerY,
                roundToInt(nearestRect.x), roundToInt(nearestRect.y),
                roundToInt(nearestRect.x + nearestRect.width),
                roundToInt(nearestRect.y + nearestRect.height));

            if (crossCount == 1 && g.crossTrack.load() && g.crossTrigger == "true")
            {
                g.prioRatio = crossRatio;
                if (crossRatio < (double)g.triggerRange && crossRatio > 0) g.triggerFire = true;
            }

            if (g.centerTrigger == "true")
            {
                g.prioRatio = crossRatio;
                if (crossRatio < (double)g.triggerRange && crossRatio > 0) g.triggerFire = true;
            }

            // 中心点范围仅限制优先锁定类别；其它类别继续追向锁定位置 XY。
            if (targets[picked.index].lockType == 1 &&
                crossRatio > 0 && crossRatio <= (double)g.centerRange / 100.0)
                continue;

            // ---- 移动量计算 ----
            if (g.moveMode == 1)
            {
                if (g.nearestDist.load() <= 100)
                {
                    if (nearestX < 0) nearestX = truncToInt(nearestX * g.moveFactor);
                    if (nearestY < 0) nearestY = truncToInt(nearestY * g.moveFactor);
                    if (nearestX > 0) nearestX = truncToInt(nearestX * g.moveFactor + 0.9999);
                    if (nearestY > 0) nearestY = truncToInt(nearestY * g.moveFactor + 0.9999);
                }
                else
                {
                    double scale = (double)g.nearestDist.load() / 100.0;
                    if (nearestX < 0) nearestX = truncToInt(nearestX * g.moveFactor / scale);
                    if (nearestY < 0) nearestY = truncToInt(nearestY * g.moveFactor / scale);
                    if (nearestX > 0) nearestX = truncToInt(nearestX * g.moveFactor / scale + 0.9999);
                    if (nearestY > 0) nearestY = truncToInt(nearestY * g.moveFactor / scale + 0.9999);
                }
            }
            else if (g.moveMode == 2)
            {
                nearestX = roundToInt(pidPosition(pidX, (double)nearestX, g.kp, g.ki));
                nearestY = roundToInt(pidPosition(pidY, (double)nearestY, g.kp, g.ki));
            }

            if (g.threadMove == "true")
            {
                EnterCriticalSection(&g_moveLock);
                g.moveDx = nearestX;
                g.moveDy = nearestY;
                LeaveCriticalSection(&g_moveLock);
            }
            else
            {
                g.moveDx = nearestX;
                g.moveDy = nearestY;
                aimMoveOnce();
            }
        }

        // The next iteration waits for a fresh frame; no fixed per-frame delay.
    }
}
