#pragma once
#include <charconv>
#include <string>
#include <string_view>

namespace http_request {
constexpr size_t maxHeader = 16 * 1024, maxBody = 64 * 1024;
enum class State { incomplete, complete, invalid, tooLarge };
struct Result {
    State state = State::incomplete;
    size_t bodyOffset = 0, bodyLength = 0;
};
inline std::string_view trim(std::string_view value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == value.npos) return {};
    return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}
inline Result inspect(std::string_view request) {
    const auto end = request.find("\r\n\r\n");
    if (end == request.npos) return {request.size() >= maxHeader ? State::tooLarge : State::incomplete};
    if (end + 4 > maxHeader) return {State::tooLarge};
    Result result{State::incomplete, end + 4, 0};
    auto start = request.find("\r\n");
    if (start == 0 || start > end) return {State::invalid};
    bool hasLength = false;
    for (start += 2; start < end;) {
        const auto next = request.find("\r\n", start);
        const auto line = request.substr(start, next - start);
        const auto colon = line.find(':');
        if (colon == line.npos || !colon || line.front() == ' ' || line.front() == '\t') return {State::invalid};
        std::string name(line.substr(0, colon));
        for (char& c : name) {
            if (c <= 32 || c >= 127) return {State::invalid};
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        }
        const auto value = trim(line.substr(colon + 1));
        if (name == "transfer-encoding") return {State::invalid}; // No chunked request support.
        if (name == "content-length") {
            if (hasLength || value.empty()) return {State::invalid};
            hasLength = true;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.bodyLength);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {State::invalid};
            if (result.bodyLength > maxBody) return {State::tooLarge};
        }
        start = next + 2;
    }
    const auto received = request.size() - result.bodyOffset;
    if (received > result.bodyLength) return {State::invalid};
    if (received == result.bodyLength) result.state = State::complete;
    return result;
}
}
