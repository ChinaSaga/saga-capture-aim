#pragma once

#include <windows.h>

// Dispatch before capture, driver, network and model initialization.
bool runHumanTrajectoryCommandLine(HINSTANCE instance, int& result);

// Starts this same EXE in native collector or CPU trainer mode.
void startHumanTrajectoryTool(HWND owner, bool training);
