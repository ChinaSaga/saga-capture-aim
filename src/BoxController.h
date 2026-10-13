#pragma once
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
bool boxButtonState(int key);
// 1/2/3/5/6: existing mouse IDs. 1000 + Linux input code: CAT keyboard.
bool boxKeyState(int key);
