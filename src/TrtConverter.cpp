#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "TrtBuild.h"
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
constexpr UINT kLog = WM_APP + 1, kFinished = WM_APP + 2;
constexpr int kInput = 101, kBrowseInput = 103;
constexpr int kSize = 105, kStart = 106, kLogEdit = 107;
constexpr int kReplace = 1001, kSkipExisting = 1002;

struct Result { bool success; std::wstring text; };
struct Ui {
    struct Placement { HWND window; int x, y, width, height; };
    HWND input{}, size{}, start{}, log{}, progress{}, status{};
    std::vector<std::filesystem::path> inputs;
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
std::filesystem::path executableDirectory() {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    return length && length < 32768 ? std::filesystem::path(executable).parent_path() : std::filesystem::path{};
}
std::filesystem::path modelDirectory() {
    const auto directory = executableDirectory();
    return directory.empty() ? directory : directory / L"模型数据";
}
void showInputs(Ui& ui) {
    std::wstring list;
    for (const auto& path : ui.inputs) {
        if (!list.empty()) list += L"\r\n";
        list += path.wstring();
    }
    SetWindowTextW(ui.input, list.c_str());
    const auto status = L"已选择 " + std::to_wstring(ui.inputs.size()) + L" 个模型，点击开始转换。";
    SetWindowTextW(ui.status, status.c_str());
}
bool pick(HWND window, std::vector<std::filesystem::path>& selected) {
    std::vector<wchar_t> filename(1024 * 1024);
    const auto directory = selected.empty() ? modelDirectory() : selected.front().parent_path();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
    dialog.lpstrFile = filename.data(); dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrFilter = L"ONNX 模型 (*.onnx)\0*.onnx\0\0";
    dialog.lpstrInitialDir = directory.c_str();
    dialog.lpstrDefExt = L"onnx";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError()) MessageBoxW(window, L"模型选择失败，请减少一次选择的文件数量后重试。", L"选择模型", MB_OK | MB_ICONERROR);
        return false;
    }
    selected.clear();
    const std::filesystem::path first(filename.data());
    const wchar_t* next = filename.data() + wcslen(filename.data()) + 1;
    if (!*next) selected.push_back(first);
    else for (; *next; next += wcslen(next) + 1) selected.push_back(first / next);
    return true;
}
void enable(HWND window, Ui& ui, bool enabled) {
    for (int id : {kInput, kBrowseInput, kSize, kStart})
        EnableWindow(GetDlgItem(window, id), enabled);
    SendMessageW(ui.progress, PBM_SETMARQUEE, !enabled, 40);
}
int conversionChoice(HWND window, size_t total, size_t existing) {
    const std::wstring message = L"已选择 " + std::to_wstring(total) + L" 个 ONNX 模型，其中 " +
        std::to_wstring(existing) + L" 个已有同名 TRT。\n\n"
        L"代替并转换：转换全部模型，覆盖已有 TRT。\n"
        L"不代替转换：保留已有 TRT，只转换缺少 TRT 的模型。\n"
        L"不转换：取消本次操作。";
    const TASKDIALOG_BUTTON buttons[] = {
        {kReplace, L"代替并转换"}, {kSkipExisting, L"不代替转换"}, {IDCANCEL, L"不转换"}
    };
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = window;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"选择转换方式";
    dialog.pszMainInstruction = L"请选择本批模型的转换方式";
    dialog.pszContent = message.c_str();
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = kSkipExisting;
    int choice = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr))) {
        MessageBoxW(window, L"无法打开转换确认窗口，本次转换已取消。", L"转换模型", MB_OK | MB_ICONERROR);
        return IDCANCEL;
    }
    return choice;
}
void start(HWND window, Ui& ui) {
    if (ui.busy) return;
    trt::ConversionOptions options;
    if (ui.inputs.empty()) {
        MessageBoxW(window, L"请先选择一个或多个 ONNX 模型。", L"转换模型", MB_OK | MB_ICONINFORMATION); return;
    }
    if (!sizeValue(text(ui.size), options.dynamicWidth, options.dynamicHeight)) {
        MessageBoxW(window, L"动态尺寸请填写 宽x高，例如 416x416，宽高均不超过 4096；静态模型可以留空。", L"尺寸格式不正确", MB_OK | MB_ICONINFORMATION); return;
    }
    std::vector<trt::ConversionOptions> jobs;
    size_t existing = 0;
    for (const auto& input : ui.inputs) {
        options.input = input;
        options.output = input;
        options.output.replace_extension(L".trt");
        std::error_code error;
        if (std::filesystem::exists(options.output, error)) ++existing;
        jobs.push_back(options);
    }
    const int choice = conversionChoice(window, jobs.size(), existing);
    if (choice != kReplace && choice != kSkipExisting) return;
    for (auto& job : jobs) job.overwrite = choice == kReplace;
    if (ui.worker.joinable()) ui.worker.join();
    ui.busy = true;
    SetWindowTextW(ui.log, L""); SetWindowTextW(ui.status, L"正在转换，首次构建可能需要几分钟……");
    enable(window, ui, false);
    try {
        ui.worker = std::thread([window, jobs] {
            auto log = [window](const std::string& message) {
                auto line = std::make_unique<std::wstring>(wide(message));
                if (PostMessageW(window, kLog, 0, reinterpret_cast<LPARAM>(line.get()))) line.release();
            };
            size_t completed = 0, skipped = 0;
            for (size_t index = 0; index < jobs.size(); ++index) {
                const auto& options = jobs[index];
                log("[" + std::to_string(index + 1) + "/" + std::to_string(jobs.size()) + "] " + utf8(options.input.wstring()));
                std::string error;
                bool success = false;
                try {
                    // Recheck each job so files created while this batch runs are also preserved.
                    if (!options.overwrite && std::filesystem::exists(options.output)) {
                        ++skipped;
                        log("已跳过，保留已有 TRT：" + utf8(options.output.wstring()));
                        continue;
                    }
                    success = trt::convertOnnx(options, log, error);
                } catch (const std::exception& exception) { error = exception.what(); }
                catch (...) { error = "Unexpected conversion error."; }
                if (success) ++completed;
                log(success ? "转换完成：" + utf8(options.output.wstring()) : "转换失败：" + utf8(options.input.wstring()) + "\r\n" + error);
            }
            auto result = std::make_unique<Result>();
            const size_t failed = jobs.size() - completed - skipped;
            result->success = failed == 0;
            result->text = L"批量转换结束：成功 " + std::to_wstring(completed) + L" 个，跳过 " +
                std::to_wstring(skipped) + L" 个，失败 " + std::to_wstring(failed) +
                L" 个。\nTRT 保存在对应 ONNX 同目录，跳过的已有 TRT 保持不变，详情见转换记录。";
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
        ui->input = control(window, *ui, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP, kInput, 126, 58, 474, 77);
        SendMessageW(ui->input, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        control(window, *ui, L"BUTTON", L"选择模型", WS_TABSTOP, kBrowseInput, 612, 58, 116, 32);
        control(window, *ui, L"STATIC", L"动态输入尺寸", 0, 0, 24, 153, 124, 24);
        ui->size = control(window, *ui, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kSize, 158, 148, 160, 32);
        SendMessageW(ui->size, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如 416x416"));
        control(window, *ui, L"STATIC", L"静态模型留空；默认 FP16。", 0, 0, 334, 153, 360, 24);
        control(window, *ui, L"STATIC", L"推理精度：FP16。自动转换 ONNX，生成半精度 TRT 模型。", 0, 0, 24, 190, 704, 28);
        control(window, *ui, L"STATIC", L"TRT 与 ONNX 同目录、同名保存。原始 ONNX 文件会保留。", 0, 0, 24, 230, 700, 24);
        ui->start = control(window, *ui, L"BUTTON", L"开始转换", BS_DEFPUSHBUTTON | WS_TABSTOP, kStart, 24, 270, 150, 38);
        ui->status = control(window, *ui, L"STATIC", L"选择模型后开始转换。", 0, 0, 192, 278, 520, 24);
        ui->progress = control(window, *ui, PROGRESS_CLASSW, L"", PBS_MARQUEE, 0, 24, 323, 704, 18);
        ui->log = control(window, *ui, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
            kLogEdit, 24, 357, 704, 232);
        SendMessageW(ui->log, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
        const auto directory = modelDirectory();
        std::error_code scanError;
        if (!directory.empty()) {
            std::filesystem::recursive_directory_iterator it(directory, scanError), end;
            while (!scanError && it != end) {
                std::error_code fileError;
                if (it->is_regular_file(fileError) && _wcsicmp(it->path().extension().c_str(), L".onnx") == 0)
                    ui->inputs.push_back(it->path());
                it.increment(scanError);
            }
        }
        std::sort(ui->inputs.begin(), ui->inputs.end());
        showInputs(*ui);
        if (directory.empty() || scanError)
            SetWindowTextW(ui->status, L"未能完整扫描“模型数据”及子目录，请手动选择模型。");
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
            if (pick(window, ui->inputs)) showInputs(*ui);
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
        SetWindowTextW(ui->status, result->success ? L"本批处理完成，详情见下方记录" : L"部分或全部失败，详情见下方记录");
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
        else if (flag == L"--fp16") {} // FP16 is always enabled.
        else if ((flag == L"--input" || flag == L"--output" || flag == L"--size") && i + 1 < args.size()) {
            const auto& value = args[++i];
            if (flag == L"--input") options.input = value;
            else if (flag == L"--output") options.output = value;
            else if (value.empty() || !sizeValue(value, options.dynamicWidth, options.dynamicHeight)) { print("Invalid --size; expected WIDTHxHEIGHT, each dimension between 1 and 4096."); return 2; }
        } else { print("Usage: TrtConverter --input MODEL.onnx --output MODEL.trt [--size WIDTHxHEIGHT] [--fp16] [--overwrite]"); return 2; }
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
