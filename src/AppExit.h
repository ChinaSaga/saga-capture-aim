#pragma once
#include <windows.h>

// Explicit user-requested close: end this process and all its worker threads.
// Detached capture/serial workers may hold locks during static/DLL teardown;
// do not wait for those destructors. Save accepted settings before calling.
// Windows reclaims this process's sockets, serial handles and windows.
[[noreturn]] inline void exitApplicationNow()
{
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0); // Defensive fallback if self-termination fails.
}
