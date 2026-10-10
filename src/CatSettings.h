#pragma once
#include <charconv>
#include <string>
#include <string_view>

struct CatSettings {
    std::string ip, port, uuid;
    bool enabled = false;
};

inline bool validCatSettings(const CatSettings& s) {
    unsigned port = 0;
    auto result = std::from_chars(s.port.data(), s.port.data() + s.port.size(), port);
    if (result.ec != std::errc{} || result.ptr != s.port.data() + s.port.size() || !port || port > 65535)
        return false;
    if (s.uuid.size() != 8) return false;
    for (char c : s.uuid)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    std::string_view ip(s.ip);
    unsigned first = 0;
    for (unsigned i = 0; i < 4; ++i) {
        auto dot = ip.find('.');
        auto part = ip.substr(0, dot);
        if (part.size() > 1 && part.front() == '0') return false;
        unsigned value = 0;
        auto r = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || r.ec != std::errc{} || r.ptr != part.data() + part.size() || value > 255) return false;
        if (i == 0) first = value;
        if (i == 3) return dot == std::string_view::npos && first != 0 && first < 224 && s.ip != "255.255.255.255";
        if (dot == std::string_view::npos) return false;
        ip.remove_prefix(dot + 1);
    }
    return false;
}
