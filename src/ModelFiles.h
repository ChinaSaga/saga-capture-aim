#pragma once
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace model_files {
inline std::wstring fromGbk(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(936, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) MultiByteToWideChar(936, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
inline std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
inline std::wstring lowercase(std::wstring value) {
    // Invariant Windows casing handles filename pairing without changing the
    // original displayed basename or depending on a process-local C locale.
    if (value.empty()) return value;
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr, 0);
    return count ? result : value;
}
inline bool parseEngine(const std::string& query, int& engine) {
    bool found = false;
    size_t position = 0;
    while (position <= query.size()) {
        const size_t end = query.find('&', position);
        const std::string field = query.substr(position, end == std::string::npos ? end : end-position);
        if (field.compare(0, 7, "engine=") == 0) {
            const auto value = field.substr(7);
            if (found || (value != "1" && value != "2" && value != "3")) return false;
            engine = value[0]-'0';
            found = true;
        }
        if (end == std::string::npos) break;
        position = end+1;
    }
    return found;
}
inline std::vector<std::string> list(const std::filesystem::path& directory, int engine) {
    if (engine < 1 || engine > 3) return {};
    std::map<std::wstring, std::wstring> names;
    std::set<std::wstring> binaries;
    std::error_code error;
    std::filesystem::directory_iterator iterator(directory, error), end;
    for (; !error && iterator != end; iterator.increment(error)) {
        std::error_code statusError;
        if (!iterator->is_regular_file(statusError) || statusError) continue;
        const auto path = iterator->path();
        const auto extension = lowercase(path.extension().wstring());
        const auto name = path.stem().wstring();
        if (name.empty()) continue;
        const auto key = lowercase(name);
        if (engine == 1 && extension == L".bin") binaries.insert(key);
        if ((engine == 1 && extension == L".param") ||
            (engine == 2 && extension == L".onnx") ||
            (engine == 3 && extension == L".trt")) names.emplace(key, name);
    }
    std::vector<std::string> result;
    for (const auto& entry : names)
        if (engine != 1 || binaries.count(entry.first)) result.push_back(utf8(entry.second));
    return result;
}
inline std::string escapeJson(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += c;
    }
    return result;
}
inline std::string json(const std::filesystem::path& directory, int engine) {
    std::string result = "{\"engine\":" + std::to_string(engine) + ",\"models\":[";
    bool first = true;
    for (const auto& name : list(directory, engine)) {
        if (!first) result += ',';
        result += '"'; result += escapeJson(name); result += '"';
        first = false;
    }
    return result + "]}";
}
}
