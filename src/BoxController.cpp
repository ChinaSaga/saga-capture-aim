#include "App.h"
#include "BoxController.h"
#include "CatSettings.h"
#include "RuntimeLog.h"
#include "Makcu/cat/include/catdevice.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <array>

namespace {
struct CatSession {
    cat::Device device;
    std::atomic<bool> online{true};
};
std::atomic<std::shared_ptr<CatSession>> activeCat;
std::atomic<unsigned long long> routeGeneration{0};
std::mutex routeMutex, settingsMutex;
CatSettings settings;
unsigned settingsVersion = 0;
unsigned injectedButtons = 0; // Protected by routeMutex; belongs to current route.
unsigned pendingCatReleases = 0;
std::atomic<bool> catConfigured{false}, catConnecting{false};
constexpr int keys[] = {1, 2, 3, 5, 6};
constexpr cat::MouseButton buttons[] = {cat::MouseButton::LEFT, cat::MouseButton::RIGHT,
    cat::MouseButton::MIDDLE, cat::MouseButton::SIDE1, cat::MouseButton::SIDE2};
int buttonIndex(int key) {
    for (int i = 0; i < 5; ++i) if (keys[i] == key) return i;
    return -1;
}
std::string settingsPath() { return g.runDir + "\\CAT盒子.ini"; }
void releaseInjected(const std::shared_ptr<CatSession>& old) {
    for (int i = 0; i < 5; ++i) if (injectedButtons & (1u << i)) {
        if (old) {
            if (old->device.mouseUp(buttons[i]) != cat::ErrorCode::SUCCESS)
                pendingCatReleases |= 1u << i;
        }
        else saga::makcuMouseUp(keys[i]);
    }
    injectedButtons = 0;
}
void switchRoute(std::shared_ptr<CatSession> next) {
    std::lock_guard<std::mutex> route(routeMutex);
    auto old = activeCat.load();
    releaseInjected(old);
    if (next) {
        // A lost release ACK must not leave a software-held key across recovery.
        // Releases are idempotent; failed movement/press commands are never replayed.
        for (int i = 0; i < 5; ++i) if (pendingCatReleases & (1u << i)) {
            if (next->device.mouseUp(buttons[i]) != cat::ErrorCode::SUCCESS) {
                next.reset();
                break;
            }
            pendingCatReleases &= ~(1u << i);
        }
    }
    if (old) old->online = false;
    activeCat.store(std::move(next));
    routeGeneration.fetch_add(1);
    if (old || activeCat.load()) runtime_log::write("[hw] 控制设备切换为 %s", activeCat.load() ? "CAT" : "MAKCU（可用时）");
}
void catWorker() {
    unsigned seen = ~0u;
    CatSettings config;
    int lastFailure = -1;
    ULONGLONG retry = 0, probe = 0;
    while (g.running.load()) {
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            if (seen != settingsVersion) {
                config = settings;
                seen = settingsVersion;
                retry = 0;
                lastFailure = -1;
                switchRoute(nullptr);
            }
        }
        auto session = activeCat.load();
        if (session && GetTickCount64() >= probe) {
            std::lock_guard<std::mutex> route(routeMutex);
            if (session->online && (!session->device.isMonitoring() || session->device.mouseMove(0, 0) != cat::ErrorCode::SUCCESS))
                session->online = false;
            probe = GetTickCount64() + 1000;
        }
        if (session && !session->online) {
            switchRoute(nullptr);
            session.reset();
            retry = GetTickCount64() + 3000;
        }
        if (!session && config.enabled && validCatSettings(config) && GetTickCount64() >= retry) {
            catConnecting = true;
            try {
                auto candidate = std::make_shared<CatSession>();
                auto result = candidate->device.connect(config.ip, std::stoi(config.port), config.uuid, 300);
                if (result == cat::ErrorCode::SUCCESS) result = candidate->device.startMonitor(0, 300);
                if (result == cat::ErrorCode::SUCCESS) {
                    lastFailure = -1;
                    // A settings edit during the handshake invalidates this result.
                    std::lock_guard<std::mutex> lock(settingsMutex);
                    if (seen == settingsVersion && g.running.load()) switchRoute(candidate);
                } else if (lastFailure != static_cast<int>(result)) {
                    lastFailure = static_cast<int>(result);
                    runtime_log::write("[hw] CAT 连接/监听失败（错误码 %d），后台继续重试", lastFailure);
                }
            } catch (const std::exception&) { /* Offline: retry without blocking UI. */ }
            catConnecting = false;
            retry = GetTickCount64() + 3000;
            probe = GetTickCount64() + 1000;
        }
        Sleep(250);
    }
    switchRoute(nullptr);
}

struct ConfigDialog { HWND fields[3]{}; CatSettings value; bool accepted = false; };
LRESULT CALLBACK configProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* dialog = reinterpret_cast<ConfigDialog*>(GetWindowLongPtrA(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        dialog = static_cast<ConfigDialog*>(reinterpret_cast<CREATESTRUCTA*>(l)->lpCreateParams);
        SetWindowLongPtrA(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    if (m == WM_COMMAND && dialog) {
        int id = LOWORD(w);
        if (id == IDOK) {
            char values[3][128]{};
            for (int i = 0; i < 3; ++i) GetWindowTextA(dialog->fields[i], values[i], 128);
            CatSettings next{values[0], values[1], values[2], true};
            if (!validCatSettings(next)) {
                MessageBoxA(h, "请填写有效的 IPv4 地址、1～65535 的端口和 8 位十六进制 UUID。", "CAT 参数有误", MB_OK | MB_ICONWARNING);
                return 0;
            }
            dialog->value = std::move(next);
            dialog->accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (id == 3) {
            dialog->value.enabled = false;
            dialog->accepted = true;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) { DestroyWindow(h); return 0; }
    }
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    return DefWindowProcA(h, m, w, l);
}
}

void boxStart() {
    char ip[128]{}, port[128]{}, uuid[128]{};
    const auto path = settingsPath();
    GetPrivateProfileStringA("CAT", "IP", "", ip, 128, path.c_str());
    GetPrivateProfileStringA("CAT", "Port", "", port, 128, path.c_str());
    GetPrivateProfileStringA("CAT", "UUID", "", uuid, 128, path.c_str());
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        settings = {ip, port, uuid, GetPrivateProfileIntA("CAT", "Enabled", 0, path.c_str()) != 0};
        settings.enabled = settings.enabled && validCatSettings(settings);
        catConfigured = settings.enabled;
    }
    std::thread(catWorker).detach();
}

void boxConfigure(HWND owner) {
    ConfigDialog dialog;
    { std::lock_guard<std::mutex> lock(settingsMutex); dialog.value = settings; }
    WNDCLASSA wc{};
    wc.lpfnWndProc = configProc; wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "SagaCatConfig"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    HWND h = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName, "CAT 网络盒子",
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 460, 285,
        owner, nullptr, wc.hInstance, &dialog);
    if (!h) return;
    auto control = [&](const char* cls, const char* text, DWORD style, int x, int y, int width, int id) {
        HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, width, 28, h,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
        SendMessageA(c, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return c;
    };
    const char* labels[] = {"IP 地址", "端口 Port", "UUID"};
    const std::string values[] = {dialog.value.ip, dialog.value.port, dialog.value.uuid};
    for (int i = 0; i < 3; ++i) {
        control("STATIC", labels[i], 0, 20, 20 + i * 42, 95, 0);
        dialog.fields[i] = control("EDIT", values[i].c_str(), WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 120, 18 + i * 42, 290, 10 + i);
        SendMessageA(dialog.fields[i], EM_SETLIMITTEXT, i == 0 ? 15 : i == 1 ? 5 : 8, 0);
    }
    control("STATIC", "CAT 可用时优先；离线时自动使用 MAKCU。", 0, 20, 151, 410, 0);
    control("BUTTON", "保存并连接", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, 195, 125, IDOK);
    control("BUTTON", "停用 CAT", WS_TABSTOP, 158, 195, 125, 3);
    control("BUTTON", "取消", WS_TABSTOP, 295, 195, 115, IDCANCEL);
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetFocus(dialog.fields[0]);
    MSG msg{};
    while (IsWindow(h)) {
        const int result = GetMessageA(&msg, nullptr, 0, 0);
        if (result <= 0) { if (!result) PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        if (!IsDialogMessageA(h, &msg)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    }
    if (IsWindow(h)) DestroyWindow(h);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (!dialog.accepted) return;
    const auto path = settingsPath();
    bool saved = WritePrivateProfileStringA("CAT", "IP", dialog.value.ip.c_str(), path.c_str()) != 0;
    saved = (WritePrivateProfileStringA("CAT", "Port", dialog.value.port.c_str(), path.c_str()) != 0) && saved;
    saved = (WritePrivateProfileStringA("CAT", "UUID", dialog.value.uuid.c_str(), path.c_str()) != 0) && saved;
    saved = (WritePrivateProfileStringA("CAT", "Enabled", dialog.value.enabled ? "1" : "0", path.c_str()) != 0) && saved;
    if (!saved) MessageBoxA(owner, "参数将在本次运行生效，但无法保存到程序目录。", "CAT 配置", MB_OK | MB_ICONWARNING);
    { std::lock_guard<std::mutex> lock(settingsMutex); settings = dialog.value; ++settingsVersion; catConfigured = settings.enabled; }
}

const char* boxStatus() {
    auto session = activeCat.load();
    if (session && session->online) return "CAT 已连接";
    if (saga::makcuIsConnected()) return catConfigured ? "MAKCU 已连接 · CAT 重试中" : "MAKCU 已连接";
    if (catConfigured) return catConnecting ? "CAT 连接中 · MAKCU 未连接" : "CAT 离线，自动重试 · MAKCU 未连接";
    return makcuStartupStatus();
}

void boxMove(int x, int y) {
    const auto generation = routeGeneration.load();
    std::lock_guard<std::mutex> route(routeMutex);
    if (generation != routeGeneration.load()) return;
    auto session = activeCat.load();
    if (!session) { saga::makcuMove(x, y); return; }
    if (!session->online || x < -32768 || x > 32767 || y < -32768 || y > 32767) return;
    if (session->device.mouseMove(static_cast<int16_t>(x), static_cast<int16_t>(y)) != cat::ErrorCode::SUCCESS) session->online = false;
}
void boxDown(int key) {
    const int index = buttonIndex(key);
    if (index < 0) return;
    const auto generation = routeGeneration.load();
    std::lock_guard<std::mutex> route(routeMutex);
    if (generation != routeGeneration.load()) return;
    auto session = activeCat.load();
    if (session && !session->online) return;
    // Remember even a timed-out press: its ACK may be lost after execution.
    injectedButtons |= 1u << index;
    if (session) {
        if (session->device.mouseDown(buttons[index]) != cat::ErrorCode::SUCCESS) session->online = false;
    } else saga::makcuMouseDown(key);
}
void boxUp(int key) {
    const int index = buttonIndex(key);
    if (index < 0) return;
    const auto generation = routeGeneration.load();
    std::lock_guard<std::mutex> route(routeMutex);
    if (generation != routeGeneration.load()) return;
    auto session = activeCat.load();
    if (session) {
        if (!session->online) return;
        if (session->device.mouseUp(buttons[index]) != cat::ErrorCode::SUCCESS) { session->online = false; return; }
    } else saga::makcuMouseUp(key);
    injectedButtons &= ~(1u << index);
}
bool boxButtonState(int key) {
    const int index = buttonIndex(key);
    if (index < 0) return false;
    auto session = activeCat.load();
    if (session) return session->online && session->device.isMouseButtonPressed(buttons[index]);
    return saga::makcuMouseButtonState(key) != 0;
}

bool boxKeyState(int key) {
    if (key > 1000 && key <= 1127) {
        auto session = activeCat.load();
        return session && session->online && session->device.isKeyPressed(static_cast<uint16_t>(key - 1000));
    }
    return boxButtonState(key);
}
