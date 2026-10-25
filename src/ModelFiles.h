#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <utility>
#include <string>
#include <vector>
#include "FilePaths.h"
#include "AdaptiveCapture.h"

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
struct Model {
    std::string name;
    std::array<std::string, 3> engineNames;
    bool group = false;
    std::array<std::vector<int>, 3> sizes;
};
inline bool isGroup(const std::filesystem::path& directory, const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.find_first_of(L"/\\:") != std::wstring::npos) return false;
    std::error_code error;
    return std::filesystem::is_directory(file_paths::io(directory / name), error);
}
inline std::vector<int> groupSizes(const std::filesystem::path& directory, int engine) {
    std::vector<int> sizes;
    for (int size = adaptive_capture::minSize; size <= adaptive_capture::maxSize; size += adaptive_capture::sizeStep) {
        const auto base = directory / std::to_wstring(size);
        const auto exists = [&](const wchar_t* extension) {
            std::error_code error;
            return std::filesystem::is_regular_file(file_paths::io(base.wstring() + extension), error);
        };
        if ((engine == 1 && exists(L".param") && exists(L".bin")) ||
            (engine == 2 && exists(L".onnx")) || (engine == 3 && exists(L".trt"))) sizes.push_back(size);
    }
    return sizes;
}
inline std::vector<Model> catalog(const std::filesystem::path& directory, bool includeGroups = true) {
    struct Files { std::wstring param, bin, onnx, trt; };
    std::map<std::wstring, Files> files;
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    std::filesystem::directory_iterator iterator(file_paths::io(directory), error), end;
    for (; !error && iterator != end; iterator.increment(error)) {
        std::error_code statusError;
        const bool regular = iterator->is_regular_file(statusError);
        if (statusError) throw std::filesystem::filesystem_error("Cannot inspect model file", iterator->path(), statusError);
        if (!regular) {
            if (includeGroups && iterator->is_directory(statusError)) folders.push_back(iterator->path());
            continue;
        }
        const auto path = iterator->path();
        const auto extension = lowercase(path.extension().wstring());
        const auto name = path.stem().wstring();
        if (name.empty()) continue;
        const auto key = lowercase(name);
        if (extension == L".param") files[key].param = name;
        else if (extension == L".bin") files[key].bin = name;
        else if (extension == L".onnx") files[key].onnx = name;
        else if (extension == L".trt") files[key].trt = name;
    }
    if (error) throw std::filesystem::filesystem_error("Cannot read model directory", directory, error);
    std::vector<Model> result;
    for (const auto& [key, entry] : files) {
        Model model;
        if (!entry.param.empty() && !entry.bin.empty()) model.engineNames[0] = utf8(entry.param);
        model.engineNames[1] = utf8(entry.onnx); model.engineNames[2] = utf8(entry.trt);
        for (const auto& name : model.engineNames) if (!name.empty()) { model.name = name; break; }
        if (!model.name.empty()) result.push_back(std::move(model));
    }
    for (const auto& path : folders) {
        Model model;
        model.name = utf8(path.filename().wstring());
        model.group = true;
        bool any = false;
        for (int engine = 1; engine <= 3; ++engine) {
            auto& sizes = model.sizes[engine-1];
            sizes = groupSizes(path, engine);
            any = any || !sizes.empty();
            if (!sizes.empty()) model.engineNames[engine-1] = model.name;
        }
        if (!any) continue;
        // A folder owns its name, including when a legacy file shares that stem.
        const auto key = lowercase(path.filename().wstring());
        result.erase(std::remove_if(result.begin(), result.end(), [&](const Model& old) {
            return lowercase(file_paths::wide(old.name, CP_UTF8)) == key;
        }), result.end());
        result.push_back(std::move(model));
    }
    std::sort(result.begin(), result.end(), [](const Model& a, const Model& b) {
        return lowercase(file_paths::wide(a.name, CP_UTF8)) < lowercase(file_paths::wide(b.name, CP_UTF8));
    });
    return result;
}
inline std::vector<std::string> list(const std::filesystem::path& directory, int engine) {
    if (engine < 1 || engine > 3) return {};
    std::vector<std::string> result;
    for (const auto& model : catalog(directory))
        if (!model.engineNames[engine-1].empty()) result.push_back(model.engineNames[engine-1]);
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
inline std::string catalogJson(const std::filesystem::path& directory) {
    std::string result = "{\"models\":[";
    bool first = true;
    for (const auto& model : catalog(directory)) {
        if (!first) result += ',';
        first = false;
        result += "{\"name\":\"" + escapeJson(model.name) + "\",\"engines\":[";
        bool firstEngine = true;
        for (int engine = 1; engine <= 3; ++engine) if (!model.engineNames[engine-1].empty()) {
            if (!firstEngine) result += ',';
            firstEngine = false; result += std::to_string(engine);
        }
        result += "]";
        if (model.group) {
            result += ",\"adaptive\":true,\"sizes\":{";
            for (int engine = 1; engine <= 3; ++engine) {
                if (engine > 1) result += ',';
                result += "\"" + std::to_string(engine) + "\":[";
                bool firstSize = true;
                for (int size : model.sizes[engine-1]) {
                    if (!firstSize) result += ',';
                    firstSize = false; result += std::to_string(size);
                }
                result += ']';
            }
            result += '}';
        }
        result += '}';
    }
    return result + "]}";
}
}
