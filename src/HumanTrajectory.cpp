#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "HumanTrajectory.h"
#include "HumanTrajectoryModel.h"
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {
namespace fs = std::filesystem;
namespace ht = humanTrajectory;
constexpr wchar_t dataFileName[] = L"人手数据.txt";

void migrateLegacyData(const fs::path& directory)
{
    const auto current = directory / dataFileName;
    const auto legacy = directory / L"mouse_data.csv";
    // Called under DataLock. Never overwrite an existing text recording.
    if (!fs::exists(current) && fs::exists(legacy) && !MoveFileW(legacy.c_str(), current.c_str()))
        throw std::runtime_error("无法将旧数据改名为 人手数据.txt，请检查文件是否被占用或目录权限。");
}

fs::path executablePath()
{
    wchar_t buffer[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, buffer, _countof(buffer));
    if (!length || length >= _countof(buffer)) throw std::runtime_error("无法定位程序目录。");
    return buffer;
}

std::wstring wide(const std::string& text)
{
    int size = MultiByteToWideChar(936, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size) MultiByteToWideChar(936, 0, text.data(), (int)text.size(), result.data(), size);
    return result;
}

void consoleLine(const std::wstring& text)
{
    const std::wstring line = text + L"\r\n";
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, written;
    if (GetConsoleMode(output, &mode)) {
        WriteConsoleW(output, line.data(), (DWORD)line.size(), &written, nullptr);
    } else {
        int size = WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.data(), (int)line.size(), utf8.data(), size, nullptr, nullptr);
        WriteFile(output, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
}

void prepareConsole()
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) AllocConsole();
    SetConsoleTitleW(L"人手轨迹 CPU 训练 / 原生 C++");
}

void pauseConsole()
{
    DWORD mode;
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(input, &mode)) return;
    consoleLine(L"\n按 Enter 关闭此窗口。");
    wchar_t buffer[16];
    DWORD read;
    ReadConsoleW(input, buffer, _countof(buffer), &read, nullptr);
}

struct DataLock {
    HANDLE handle;
    explicit DataLock(const fs::path& directory) {
        fs::create_directories(directory);
        handle = CreateFileW((directory / L".trajectory.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("人手数据正在被另一个记录或训练窗口使用，或目录不可写。请关闭该窗口后重试。");
    }
    ~DataLock() { CloseHandle(handle); }
    DataLock(const DataLock&) = delete;
    DataLock& operator=(const DataLock&) = delete;
};

std::atomic<bool> trainingCancelled{false};
BOOL WINAPI trainingControl(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        trainingCancelled.store(true);
        return TRUE;
    }
    return FALSE;
}

int runTraining(const fs::path& directory, int epochs)
{
    trainingCancelled = false;
    SetConsoleCtrlHandler(trainingControl, TRUE);
    struct HandlerCleanup { ~HandlerCleanup() { SetConsoleCtrlHandler(trainingControl, FALSE); } } cleanup;
    try {
        consoleLine(L"原生 C++ / CPU 训练（2 → 64 → 32 → 20）");
        consoleLine(L"数据目录：" + directory.wstring());
        consoleLine(L"按 Ctrl+C 可中止；关闭窗口也会结束训练。成功后主程序会自动加载新模型。\n");
        const auto samples = ht::loadSamples(directory / dataFileName);
        consoleLine(L"样本数：" + std::to_wstring(samples.size()) + L"，训练轮数：" + std::to_wstring(epochs));
        ht::TrainOptions options;
        options.epochs = epochs;
        options.seed = std::random_device{}();
        const auto started = std::chrono::steady_clock::now();
        auto network = ht::train(samples, options, [&](const ht::Progress& progress) {
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            wchar_t line[192];
            swprintf_s(line, L"Epoch %d/%d   loss=%.6f   lr=%.6g   用时 %.1f 秒",
                progress.epoch, progress.epochs, progress.loss, progress.learningRate, seconds);
            consoleLine(line);
        }, [] { return trainingCancelled.load(); });
        const double loss = ht::meanLoss(network, samples);
        if (!std::isfinite(loss)) throw std::runtime_error("模型复测出现无效数值，未替换已有模型。");
        if (trainingCancelled.load()) throw ht::TrainingCancelled();
        ht::saveModel(network, directory / L"mouse.bin");
        wchar_t line[192];
        swprintf_s(line, L"\n训练集复测 loss=%.6f（使用同一份采集数据）", loss);
        consoleLine(line);
        consoleLine(L"训练完成！模型已保存：" + (directory / L"mouse.bin").wstring());
        consoleLine(L"运行中的主程序会自动加载新模型，无需重启。");
        return 0;
    } catch (const ht::TrainingCancelled&) {
        consoleLine(L"\n训练已中止，已有模型保持不变。");
        return 130;
    }
}

struct Collector {
    fs::path csv;
    HWND window = nullptr;
    HFONT font = nullptr;
    int width = 0, height = 0;
    size_t count = 0;
    bool recording = false;
    ht::Point red{}, blue{};
    std::vector<ht::Point> path;
    std::wstring status;
    std::mt19937 random{std::random_device{}()};
    static constexpr int radius = 40;

    explicit Collector(const fs::path& directory) : csv(directory / dataFileName) {
        std::ofstream check(csv, std::ios::app);
        if (!check) throw std::runtime_error("无法写入 人手数据.txt，请检查文件权限。");
        check.close();

        // Each completed recording saves two rows: original and horizontal mirror.
        // Recover from the data itself so existing recordings need no counter file.
        std::ifstream saved(csv, std::ios::binary);
        if (!saved) throw std::runtime_error("无法读取 人手数据.txt 中的累计记录数。");
        std::string line;
        size_t rows = 0;
        bool firstLine = true;
        while (std::getline(saved, line)) {
            if (firstLine && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
            firstLine = false;
            if (line.find_first_not_of(" \t\r\n") != std::string::npos) ++rows;
        }
        if (!saved.eof()) throw std::runtime_error("读取累计记录数时发生错误，请检查人手数据文件。");
        count = rows / 2;
    }
    ~Collector() { if (font) DeleteObject(font); }

    void spawnBlue() {
        const int minX = radius + 20, maxX = std::max(minX, width - radius - 20);
        const int minY = 180, maxY = std::max(minY, height - radius - 20);
        std::uniform_int_distribution<int> x(minX, maxX), y(minY, maxY);
        for (int attempt = 0; attempt < 4096; ++attempt) {
            blue = {x(random), y(random)};
            const double distance = std::hypot(double(blue.x - red.x), double(blue.y - red.y));
            if (distance >= 150 && distance <= 400) return;
        }
        // Bounded fallback for unusually small displays.
        blue = {red.x > width / 2 ? minX : maxX, std::clamp(red.y, minY, maxY)};
    }
    void resize(int w, int h) {
        width = w; height = h;
        red = {w / 2, h / 2 + 30};
        path.clear(); recording = false;
        spawnBlue();
    }
    bool hit(ht::Point mouse, ht::Point ball) const {
        return std::hypot(double(mouse.x - ball.x), double(mouse.y - ball.y)) <= radius + 10;
    }
    void click(ht::Point mouse) {
        if (hit(mouse, red)) {
            recording = true;
            path = {mouse};
            status = L"正在记录…";
        } else if (recording && hit(mouse, blue)) {
            recording = false;
            try {
                if (ht::appendRecording(csv, path)) {
                    ++count;
                    status = L"已保存，点击红球继续记录";
                } else status = L"轨迹太短，请重新点击红球后移动到蓝球";
            } catch (const std::exception& error) {
                status = L"保存失败，请检查文件后重试";
                MessageBoxW(window, wide(error.what()).c_str(), L"人手数据记录", MB_OK | MB_ICONERROR);
            }
            path.clear();
            spawnBlue();
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void paint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1, width), std::max(1, height));
        auto oldBitmap = SelectObject(buffer, bitmap);
        RECT all{0, 0, width, height};
        FillRect(buffer, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
        auto oldFont = SelectObject(buffer, font);
        SetBkMode(buffer, TRANSPARENT);
        auto text = [&](const std::wstring& value, RECT rect, COLORREF color) {
            SetTextColor(buffer, color);
            DrawTextW(buffer, value.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };
        text(L"累计记录：" + std::to_wstring(count) + L" 条", {20, 15, width - 20, 55}, RGB(255,255,255));
        text(L"点击红球开始 → 移动到蓝球，再点击结束", {20, 62, width - 20, 105}, RGB(255,255,255));
        text(L"按 Esc 或 Q 退出，完成的记录自动保存", {20, 110, width - 20, 150}, RGB(160,160,160));
        text(status, {20, height - 70, width - 20, height - 20}, recording ? RGB(80,255,100) : RGB(210,210,210));
        auto ball = [&](ht::Point point, COLORREF color, COLORREF outline) {
            HBRUSH brush = CreateSolidBrush(color);
            HPEN pen = CreatePen(PS_SOLID, 3, outline);
            auto oldBrush = SelectObject(buffer, brush);
            auto oldPen = SelectObject(buffer, pen);
            Ellipse(buffer, point.x - radius, point.y - radius, point.x + radius, point.y + radius);
            SelectObject(buffer, oldBrush); SelectObject(buffer, oldPen);
            DeleteObject(brush); DeleteObject(pen);
        };
        ball(red, RGB(255,0,0), RGB(255,255,255));
        ball(blue, RGB(0,0,255), RGB(0,255,255));
        SelectObject(buffer, oldFont);
        BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap); DeleteDC(buffer);
        EndPaint(window, &ps);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Collector*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Collector*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        switch (message) {
        case WM_CREATE:
            self->font = CreateFontW(-26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            return 0;
        case WM_SIZE: self->resize(LOWORD(l), HIWORD(l)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: self->paint(); return 0;
        case WM_MOUSEMOVE:
            if (self->recording) self->path.push_back({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            return 0;
        case WM_LBUTTONDOWN: self->click({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE || w == 'Q') DestroyWindow(window);
            return 0;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window, message, w, l);
    }
    int run(HINSTANCE instance) {
        WNDCLASSW type{};
        type.hInstance = instance;
        type.lpfnWndProc = procedure;
        type.lpszClassName = L"SagaHumanTrajectoryCollector";
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("无法创建记录窗口。");
        POINT cursor{}; GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const RECT bounds = monitor.rcMonitor;
        window = CreateWindowExW(0, type.lpszClassName, L"人手数据记录 / 原生 C++", WS_POPUP,
            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, this);
        if (!window) throw std::runtime_error("无法创建记录窗口。");
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
        UpdateWindow(window);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }
};

struct ToolProcess {
    HANDLE handle = nullptr;
    ~ToolProcess() { if (handle) CloseHandle(handle); }
    bool running() {
        if (!handle) return false;
        if (WaitForSingleObject(handle, 0) != WAIT_OBJECT_0) return true;
        CloseHandle(handle); handle = nullptr;
        return false;
    }
} toolProcess;
} // namespace

bool runHumanTrajectoryCommandLine(HINSTANCE instance, int& result)
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    struct ArgumentsCleanup { wchar_t** value; ~ArgumentsCleanup() { LocalFree(value); } } cleanup{arguments};
    if (count < 2) return false;
    const bool training = wcscmp(arguments[1], L"--train-human-data") == 0;
    if (!training && wcscmp(arguments[1], L"--collect-human-data") != 0) return false;
    bool pause = training;
    if (training) prepareConsole();
    try {
        int epochs = 500;
        fs::path directory = executablePath().parent_path() / L"人手数据";
        for (int i = 2; i < count; ++i) {
            if (wcscmp(arguments[i], L"--no-pause") == 0) pause = false;
            else if (wcscmp(arguments[i], L"--data-dir") == 0 && i + 1 < count) directory = fs::absolute(arguments[++i]);
            else if (wcscmp(arguments[i], L"--epochs") == 0 && i + 1 < count && training) {
                wchar_t* end;
                long value = wcstol(arguments[++i], &end, 10);
                if (*end || value < 1 || value > 1000000) throw std::runtime_error("训练轮数必须为 1 到 1000000 的整数。");
                epochs = (int)value;
            } else throw std::runtime_error("无法识别的人手轨迹命令行参数。");
        }
        DataLock lock(directory);
        migrateLegacyData(directory);
        result = training ? runTraining(directory, epochs) : Collector(directory).run(instance);
        if (pause) pauseConsole();
    } catch (const std::exception& error) {
        result = 1;
        if (training) {
            consoleLine(L"失败：" + wide(error.what()));
            if (pause) pauseConsole();
        } else MessageBoxW(nullptr, wide(error.what()).c_str(), L"人手数据记录", MB_OK | MB_ICONERROR);
    }
    return true;
}

void startHumanTrajectoryTool(HWND owner, bool training)
{
    if (toolProcess.running()) {
        MessageBoxW(owner, L"记录或训练窗口已经打开，请先关闭该窗口再开始下一项。", L"人手轨迹", MB_OK | MB_ICONINFORMATION);
        return;
    }
    try {
        const fs::path executable = executablePath();
        const fs::path directory = executable.parent_path() / L"人手数据";
        fs::create_directories(directory);
        auto dataFile = directory / dataFileName;
        if (!fs::exists(dataFile)) dataFile = directory / L"mouse_data.csv";
        if (training && (!fs::exists(dataFile) || fs::file_size(dataFile) == 0)) {
            MessageBoxW(owner, L"还没有人手数据，请先点击“点我开始记录人手数据”，完成红球到蓝球的记录后再训练。",
                L"人手轨迹", MB_OK | MB_ICONINFORMATION);
            return;
        }
        // No command shell, Python interpreter, extracted script or extra runtime.
        std::wstring command = L"\"" + executable.wstring() + L"\" " +
            (training ? L"--train-human-data" : L"--collect-human-data");
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            training ? CREATE_NEW_CONSOLE : 0, nullptr, directory.c_str(), &startup, &process)) {
            const std::wstring message = L"无法打开人手轨迹窗口，Windows 错误码：" + std::to_wstring(GetLastError());
            MessageBoxW(owner, message.c_str(), L"人手轨迹", MB_OK | MB_ICONERROR);
            return;
        }
        CloseHandle(process.hThread);
        toolProcess.handle = process.hProcess;
    } catch (const std::exception& error) {
        MessageBoxW(owner, wide(error.what()).c_str(), L"人手轨迹", MB_OK | MB_ICONERROR);
    }
}
