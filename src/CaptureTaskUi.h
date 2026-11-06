#pragma once
#include <windows.h>
#include <functional>
#include <thread>
#include <exception>
#include "RuntimeLog.h"

namespace capture_task_ui {
static constexpr UINT kCaptureTaskDone = WM_APP + 43;
static bool captureTaskBusy = false, captureTaskWarned = false;
static ULONGLONG captureTaskStarted = 0;
static constexpr int captureControls[] = {100, 101, 200, 201, 202};
static BOOL captureControlsEnabled[5]{};

static void queueCaptureTask(HWND target, std::function<bool()> work, bool crop = false)
{
    if (captureTaskBusy) return;
    captureTaskBusy = true;
    captureTaskWarned = false;
    captureTaskStarted = GetTickCount64();
    for (int i = 0; i < 5; ++i) {
        HWND control = GetDlgItem(target, captureControls[i]);
        captureControlsEnabled[i] = IsWindowEnabled(control);
        EnableWindow(control, FALSE);
    }
    SetWindowTextA(GetDlgItem(target, 101), "采集启动中…");

    try {
        std::thread([target, crop, work = std::move(work)] {
            bool ok = false;
            try { ok = work(); }
            catch (const std::exception& e) { runtime_log::write("[capture] 后台初始化异常：%s", e.what()); }
            catch (...) { runtime_log::write("[capture] 后台初始化未知异常"); }
            PostMessageA(target, kCaptureTaskDone, ok, crop);
        }).detach();
    } catch (...) {
        PostMessageA(target, kCaptureTaskDone, FALSE, crop);
    }
}

}
