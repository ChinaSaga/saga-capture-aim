#define NOMINMAX
#include "../src/RuntimeLog.h"
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
static void seed(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(file); file.write(data.data(), data.size()); assert(file);
}
int main() {
    wchar_t module[32768]{};
    assert(GetModuleFileNameW(nullptr, module, _countof(module)));
    const fs::path directory = fs::canonical(fs::path(module).parent_path());
    bool isolated = false;
    for (const auto& part : directory) if (part == L".workbuddy") isolated = true;
    assert(isolated && "Compile/run this test only within its own .workbuddy directory");
    const auto file = directory / L"运行日志.txt";
    assert(!fs::exists(file) && "Use a fresh isolated test directory");
    std::string history;
    for (int i = 0; i < 2001; ++i) history += "old-" + std::to_string(i) + "\n";
    seed(file, history);
    runtime_log::initialize(); runtime_log::shutdown();
    std::string text = read(file);
    assert(std::count(text.begin(), text.end(), '\n') == 1000);
    assert(text.starts_with("old-1001\n") && text.ends_with("old-2000\n"));
    std::cout << "PASS: startup trims 2001 lines to newest 1000.\n";

    history.clear();
    for (int i = 0; i < 1999; ++i) history += "history-" + std::to_string(i) + "\n";
    seed(file, history);
    runtime_log::initialize();
    runtime_log::write("new-record-1");
    runtime_log::write("new-record-2");
    runtime_log::write("new-record-3");
    runtime_log::shutdown(); text = read(file);
    assert(std::count(text.begin(), text.end(), '\n') == 1000);
    assert(text.starts_with("history-1002\n") && text.ends_with("new-record-3\n"));
    std::cout << "PASS: appending across the line limit keeps newest records.\n";

    seed(file, {});
    runtime_log::initialize();
    runtime_log::writeWide(L"中文串口诊断：正常\r\n");
    runtime_log::write("GBK中文诊断");
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([t] {
        for (int i = 0; i < 80; ++i) runtime_log::write("thread=%d item=%d END", t, i);
    });
    for (auto& thread : threads) thread.join();
    runtime_log::shutdown(); text = read(file);
    assert(text.find("\xe4\xb8\xad\xe6\x96\x87") != std::string::npos);
    std::set<std::string> messages;
    std::istringstream lines(text); std::string line;
    while (std::getline(lines, line)) {
        auto begin = line.find("thread=");
        if (begin != std::string::npos) { assert(line.ends_with(" END")); messages.insert(line.substr(begin)); }
    }
    assert(messages.size() == 320);
    for (int t = 0; t < 4; ++t) for (int i = 0; i < 80; ++i)
        assert(messages.contains("thread=" + std::to_string(t) + " item=" + std::to_string(i) + " END"));
    std::cout << "PASS: UTF-8 Chinese and concurrent records remain intact.\n";

    history.clear();
    for (int i = 0; i < 1500; ++i) history += std::string(1000, 'x') + "\n";
    seed(file, history);
    runtime_log::initialize();
    runtime_log::writeWide(std::wstring(60000, L'中'));
    runtime_log::write("FINAL-MARKER");
    runtime_log::shutdown(); text = read(file);
    assert(fs::file_size(file) <= 1024 * 1024);
    assert(text.ends_with("FINAL-MARKER\n"));
    assert(text.find("oversized record truncated") != std::string::npos);
    std::cout << "PASS: oversized old file and record respect 1 MiB cap.\n";

    HANDLE held = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(held != INVALID_HANDLE_VALUE);
    runtime_log::initialize();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) runtime_log::write("occupied-file-message %d", i);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(elapsed < std::chrono::milliseconds(200));
    runtime_log::shutdown();
    CloseHandle(held);
    assert(read(file) == text);
    std::cout << "PASS: exclusively occupied file does not block producers.\n";
    fs::remove(file);
}
