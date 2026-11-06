#pragma once
#include <array>
#include <string>
#include <windows.h>

void boxStart();
void boxStopKeyboard();
void boxConfigure(HWND owner);
const char* boxStatus();
void boxMove(int x, int y);
// Whether this thread's last dispatched movement used MAKCU and needs its pacing.
bool boxLastMoveNeedsDelay();
void boxDown(int key);
void boxUp(int key);
// Shared across hotkeys, the web status and camp monitoring. Each mouse button
// is sampled at most once per 2 ms; route changes invalidate the cached sample.
bool boxButtonState(int key);
// 1/2/3/5/6: existing mouse IDs. 1000 + Linux input code: CAT keyboard.
bool boxKeyState(int key);

// One scan owns one result per valid key, including released keys. Keep this
// local to the input thread so aim/crosshair/trigger use the exact same value.
class BoxInputScan {
public:
    BoxInputScan();
    bool keyDown(int key);
private:
    unsigned long long generation_;
    std::array<signed char, 132> states_{}; // 5 mouse + 127 keyboard keys.
};
