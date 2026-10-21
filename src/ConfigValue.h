#pragma once
#include "FilePaths.h"
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>

namespace config_value {
inline int integer(const std::string& text) {
    int value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : 0;
}
inline bool finite(float value) {
    // Preserve validation with the project's /fp:fast configuration.
    return (std::bit_cast<std::uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
}
inline float number(const std::string& text) {
    float value = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && finite(value) ? value : 0;
}
inline bool leafName(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    try {
        const auto value = file_paths::wide(name);
        if (value.back() == L'.' || value.back() == L' ' || value.find_first_of(L"\\/:*?\"<>|") != value.npos) return false;
        for (wchar_t c : value) if (c < 32) return false;
        auto base = value.substr(0, value.find(L'.'));
        for (wchar_t& c : base) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
        if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return false;
        if (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
            ((base[3] >= L'1' && base[3] <= L'9') || base[3] == L'\u00b9' || base[3] == L'\u00b2' || base[3] == L'\u00b3')) return false;
        return true;
    } catch (const std::exception&) { return false; }
}
}
