#pragma once
#include <windows.h>

// Caller holds lock. Sleep atomically releases it; sequence is the predicate,
// so an arrival before the wait and spurious wakes cannot lose a notification.
inline bool waitForFrameSequence(CONDITION_VARIABLE& ready, CRITICAL_SECTION& lock,
    const long long& sequence, long long previous, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (sequence == previous) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        if (!SleepConditionVariableCS(&ready, &lock, static_cast<DWORD>(deadline - now))
            && GetLastError() == ERROR_TIMEOUT) return sequence != previous;
    }
    return true;
}
