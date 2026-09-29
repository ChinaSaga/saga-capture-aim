#pragma once
#include <winsock2.h>
#include <string>

struct WebListeners {
    SOCKET primary = INVALID_SOCKET;
    SOCKET standard = INVALID_SOCKET;
    unsigned short port = 0;
    bool lan = false;
    std::wstring notes;
};

WebListeners startWebListeners(unsigned short preferredPort);
void closeWebListeners(WebListeners& listeners);
// Runs on a background worker after the listeners are ready; no startup dialogs.
void webAccessMonitor(WebListeners listeners);
void webAccessFailed(const std::wstring& reason);
// Cached display address; safe to read from the UI without querying adapters.
std::wstring webAccessAddress();
void webShowDiagnostics(HWND owner);
void webOpenLocalPage(HWND owner);
