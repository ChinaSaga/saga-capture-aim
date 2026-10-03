#pragma once
#include <windows.h>
#include <filesystem>
#include <string>

namespace model_paths {
inline constexpr wchar_t folder[] = L"模型数据";
inline std::filesystem::path directory(const std::filesystem::path& executableDirectory) {
    return executableDirectory / folder;
}
inline std::string gbkDirectory(const std::string& executableDirectory) {
    // The application uses GBK paths; the converter uses Unicode paths.
    // Encode explicitly so this shared header is independent of /utf-8.
    char encoded[32]{};
    WideCharToMultiByte(936, 0, folder, -1, encoded, sizeof(encoded), nullptr, nullptr);
    return executableDirectory + "\\" + encoded;
}
inline std::filesystem::path defaultTrtOutput(const std::filesystem::path& executableDirectory,
    const std::filesystem::path& input) {
    auto name = input.filename();
    name.replace_extension(L".trt");
    return directory(executableDirectory) / name;
}
}
