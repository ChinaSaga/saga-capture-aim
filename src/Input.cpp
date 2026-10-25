#include "App.h"
#include "BoxController.h"

// ============================================================================
//  线程_死循环热键监控
// ============================================================================
void hotkeyThread()
{
    while (g.running.load())
    {
        const auto settings = g.settings.snapshot();
        BoxInputScan input;
        const auto readKey = [&](int key, bool& state) {
            state = input.keyDown(key);
        };
        const auto config = g.aimProfiles.config();
        const int active = aim_profiles::select(config.config, [&](int key) {
            bool pressed = false;
            readKey(key, pressed);
            return pressed;
        });
        g.aimKeyDown = g.aimProfiles.activate(active, config.revision);
        // Repeat only while a configured aim key is physically held, even in always-on mode.
        auto physicalConfig = config.config;
        physicalConfig.alwaysOn = false;
        g.aimPhysicalDown = aim_profiles::select(physicalConfig, [&](int key) {
                bool pressed = false;
                readKey(key, pressed);
                return pressed;
            }) >= 0;
        g.rangePressActive = g.rangePressKey.load() > 1000 && g.aimPhysicalDown.load();

        // 准星追踪（原版为"只置真不自动复位"的锁存行为，由准星监控线程复位）
        bool track = g.crossTrack.load();
        if (!track && settings.crossBind[0] > 0) { readKey(settings.crossBind[0], track); }
        if (!track && settings.crossBind[1] > 0) { readKey(settings.crossBind[1], track); }
        if (!track && settings.crossBind[2] > 0) { readKey(settings.crossBind[2], track); }
        if (!track && settings.crossBind[3] > 0) { readKey(settings.crossBind[3], track); }
        g.crossTrack = track;

        bool tdown = false;
        if (!tdown && settings.triggerKey[0] > 0) { readKey(settings.triggerKey[0], tdown); }
        if (!tdown && settings.triggerKey[1] > 0) { readKey(settings.triggerKey[1], tdown); }
        if (!tdown && settings.triggerKey[2] > 0) { readKey(settings.triggerKey[2], tdown); }
        if (!tdown && settings.triggerKey[3] > 0) { readKey(settings.triggerKey[3], tdown); }   // 原版此处无延时
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
        const auto settings = g.settings.snapshot();
        if (settings.aimContinuous)
        {
            if (g.aimKeyDown.load())
            {
                if (!first)
                {
                    if (timer.ms() > (double)settings.adsTime) g.crossTrack = true;
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
        sleepMs(settings.aimContinuous ? 5 : 50);
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
        const auto settings = g.settings.snapshot();
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
        const auto settings = g.settings.snapshot();
        int camp = g.camp.load();
        const bool* src = nullptr;

        if (camp == 0)
        {
            src = settings.type;
        }
        else if (camp == 1)
        {
            src = settings.team1;
            if (lastCamp != camp) { lastCamp = camp; soundPlayCamp(1); }
        }
        else if (camp == 2)
        {
            src = settings.team2;
            if (lastCamp != camp) { lastCamp = camp; soundPlayCamp(2); }
        }

        if (src)
        {
            for (int i = 0; i < 10; ++i)
                g.classEnabled[i] = src[i];
        }

        sleepMs(20);
    }
}
