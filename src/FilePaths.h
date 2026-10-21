#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdlib>

// UI/config text remains GBK. Filesystem paths never pass through the active
// Windows code page: a desktop/user/OneDrive folder can contain any Unicode.
namespace file_paths {
inline std::wstring wide(const std::string& text, UINT page = 936) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw std::runtime_error("Invalid path encoding");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
inline std::string narrow(const std::wstring& text, UINT page = 936) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(page, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(page, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}
inline std::filesystem::path io(const std::filesystem::path& path) {
    if (path.empty()) return {};
    auto value = std::filesystem::absolute(path).lexically_normal().wstring();
    if (value.compare(0, 4, L"\\\\?\\") == 0) return value;
    if (value.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
inline std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!size || size >= buffer.size()) throw std::runtime_error("Cannot determine executable directory");
    return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
}
inline FILE* open(const std::filesystem::path& path, const wchar_t* mode) {
    return _wfopen(io(path).c_str(), mode);
}
inline std::wstring errorText(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = size ? std::wstring(buffer, size) : L"Windows error";
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result + L" (" + std::to_wstring(code) + L")";
}
// Legacy INI files are GBK, regardless of the system ACP. W profile APIs cannot
// write a new ANSI INI under activeCodePage=UTF-8 (ERROR_INVALID_PARAMETER).
// Read without modifying files; migrate to UTF-16 only on an actual save.
inline bool readIni(const std::filesystem::path& path, std::string& bytes) {
    std::unique_ptr<FILE, decltype(&fclose)> file(open(path, L"rb"), fclose);
    if (!file || fseek(file.get(), 0, SEEK_END)) return false;
    const long length = ftell(file.get());
    if (length < 0 || length > 1024*1024 || fseek(file.get(), 0, SEEK_SET)) return false;
    bytes.resize(static_cast<size_t>(length));
    return fread(bytes.data(), 1, bytes.size(), file.get()) == bytes.size() && !ferror(file.get());
}
inline std::wstring iniText(const std::string& bytes) {
    if (bytes.starts_with("\xff\xfe")) {
        if (bytes.size()%2) throw std::runtime_error("Invalid UTF-16 INI file");
        std::wstring value((bytes.size()-2)/sizeof(wchar_t), L'\0');
        memcpy(value.data(), bytes.data()+2, value.size()*sizeof(wchar_t));
        return value;
    }
    return bytes.starts_with("\xef\xbb\xbf") ? wide(bytes.substr(3), CP_UTF8) : wide(bytes);
}
inline std::wstring iniTrim(const std::wstring& text) {
    const auto start = text.find_first_not_of(L" \t\r\n");
    return start == std::wstring::npos ? L"" : text.substr(start, text.find_last_not_of(L" \t\r\n")-start+1);
}
inline DWORD profileString(const char* section, const char* key, const char* fallback,
    char* output, DWORD capacity, const wchar_t* path) {
    std::string bytes, value = fallback;
    if (readIni(path, bytes)) {
        std::wstring text;
        try { text = iniText(bytes); } catch (const std::runtime_error&) { /* Keep the fallback for malformed files. */ }
        const auto wantedSection = wide(section), wantedKey = wide(key);
        const auto equal = [](const std::wstring& a, const std::wstring& b) {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
        };
        bool inSection = false;
        for (size_t start = 0; start < text.size();) {
            const auto end = text.find(L'\n', start);
            const auto line = iniTrim(text.substr(start, end == std::wstring::npos ? end : end-start));
            if (line.size() >= 2 && line.front() == L'[' && line.back() == L']')
                inSection = equal(iniTrim(line.substr(1,line.size()-2)), wantedSection);
            else if (inSection && !line.empty() && line.front() != L';' && line.front() != L'#') {
                const auto separator = line.find(L'=');
                if (separator != std::wstring::npos && equal(iniTrim(line.substr(0,separator)), wantedKey)) {
                    auto item = iniTrim(line.substr(separator+1));
                    if (item.size() >= 2 && (item.front() == L'\'' || item.front() == L'"') && item.back() == item.front())
                        item = item.substr(1,item.size()-2);
                    value = narrow(item); break;
                }
            }
            if (end == std::wstring::npos) break;
            start = end+1;
        }
    }
    if (!capacity) return 0;
    const auto count = (std::min)(value.size(), static_cast<size_t>(capacity - 1));
    memcpy(output, value.data(), count); output[count] = 0;
    return static_cast<DWORD>(count);
}
inline UINT profileInt(const char* section, const char* key, int fallback, const wchar_t* path) {
    char value[64]{};
    profileString(section,key,std::to_string(fallback).c_str(),value,sizeof(value),path);
    return static_cast<UINT>(strtol(value,nullptr,10));
}
inline BOOL writeProfile(const char* section, const char* key, const char* value, const wchar_t* path) {
    static std::mutex mutex;
    static std::atomic<unsigned> serial{0};
    std::lock_guard guard(mutex);
    const auto target = io(path);
    std::string bytes;
    const auto attributes = GetFileAttributesW(target.c_str());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    if (exists && (!readIni(target,bytes) || (attributes & FILE_ATTRIBUTE_READONLY))) return FALSE;
    if (!bytes.starts_with("\xff\xfe")) {
        std::wstring text;
        try { text = iniText(bytes); }
        catch (const std::runtime_error&) { SetLastError(ERROR_NO_UNICODE_TRANSLATION); return FALSE; }
        const auto temporary = target.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial)+L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file == INVALID_HANDLE_VALUE) return FALSE;
        std::string encoded("\xff\xfe",2);
        encoded.append(reinterpret_cast<const char*>(text.data()),text.size()*sizeof(wchar_t));
        DWORD written = 0;
        const bool saved = WriteFile(file,encoded.data(),static_cast<DWORD>(encoded.size()),&written,nullptr) &&
            written == encoded.size() && FlushFileBuffers(file);
        CloseHandle(file);
        const bool replaced = saved && (exists ? ReplaceFileW(target.c_str(),temporary.c_str(),nullptr,0,nullptr,nullptr) :
            MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH));
        if (!replaced) { const DWORD error = GetLastError(); DeleteFileW(temporary.c_str()); SetLastError(error); return FALSE; }
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,target.c_str());
    }
    return WritePrivateProfileStringW(wide(section).c_str(), wide(key).c_str(), wide(value).c_str(), target.c_str());
}
}
