#pragma once
#include "FilePaths.h"
#include <memory>
#include <limits>

namespace file_io {
inline bool exists(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(file_paths::io(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
inline std::string read(const std::filesystem::path& path) {
    std::unique_ptr<FILE, decltype(&fclose)> file(file_paths::open(path, L"rb"), fclose);
    if (!file || _fseeki64(file.get(), 0, SEEK_END)) return {};
    const auto length = _ftelli64(file.get());
    if (length < 0 || static_cast<unsigned long long>(length) > std::string().max_size() ||
        _fseeki64(file.get(), 0, SEEK_SET)) return {};
    std::string data(static_cast<size_t>(length), '\0');
    if (fread(data.data(), 1, data.size(), file.get()) != data.size() || ferror(file.get())) return {};
    return data;
}
inline bool write(const std::filesystem::path& path, const std::string& data) {
    // Publish only a complete file. Failed writes must preserve the previous
    // configuration instead of leaving a truncated file for the next startup.
    static std::atomic<unsigned long long> serial{0};
    const auto target = file_paths::io(path);
    const auto temporary = target.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(++serial) + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool saved = true;
    size_t offset = 0;
    while (offset < data.size()) {
        const DWORD count = static_cast<DWORD>((std::min)(data.size() - offset, size_t(1024 * 1024)));
        DWORD written = 0;
        if (!WriteFile(file, data.data() + offset, count, &written, nullptr) || written != count) { saved = false; break; }
        offset += written;
    }
    saved = saved && FlushFileBuffers(file);
    CloseHandle(file);
    if (saved) {
        const DWORD attributes = GetFileAttributesW(target.c_str());
        saved = attributes != INVALID_FILE_ATTRIBUTES
            ? ReplaceFileW(target.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr) != FALSE
            : MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!saved) { const DWORD error = GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(error); }
    return saved;
}
}
