#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <csignal>
#include "CrashDiagnostics.h"
#pragma comment(lib, "dbghelp.lib")

namespace {
HANDLE requested = nullptr, finished = nullptr;
LONG handling = 0;
DWORD faultThread = 0;
EXCEPTION_RECORD record{};
CONTEXT context{};
EXCEPTION_POINTERS fault{&record, &context};
wchar_t reportPath[32768]{}, dumpPath[32768]{};
char terminateReason[512]{};

DWORD WINAPI writeCrash(void*) {
    if (WaitForSingleObject(requested, INFINITE) != WAIT_OBJECT_0) return 0;
    HANDLE report = CreateFileW(reportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    char message[2048]{};
    MEMORY_BASIC_INFORMATION region{};
    VirtualQuery(record.ExceptionAddress, &region, sizeof(region));
    wchar_t module[32768]{};
    if (region.AllocationBase) GetModuleFileNameW(static_cast<HMODULE>(region.AllocationBase), module, 32768);
    char moduleUtf8[2048]{};
    WideCharToMultiByte(CP_UTF8, 0, module, -1, moduleUtf8, sizeof(moduleUtf8), nullptr, nullptr);
    const auto offset = reinterpret_cast<ULONG_PTR>(record.ExceptionAddress) - reinterpret_cast<ULONG_PTR>(region.AllocationBase);
    const int length = sprintf_s(message,
        "Saga crash report\r\nBuild: %s %s\r\nPID: %lu  Thread: %lu\r\nException: 0x%08lX\r\nAddress: %p\r\nModule: %s\r\nModule offset: 0x%llX\r\nReason: %s\r\nParameters: %lu  0x%llX 0x%llX\r\n",
        __DATE__, __TIME__, GetCurrentProcessId(), faultThread, record.ExceptionCode,
        record.ExceptionAddress, moduleUtf8, static_cast<unsigned long long>(offset), terminateReason,
        record.NumberParameters, static_cast<unsigned long long>(record.ExceptionInformation[0]),
        static_cast<unsigned long long>(record.ExceptionInformation[1]));
    DWORD written = 0;
    if (report != INVALID_HANDLE_VALUE && length > 0) {
        WriteFile(report, message, length, &written, nullptr);
        FlushFileBuffers(report);
    }
    HANDLE dump = CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL success = FALSE;
    DWORD error = GetLastError();
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{faultThread, &fault, FALSE};
        success = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), &info, nullptr, nullptr);
        error = success ? ERROR_SUCCESS : GetLastError();
        CloseHandle(dump);
    }
    if (report != INVALID_HANDLE_VALUE) {
        const int n = sprintf_s(message, "Minidump: %s; error=%lu\r\n", success ? "saved" : "failed", error);
        if (n > 0) WriteFile(report, message, n, &written, nullptr);
        CloseHandle(report);
    }
    SetEvent(finished);
    return 0;
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* pointers) {
    if (!pointers || InterlockedCompareExchange(&handling, 1, 0)) return EXCEPTION_EXECUTE_HANDLER;
    faultThread = GetCurrentThreadId();
    record = *pointers->ExceptionRecord;
    record.ExceptionRecord = nullptr;
    context = *pointers->ContextRecord;
    SetEvent(requested);
    // Never wait indefinitely on a damaged driver, loader or heap.
    WaitForSingleObject(finished, 8000);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void fatalSignal(DWORD code) {
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}
}

bool initializeCrashDiagnostics(const wchar_t* outputDirectory) {
    if (requested) return true;
    wchar_t directory[32768]{};
    if (outputDirectory) {
        if (wcslen(outputDirectory) + 80 >= 32768) return false;
        wcscpy_s(directory, outputDirectory);
    } else {
        DWORD n = GetModuleFileNameW(nullptr, directory, 32768);
        if (!n || n >= 32700) return false;
        wchar_t* slash = wcsrchr(directory, L'\\');
        if (!slash) return false;
        *slash = 0;
        wcscat_s(directory, L"\\崩溃报告");
    }
    if (!CreateDirectoryW(directory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stem[128]{};
    swprintf_s(stem, L"\\crash-%04u%02u%02u-%02u%02u%02u-%lu", now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
    wcscpy_s(reportPath, directory); wcscat_s(reportPath, stem); wcscat_s(reportPath, L".txt");
    wcscpy_s(dumpPath, directory); wcscat_s(dumpPath, stem); wcscat_s(dumpPath, L".dmp");
    requested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE worker = requested && finished ? CreateThread(nullptr, 0, writeCrash, nullptr, 0, nullptr) : nullptr;
    if (!worker) {
        if (requested) CloseHandle(requested);
        if (finished) CloseHandle(finished);
        requested = finished = nullptr;
        return false;
    }
    CloseHandle(worker);
    SetUnhandledExceptionFilter(crashFilter);
    std::set_terminate([] {
        strcpy_s(terminateReason, "Unhandled C++ exception / std::terminate");
        try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
        catch (const std::exception& e) { strncpy_s(terminateReason, e.what(), _TRUNCATE); }
        catch (...) {}
        fatalSignal(0xE0530001);
    });
    std::signal(SIGABRT, [](int) { fatalSignal(0xE0530002); });
    return true;
}
