#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace makcu::detail {

// Device commands never retain this view after the synchronous write completes.
// Each sending thread owns its cache; equal deltas still get sent on every call.
class MoveCommandCache {
public:
    std::string_view format(int32_t x, int32_t y) noexcept {
        if (!valid_ || x != lastX_ || y != lastY_) {
            char* out = bytes_;
            std::memcpy(out, "km.move(", 8);
            out = integer(out + 8, x);
            *out++ = ',';
            out = integer(out, y);
            *out++ = ')';
            length_ = static_cast<size_t>(out - bytes_);
            *out = '\0';
            lastX_ = x;
            lastY_ = y;
            valid_ = true;
        }
        return {bytes_, length_};
    }

private:
    static char* integer(char* out, int32_t value) noexcept {
        if (value < 0) *out++ = '-';
        uint32_t number = value < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(value))
                                    : static_cast<uint32_t>(value);
        if (number < 10000) {
            if (number >= 1000) { *out++ = static_cast<char>('0' + number / 1000); number %= 1000; }
            else if (number < 100) {
                if (number >= 10) { *out++ = static_cast<char>('0' + number / 10); number %= 10; }
                *out++ = static_cast<char>('0' + number);
                return out;
            }
            *out++ = static_cast<char>('0' + number / 100); number %= 100;
            *out++ = static_cast<char>('0' + number / 10);
            *out++ = static_cast<char>('0' + number % 10);
            return out;
        }
        char reversed[10];
        char* end = reversed;
        do { *end++ = static_cast<char>('0' + number % 10); number /= 10; } while (number);
        while (end != reversed) *out++ = *--end;
        return out;
    }

    char bytes_[40]{};
    size_t length_ = 0;
    int32_t lastX_ = 0, lastY_ = 0;
    bool valid_ = false;
};

inline bool isMakcuMoveInRange(int32_t x, int32_t y) noexcept {
    return x >= -32768 && x <= 32767 && y >= -32768 && y <= 32767;
}

enum class DeviceIdentity { Unknown, Makcu, Ferrum };

inline std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
        value.remove_suffix(1);
    return value;
}

inline char lowerAscii(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

inline bool identityPrefix(std::string_view value, std::string_view brand) noexcept {
    if (value.size() < brand.size()) return false;
    for (size_t i = 0; i < brand.size(); ++i)
        if (lowerAscii(value[i]) != lowerAscii(brand[i])) return false;
    if (value.size() == brand.size()) return true;
    const char next = value[brand.size()];
    return next == ' ' || next == '\t' || next == ':' || next == '-' || next == '_' || next == '.' ||
           (next >= '0' && next <= '9');
}

// Only a recognized identity payload is evidence of a responding device.
// A command echo, button stream prefix, error, or arbitrary text is not one.
inline DeviceIdentity identifyDevice(std::string_view value) noexcept {
    value = trim(value);
    for (const unsigned char c : value)
        if (c < 0x20 || c > 0x7e) return DeviceIdentity::Unknown;
    if (value.substr(0, 3) == "km.") value.remove_prefix(3);
    if (identityPrefix(value, "Ferrum")) return DeviceIdentity::Ferrum;
    if (identityPrefix(value, "MAKCU") || identityPrefix(value, "kmbox")) return DeviceIdentity::Makcu;
    return DeviceIdentity::Unknown;
}

inline int parseButtonState(std::string_view value, std::string_view name) noexcept {
    value = trim(value);
    if (value.substr(0, 3) == "km.") {
        value.remove_prefix(3);
        if (value.substr(0, name.size()) != name) return -1;
        value.remove_prefix(name.size());
        if (value.size() < 3 || value.front() != '(' || value.back() != ')') return -1;
        value.remove_prefix(1);
        value.remove_suffix(1);
        value = trim(value);
    }
    return value.size() == 1 && value[0] >= '0' && value[0] <= '3' ? value[0] - '0' : -1;
}

} // namespace makcu::detail
