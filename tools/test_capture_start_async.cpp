// Exercise the actual UI launcher with a deliberately stalled fake driver.
#include "../src/CaptureTaskUi.h"
#include <atomic>
#include <cstdio>
using namespace capture_task_ui;
static HWND g_hwnd;
#include <stdexcept>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
namespace runtime_log { void write(const char*, ...) {} }
static bool completed = false, result = false, pong = false;
static LRESULT CALLBACK testWindow(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kCaptureTaskDone) { completed = true; result = w != 0; return 0; }
    if (m == WM_APP + 90) { pong = true; return 0; }
    return DefWindowProcA(h, m, w, l);
}
static bool pumpUntil(const bool& flag, unsigned timeout) {
    const auto deadline = GetTickCount64() + timeout;
    while (!flag && GetTickCount64() < deadline) {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(1);
    }
    return flag;
}
int main() {
    WNDCLASSA wc{}; wc.lpfnWndProc = testWindow;
    wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "CaptureAsyncTest";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return 1;
    for (int id : captureControls) CreateWindowA("BUTTON", "", WS_CHILD, 0, 0, 1, 1, g_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
    HANDLE release = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> attempts{0};
    const auto before = GetTickCount64();
    queueCaptureTask(g_hwnd, [&] { ++attempts; WaitForSingleObject(release, INFINITE); return true; });
    if (GetTickCount64() - before > 500 || !captureTaskBusy) return 2;
    queueCaptureTask(g_hwnd, [&] { attempts += 100; return false; });
    PostMessageA(g_hwnd, WM_APP + 90, 0, 0);
    if (!pumpUntil(pong, 1000) || completed || IsWindowEnabled(GetDlgItem(g_hwnd, 101))) return 3;
    SetEvent(release);
    if (!pumpUntil(completed, 1000) || !result || attempts != 1) return 4;
    CloseHandle(release);
    captureTaskBusy = false; completed = false;
    queueCaptureTask(g_hwnd, []() -> bool { throw std::runtime_error("simulated driver error"); });
    if (!pumpUntil(completed, 1000) || result) return 5;
    DestroyWindow(g_hwnd);
    std::puts("PASS: stalled setup leaves UI responsive, duplicate start blocked, completion and exceptions delivered");
    return 0;
}
