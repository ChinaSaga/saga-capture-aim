#include <windows.h>
#include <commctrl.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#include "TrtBuild.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static int nextChoice = IDCANCEL;
static int dialogCalls = 0;
static std::vector<std::wstring> notices;
static std::vector<trt::ConversionOptions> conversions;
static std::function<void()> duringDialog;
static std::function<void(const trt::ConversionOptions&)> duringConversion;
static std::wstring failStem;
static HRESULT WINAPI chooseConversion(const TASKDIALOGCONFIG* dialog, int* button, int*, BOOL*) {
    ++dialogCalls;
    assert(dialog->cButtons == 3);
    assert(std::wstring(dialog->pButtons[0].pszButtonText) == L"代替并转换");
    assert(std::wstring(dialog->pButtons[1].pszButtonText) == L"不代替转换");
    assert(std::wstring(dialog->pButtons[2].pszButtonText) == L"不转换");
    assert(dialog->nDefaultButton == dialog->pButtons[1].nButtonID);
    if (duringDialog) duringDialog();
    *button = nextChoice;
    return S_OK;
}
static int WINAPI recordMessage(HWND, LPCWSTR message, LPCWSTR, UINT) {
    notices.emplace_back(message);
    return IDOK;
}
#define TaskDialogIndirect chooseConversion
#define MessageBoxW recordMessage
#include "../src/TrtConverter.cpp"
#undef TaskDialogIndirect
#undef MessageBoxW

static void writeFile(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
    assert(stream.good());
}
static std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
bool trt::convertOnnx(const ConversionOptions& options, const BuildLog&, std::string& error) {
    conversions.push_back(options);
    if (duringConversion) duringConversion(options);
    if (options.input.stem().wstring() == failStem) { error = "fixture failure"; return false; }
    if (!options.overwrite && std::filesystem::exists(options.output)) {
        error = "Output already exists";
        return false;
    }
    writeFile(options.output, "converted " + options.input.stem().string());
    return true;
}

int main() {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&common);
    WNDCLASSW type{};
    type.lpfnWndProc = procedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"TrtConverterRegression";
    assert(RegisterClassW(&type));
    Ui ui;
    HWND window = CreateWindowW(type.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 776, 644,
        nullptr, nullptr, type.hInstance, &ui);
    assert(window);
    const auto fixture = std::filesystem::temp_directory_path() /
        (L"saga-trt-choice-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(fixture));
    ui.inputs.clear();
    for (const auto* stem : {L"a", L"b", L"c"}) {
        auto input = fixture / (std::wstring(stem) + L".onnx");
        writeFile(input, "original ONNX");
        ui.inputs.push_back(input);
    }
    const auto a = fixture / L"a.trt", b = fixture / L"b.trt", c = fixture / L"c.trt";
    auto clearOutputs = [&] {
        for (const auto& output : {a, b, c}) std::filesystem::remove(output);
    };
    auto run = [&](int choice) {
        nextChoice = choice;
        conversions.clear(); notices.clear();
        const int previousDialogs = dialogCalls;
        start(window, ui);
        if (ui.worker.joinable()) ui.worker.join();
        MSG message{};
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        assert(!ui.busy);
        assert(dialogCalls == previousDialogs + 1);
    };
    auto summary = [&](int completed, int skipped, int failed) {
        assert(notices.size() == 1);
        const auto expected = L"成功 " + std::to_wstring(completed) + L" 个，跳过 " +
            std::to_wstring(skipped) + L" 个，失败 " + std::to_wstring(failed) + L" 个";
        assert(notices.back().find(expected) != std::wstring::npos);
    };

    // Mixed batch: preserve both existing bytes and timestamp, convert only missing outputs.
    writeFile(a, "original TRT");
    const auto originalTime = std::filesystem::last_write_time(a);
    run(kSkipExisting);
    summary(2, 1, 0);
    assert(conversions.size() == 2);
    for (const auto& job : conversions) assert(!job.overwrite);
    assert(readFile(a) == "original TRT" && std::filesystem::last_write_time(a) == originalTime);
    assert(readFile(b) == "converted b" && readFile(c) == "converted c");

    // All exist: no converter invocation and no false failures.
    run(kSkipExisting);
    summary(0, 3, 0);
    assert(conversions.empty());
    assert(readFile(a) == "original TRT" && std::filesystem::last_write_time(a) == originalTime);

    // Explicit replacement applies to the complete batch.
    run(kReplace);
    summary(3, 0, 0);
    assert(conversions.size() == 3);
    for (const auto& job : conversions) assert(job.overwrite);
    assert(readFile(a) == "converted a");

    // Cancel never starts a worker, clears a previous log, or changes output bytes.
    SetWindowTextW(ui.log, L"previous log");
    const auto beforeCancel = std::filesystem::last_write_time(a);
    run(IDCANCEL);
    assert(notices.empty() && conversions.empty());
    assert(text(ui.log) == L"previous log");
    assert(readFile(a) == "converted a" && std::filesystem::last_write_time(a) == beforeCancel);

    // No existing files still shows the three choices and converts everything.
    clearOutputs();
    run(kSkipExisting);
    summary(3, 0, 0);
    assert(conversions.size() == 3);

    // Recheck outputs created while the confirmation is open or an earlier job is running.
    clearOutputs();
    duringDialog = [&] { writeFile(a, "arrived during dialog"); };
    duringConversion = [&](const auto& job) {
        if (job.input.stem() == L"b") writeFile(c, "arrived during conversion");
    };
    run(kSkipExisting);
    summary(1, 2, 0);
    assert(conversions.size() == 1 && conversions.front().input.stem() == L"b");
    assert(readFile(a) == "arrived during dialog" && readFile(c) == "arrived during conversion");
    duringDialog = {}; duringConversion = {};

    // A failed conversion must not prevent later jobs or count skipped files as failures.
    clearOutputs();
    failStem = L"b";
    run(kSkipExisting);
    summary(2, 0, 1);
    assert(conversions.size() == 3 && !std::filesystem::exists(b));
    assert(readFile(a) == "converted a" && readFile(c) == "converted c");

    for (const auto& input : ui.inputs) {
        assert(readFile(input) == "original ONNX");
        std::filesystem::remove(input);
    }
    clearOutputs();
    assert(std::filesystem::remove(fixture));
    DestroyWindow(window);
    std::cout << "Converter replace/skip/cancel, output preservation, late-file and failure checks passed\n";
}
