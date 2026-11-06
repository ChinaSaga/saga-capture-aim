#define NOMINMAX
#include "RuntimeLog.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace runtime_log {
namespace {
constexpr size_t maxFileBytes = 1024 * 1024;
constexpr size_t maxLines = 2000, retainedLines = 1000;
constexpr size_t maxQueuedBytes = 256 * 1024, maxQueuedRecords = 1024;
constexpr size_t maxTextBytes = 32 * 1024;

struct State {
    std::mutex lifecycle, mutex;
    std::condition_variable ready;
    std::atomic<bool> accepting{false};
    bool stopping = false;
    std::deque<std::string> records;
    size_t queuedBytes = 0, dropped = 0;
    std::wstring path;
    std::thread worker;
};

State& state()
{
    // The app closes with TerminateProcess. Avoid detached worker/static
    // teardown races on other early exits; the OS owns process cleanup.
    static State* value = new State;
    return *value;
}

std::string toUtf8(std::wstring_view text)
{
    if (text.empty()) return {};
    const int count = static_cast<int>((std::min)(text.size(), maxTextBytes));
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), count, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), count, result.data(), size, nullptr, nullptr);
    return result;
}

std::string normalize(std::string text)
{
    if (text.size() > maxTextBytes) {
        size_t cut = maxTextBytes;
        while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
        text.resize(cut);
        text += "\n[log] oversized record truncated";
    }
    std::string result;
    result.reserve(text.size() + 1);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') continue;
            result += '\n';
        } else if (text[i] != '\0') result += text[i];
    }
    if (result.empty() || result.back() != '\n') result += '\n';
    return result;
}

std::string timestamp()
{
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u UTC pid=%lu] ",
        static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay),
        static_cast<unsigned>(utc.wHour), static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond),
        GetCurrentProcessId());
    return prefix;
}

void enqueue(std::string text)
{
    auto& log = state();
    if (!log.accepting.load(std::memory_order_acquire) || text.empty()) return;
    std::string record = timestamp() + normalize(std::move(text));
    {
        std::lock_guard lock(log.mutex);
        if (!log.accepting.load(std::memory_order_relaxed)) return;
        while (!log.records.empty() && (log.records.size() >= maxQueuedRecords ||
            log.queuedBytes + record.size() > maxQueuedBytes)) {
            log.queuedBytes -= log.records.front().size();
            log.records.pop_front();
            ++log.dropped;
        }
        log.queuedBytes += record.size();
        log.records.push_back(std::move(record));
    }
    log.ready.notify_one();
}

bool trim(std::string& contents)
{
    size_t cut = 0;
    const size_t lines = std::count(contents.begin(), contents.end(), '\n');
    if (lines > maxLines) {
        for (size_t i = 0; i < lines - retainedLines; ++i)
            cut = contents.find('\n', cut) + 1;
    }
    if (contents.size() - cut > maxFileBytes) {
        const size_t boundary = contents.find('\n', contents.size() - maxFileBytes - 1);
        cut = boundary == std::string::npos ? contents.size() : (std::max)(cut, boundary + 1);
    }
    if (cut) contents.erase(0, cut);
    return cut != 0;
}

bool writeAll(HANDLE file, std::string_view contents)
{
    while (!contents.empty()) {
        DWORD written = 0;
        if (!WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) || !written)
            return false;
        contents.remove_prefix(written);
    }
    return true;
}

void appendBatch(const std::wstring& path, const std::string& batch)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{file};
    OVERLAPPED lock{};
    // Serialize concurrent copies of this EXE without ever waiting on a
    // producer thread. If the file is busy, this optional batch is discarded.
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &lock)) return;
    struct Unlock { HANDLE value; OVERLAPPED* lock; ~Unlock() { UnlockFileEx(value, 0, MAXDWORD, MAXDWORD, lock); } } unlock{file, &lock};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) return;
    LARGE_INTEGER start{};
    start.QuadPart = (std::max)(0LL, size.QuadPart - static_cast<LONGLONG>(maxFileBytes));
    if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return;
    std::string history(static_cast<size_t>(size.QuadPart - start.QuadPart), '\0');
    DWORD received = 0;
    if (!history.empty() && !ReadFile(file, history.data(), static_cast<DWORD>(history.size()), &received, nullptr)) return;
    history.resize(received);
    bool rewrite = start.QuadPart != 0;
    if (rewrite) {
        // A suffix of an oversized pre-existing file may start mid-line.
        const size_t newline = history.find('\n');
        history.erase(0, newline == std::string::npos ? history.size() : newline + 1);
    }
    if (!history.empty() && history.back() != '\n') {
        // A killed process may have left a partial last write. Discard it.
        const size_t newline = history.rfind('\n');
        history.resize(newline == std::string::npos ? 0 : newline + 1);
        rewrite = true;
    }
    history += batch;
    rewrite = trim(history) || rewrite;
    LARGE_INTEGER position{};
    position.QuadPart = rewrite ? 0 : size.QuadPart;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return;
    if (!writeAll(file, rewrite ? std::string_view(history) : std::string_view(batch))) return;
    if (rewrite) SetEndOfFile(file);
    // No FlushFileBuffers: ordinary buffered disk writes suffice for diagnostics.
}

void run(State& log)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    // Bound an old file immediately in the worker, including on quiet starts.
    appendBatch(log.path, {});
    for (;;) {
        std::deque<std::string> records;
        size_t dropped = 0;
        bool stopping = false;
        {
            std::unique_lock lock(log.mutex);
            log.ready.wait(lock, [&] { return log.stopping || !log.records.empty(); });
            if (!log.stopping)
                log.ready.wait_for(lock, std::chrono::milliseconds(250), [&] { return log.stopping; });
            records.swap(log.records);
            log.queuedBytes = 0;
            dropped = std::exchange(log.dropped, 0);
            stopping = log.stopping;
        }
        std::string batch;
        if (dropped) batch = timestamp() + "[log] queue full; older records discarded: " + std::to_string(dropped) + "\n";
        for (auto& record : records) batch += record;
        if (!batch.empty()) appendBatch(log.path, batch);
        if (stopping) return;
    }
}
} // namespace

void initialize()
{
    auto& log = state();
    std::lock_guard lifecycle(log.lifecycle);
    if (log.accepting.load() || log.worker.joinable()) return;
    wchar_t path[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, _countof(path));
    if (!size || size >= _countof(path)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    log.path.assign(path, slash + 1);
    log.path += L"运行日志.txt";
    log.stopping = false;
    log.accepting.store(true, std::memory_order_release);
    try { log.worker = std::thread(run, std::ref(log)); }
    catch (...) { log.accepting.store(false, std::memory_order_release); }
}

void shutdown()
{
    auto& log = state();
    std::lock_guard lifecycle(log.lifecycle);
    {
        std::lock_guard lock(log.mutex);
        log.accepting.store(false, std::memory_order_release);
        log.stopping = true;
    }
    log.ready.notify_one();
    if (log.worker.joinable()) log.worker.join();
}

void writeV(const char* format, va_list arguments)
{
    if (!state().accepting.load(std::memory_order_acquire) || !format) return;
    std::vector<char> text(maxTextBytes + 1);
    va_list copy;
    va_copy(copy, arguments);
    const int written = std::vsnprintf(text.data(), text.size(), format, copy);
    va_end(copy);
    if (written < 0) return;
    const int bytes = static_cast<int>((std::min)(static_cast<size_t>(written), maxTextBytes));
    constexpr UINT inputCodePage = 936; // Project execution charset is GBK.
    const int count = MultiByteToWideChar(inputCodePage, 0, text.data(), bytes, nullptr, 0);
    if (count <= 0) return;
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(inputCodePage, 0, text.data(), bytes, wide.data(), count);
    enqueue(toUtf8(wide));
}

void write(const char* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    writeV(format, arguments);
    va_end(arguments);
}

void writeWide(std::wstring_view text)
{
    if (state().accepting.load(std::memory_order_acquire)) enqueue(toUtf8(text));
}
} // namespace runtime_log
