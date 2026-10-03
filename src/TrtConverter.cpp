#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "TrtBuild.h"
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr UINT kLog = WM_APP + 1, kFinished = WM_APP + 2;
constexpr int kInput = 101, kOutput = 102, kBrowseInput = 103, kBrowseOutput = 104;
constexpr int kSize = 105, kStart = 106, kLogEdit = 107;
constexpr int kTf32 = 108;
struct Result { bool success; std::wstring text; };
struct Ui {
    struct Placement { HWND window; int x, y, width, height; };
    HWND input{}, output{}, size{}, tf32{}, start{}, log{}, progress{}, status{};
    HFONT font{};
    unsigned dpi = 96;
    std::vector<Placement> placements;
    std::thread worker;
    bool busy = false;
};
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
void print(const std::string& text) {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE) {
        DWORD written;
        const std::string line = text + "\r\n";
        WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}
std::wstring text(HWND edit) {
    const int count = GetWindowTextLengthW(edit);
    std::wstring result(count + 1, L'\0');
    GetWindowTextW(edit, result.data(), count + 1);
    result.resize(count);
    return result;
}
bool sizeValue(const std::wstring& value, int& width, int& height) {
    if (value.empty()) { width = height = 0; return true; }
    const auto split = value.find_first_of(L"xX*×");
    if (split == std::wstring::npos) return false;
    try {
        size_t usedWidth = 0, usedHeight = 0;
        width = std::stoi(value.substr(0, split), &usedWidth);
        height = std::stoi(value.substr(split + 1), &usedHeight);
        return usedWidth == split && usedHeight == value.size() - split - 1 &&
            width > 0 && height > 0 && width <= 4096 && height <= 4096;
    } catch (...) { return false; }
}
void append(HWND edit, const std::wstring& message) {
    SendMessageW(edit, EM_SETSEL, GetWindowTextLengthW(edit), GetWindowTextLengthW(edit));
    const std::wstring line = message + L"\r\n";
    SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}
bool pick(HWND window, bool save, std::wstring& selected) {
    std::vector<wchar_t> filename(32768);
    if (!selected.empty()) wcsncpy_s(filename.data(), filename.size(), selected.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
    dialog.lpstrFile = filename.data(); dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrFilter = save ? L"TensorRT 模型 (*.trt)\0*.trt\0所有文件\0*.*\0\0"
                             : L"ONNX 模型 (*.onnx)\0*.onnx\0所有文件\0*.*\0\0";
    dialog.lpstrDefExt = save ? L"trt" : L"onnx";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? 0 : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return false;
    selected = filename.data();
    return true;
}
void enable(HWND window, Ui& ui, bool enabled) {
    for (int id : {kInput, kOutput, kBrowseInput, kBrowseOutput, kSize, kTf32, kStart})
        EnableWindow(GetDlgItem(window, id), enabled);
    SendMessageW(ui.progress, PBM_SETMARQUEE, !enabled, 40);
}
void start(HWND window, Ui& ui) {
    if (ui.busy) return;
    trt::ConversionOptions options;
    options.input = text(ui.input); options.output = text(ui.output);
    options.allowTf32 = SendMessageW(ui.tf32, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (options.input.empty() || options.output.empty()) {
        MessageBoxW(window, L"请先选择 ONNX 模型和保存位置。", L"转换模型", MB_OK | MB_ICONINFORMATION); return;
    }
    if (!sizeValue(text(ui.size), options.dynamicWidth, options.dynamicHeight)) {
        MessageBoxW(window, L"动态尺寸请填写 宽x高，例如 416x416，宽高均不超过 4096；静态模型可以留空。", L"尺寸格式不正确", MB_OK | MB_ICONINFORMATION); return;
    }
    std::error_code error;
    if (std::filesystem::exists(options.output, error)) {
        const std::wstring message = L"保存位置已有文件，是否覆盖？\n\n" + options.output.wstring();
        if (MessageBoxW(window, message.c_str(), L"确认覆盖", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        options.overwrite = true;
    }
    if (ui.worker.joinable()) ui.worker.join();
    ui.busy = true;
    SetWindowTextW(ui.log, L""); SetWindowTextW(ui.status, L"正在转换，首次构建可能需要几分钟……");
    enable(window, ui, false);
    try {
        ui.worker = std::thread([window, options] {
            std::string error;
            bool success = false;
            try {
                success = trt::convertOnnx(options, [window](const std::string& message) {
                    auto line = std::make_unique<std::wstring>(wide(message));
                    if (PostMessageW(window, kLog, 0, reinterpret_cast<LPARAM>(line.get()))) line.release();
                }, error);
            } catch (const std::exception& exception) { error = exception.what(); }
            catch (...) { error = "Unexpected conversion error."; }
            auto result = std::make_unique<Result>();
            result->success = success;
            result->text = success ? L"转换完成：\n" + options.output.wstring() : L"转换失败：\n" + wide(error);
            if (PostMessageW(window, kFinished, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
        });
    } catch (const std::exception& exception) {
        ui.busy = false; enable(window, ui, true);
        const auto message = wide(exception.what());
        SetWindowTextW(ui.status, L"无法启动转换线程");
        MessageBoxW(window, message.c_str(), L"转换失败", MB_OK | MB_ICONERROR);
    }
}
HWND control(HWND parent, Ui& ui, const wchar_t* type, const wchar_t* caption, DWORD style,
    int id, int x, int y, int width, int height) {
    HWND window = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, type, caption,
        WS_CHILD | WS_VISIBLE | style, MulDiv(x, ui.dpi, 96), MulDiv(y, ui.dpi, 96),
        MulDiv(width, ui.dpi, 96), MulDiv(height, ui.dpi, 96), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    ui.placements.push_back({window, x, y, width, height});
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
    return window;
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        ui = static_cast<Ui*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
        ui->dpi = GetDpiForWindow(window);
        ui->font = CreateFontW(-MulDiv(18, ui->dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        control(window, *ui, L"STATIC", L"ONNX 转 TensorRT", 0, 0, 24, 18, 660, 28);
        control(window, *ui, L"STATIC", L"ONNX 模型", 0, 0, 24, 63, 100, 24);
        ui->input = control(window, *ui, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kInput, 126, 58, 474, 32);
        control(window, *ui, L"BUTTON", L"选择模型", WS_TABSTOP, kBrowseInput, 612, 58, 116, 32);
        control(window, *ui, L"STATIC", L"保存为", 0, 0, 24, 108, 100, 24);
        ui->output = control(window, *ui, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kOutput, 126, 103, 474, 32);
        control(window, *ui, L"BUTTON", L"保存位置", WS_TABSTOP, kBrowseOutput, 612, 103, 116, 32);
        control(window, *ui, L"STATIC", L"动态输入尺寸", 0, 0, 24, 153, 124, 24);
        ui->size = control(window, *ui, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kSize, 158, 148, 160, 32);
        SendMessageW(ui->size, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如 416x416"));
        control(window, *ui, L"STATIC", L"静态模型留空；保留模型精度。", 0, 0, 334, 153, 360, 24);
        ui->tf32 = control(window, *ui, L"BUTTON", L"启用 TF32 加速（可能有轻微数值差异）", BS_AUTOCHECKBOX | WS_TABSTOP, kTf32, 24, 190, 704, 28);
        control(window, *ui, L"STATIC", L"转换结果用于这台电脑的 NVIDIA 显卡。原始 ONNX 文件会保留。", 0, 0, 24, 230, 700, 24);
        ui->start = control(window, *ui, L"BUTTON", L"开始转换", BS_DEFPUSHBUTTON | WS_TABSTOP, kStart, 24, 270, 150, 38);
        ui->status = control(window, *ui, L"STATIC", L"选择模型后开始转换。", 0, 0, 192, 278, 520, 24);
        ui->progress = control(window, *ui, PROGRESS_CLASSW, L"", PBS_MARQUEE, 0, 24, 323, 704, 18);
        ui->log = control(window, *ui, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
            kLogEdit, 24, 357, 704, 232);
        SendMessageW(ui->log, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        return 0;
    }
    if (!ui) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_DPICHANGED: {
        ui->dpi = HIWORD(wParam);
        HFONT old = ui->font;
        ui->font = CreateFontW(-MulDiv(18, ui->dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        const auto* bounds = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left,
            bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        for (const auto& item : ui->placements) {
            SetWindowPos(item.window, nullptr, MulDiv(item.x, ui->dpi, 96), MulDiv(item.y, ui->dpi, 96),
                MulDiv(item.width, ui->dpi, 96), MulDiv(item.height, ui->dpi, 96), SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(ui->font), TRUE);
        }
        DeleteObject(old);
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) break;
        switch (LOWORD(wParam)) {
        case kBrowseInput: {
            auto path = text(ui->input);
            if (pick(window, false, path)) {
                SetWindowTextW(ui->input, path.c_str());
                auto output = std::filesystem::path(path); output.replace_extension(L".trt");
                SetWindowTextW(ui->output, output.c_str());
            }
            return 0;
        }
        case kBrowseOutput: {
            auto path = text(ui->output);
            if (pick(window, true, path)) SetWindowTextW(ui->output, path.c_str());
            return 0;
        }
        case kStart: start(window, *ui); return 0;
        }
        break;
    case kLog: {
        std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lParam));
        append(ui->log, *line); return 0;
    }
    case kFinished: {
        std::unique_ptr<Result> result(reinterpret_cast<Result*>(lParam));
        if (ui->worker.joinable()) ui->worker.join();
        ui->busy = false; enable(window, *ui, true);
        SetWindowTextW(ui->status, result->success ? L"转换完成" : L"转换失败，详情见下方记录");
        append(ui->log, result->text);
        MessageBoxW(window, result->text.c_str(), result->success ? L"转换完成" : L"转换失败", MB_OK | (result->success ? MB_ICONINFORMATION : MB_ICONERROR));
        return 0;
    }
    case WM_CLOSE:
        if (ui->busy) {
            MessageBoxW(window, L"正在构建模型，请等待转换结束后关闭窗口。", L"转换进行中", MB_OK | MB_ICONINFORMATION); return 0;
        }
        DestroyWindow(window); return 0;
    case WM_DESTROY:
        if (ui->worker.joinable()) ui->worker.join();
        DeleteObject(ui->font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
int cli(const std::vector<std::wstring>& args) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    SetConsoleOutputCP(CP_UTF8);
    trt::ConversionOptions options;
    for (size_t i = 1; i < args.size(); ++i) {
        const auto& flag = args[i];
        if (flag == L"--overwrite") options.overwrite = true;
        else if (flag == L"--tf32") options.allowTf32 = true;
        else if ((flag == L"--input" || flag == L"--output" || flag == L"--size") && i + 1 < args.size()) {
            const auto& value = args[++i];
            if (flag == L"--input") options.input = value;
            else if (flag == L"--output") options.output = value;
            else if (value.empty() || !sizeValue(value, options.dynamicWidth, options.dynamicHeight)) { print("Invalid --size; expected WIDTHxHEIGHT, each dimension between 1 and 4096."); return 2; }
        } else { print("Usage: TrtConverter --input MODEL.onnx --output MODEL.trt [--size WIDTHxHEIGHT] [--tf32] [--overwrite]"); return 2; }
    }
    if (options.input.empty() || options.output.empty()) { print("Both --input and --output are required."); return 2; }
    std::string error;
    bool success = false;
    try { success = trt::convertOnnx(options, [](const std::string& message) { print(message); }, error); }
    catch (const std::exception& exception) { error = exception.what(); }
    print(success ? "Conversion completed: " + utf8(options.output.wstring()) : "Conversion failed: " + error);
    return success ? 0 : 1;
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    LPWSTR* parsed = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args;
    if (parsed) { for (int i = 0; i < count; ++i) args.emplace_back(parsed[i]); LocalFree(parsed); }
    if (args.size() > 1) return cli(args);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_PROGRESS_CLASS}; InitCommonControlsEx(&common);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = instance;
    type.lpszClassName = L"SagaTrtConverter"; type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&type);
    Ui ui;
    const unsigned dpi = GetDpiForSystem();
    HWND window = CreateWindowExW(0, type.lpszClassName, L"ONNX 转 TensorRT", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(776, dpi, 96), MulDiv(644, dpi, 96), nullptr, nullptr, instance, &ui);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    return static_cast<int>(message.wParam);
}
