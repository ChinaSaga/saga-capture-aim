#include "App.h"

// ============================================================================
//  线程_死循环热键监控
// ============================================================================
void hotkeyThread()
{
    while (g.running.load())
    {
        // Query a configured button once per scan (Ferrum performs serial I/O).
        bool queried[7] = {}, pressed[7] = {};
        const auto readKey = [&](int key, bool& state) {
            if (key <= 0 || key >= 7) { state = false; return; }
            if (!queried[key]) { makcuState(key, pressed[key]); queried[key] = true; }
            state = pressed[key];
        };
        bool down = false;
        if (g.alwaysOn == "true") down = true;      // 吸附常驻

        if (!down && g.aimMainKey > 0) { readKey(g.aimMainKey, down); }
        if (!down && g.aimSubKey1 > 0) { readKey(g.aimSubKey1, down); }
        if (!down && g.aimSubKey2 > 0) { readKey(g.aimSubKey2, down); }
        if (!down && g.aimSubKey3 > 0) { readKey(g.aimSubKey3, down); }
        g.aimKeyDown = down;

        // 准星追踪（原版为"只置真不自动复位"的锁存行为，由准星监控线程复位）
        bool track = g.crossTrack.load();
        if (!track && g.crossBind[0] > 0) { readKey(g.crossBind[0], track); }
        if (!track && g.crossBind[1] > 0) { readKey(g.crossBind[1], track); }
        if (!track && g.crossBind[2] > 0) { readKey(g.crossBind[2], track); }
        if (!track && g.crossBind[3] > 0) { readKey(g.crossBind[3], track); }
        g.crossTrack = track;

        bool tdown = false;
        if (!tdown && g.triggerKey[0] > 0) { readKey(g.triggerKey[0], tdown); }
        if (!tdown && g.triggerKey[1] > 0) { readKey(g.triggerKey[1], tdown); }
        if (!tdown && g.triggerKey[2] > 0) { readKey(g.triggerKey[2], tdown); }
        if (!tdown && g.triggerKey[3] > 0) { readKey(g.triggerKey[3], tdown); }   // 原版此处无延时
        g.triggerDown = tdown;

        sleepMs(2);
    }
}

// ============================================================================
//  线程_死循环准星监控（开镜计时）
// ============================================================================
void crosshairWatchThread()
{
    bool first = true;
    RunTimer timer;
    while (g.running.load())
    {
        if (g.aimContinuous == "true")
        {
            if (g.aimKeyDown.load())
            {
                if (!first)
                {
                    if (timer.ms() > (double)g.adsTime) g.crossTrack = true;
                }
                if (first)
                {
                    timer.start();
                    first = false;
                }
            }
            else
            {
                first = true;                    // 每次松开自瞄都要重新计算时间
                g.crossTrack = false;
            }
        }
        sleepMs(g.aimContinuous == "true" ? 5 : 50);
    }
}

// ============================================================================
//  线程_死循环阵容监控（中键长按 1.5 秒切换）
// ============================================================================
void campWatchThread()
{
    bool modified = false;
    bool timing = false;
    RunTimer t;
    while (g.running.load())
    {
        if (g.camp.load() != 0)
        {
            bool st = false;
            makcuState(3, st);                   // 鼠标中键
            if (st)
            {
                if (!timing) { timing = true; t.start(); }
                if (timing && t.ms() > 1500.0)
                {
                    if (!modified)
                    {
                        modified = true;
                        int c = g.camp.load();
                        if (c == 1)      g.camp = 2;
                        else if (c == 2) g.camp = 1;
                        else             g.camp = 0;
                    }
                }
            }
            else
            {
                modified = false;
                timing = false;
            }
        }
        sleepMs(20);
    }
}

// ============================================================================
//  线程_锁定类型更新（按阵容刷新 10 个类别开关）
// ============================================================================
void lockTypeUpdateThread()
{
    int lastCamp = 0;
    while (g.running.load())
    {
        int camp = g.camp.load();
        const std::string* src = nullptr;

        if (camp == 0)
        {
            src = g.type;
        }
        else if (camp == 1)
        {
            src = g.team1;
            if (lastCamp != camp) { lastCamp = camp; soundPlayCamp(1); }
        }
        else if (camp == 2)
        {
            src = g.team2;
            if (lastCamp != camp) { lastCamp = camp; soundPlayCamp(2); }
        }

        if (src)
        {
            for (int i = 0; i < 10; ++i)
                g.classEnabled[i] = (src[i] == "true");
        }

        sleepMs(20);
    }
}
