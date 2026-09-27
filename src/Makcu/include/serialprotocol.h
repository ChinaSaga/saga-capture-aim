#pragma once

#include <cstdint>
#include <charconv>
#include <string>
#include <string_view>
#include <utility>
#include "commands.h"

namespace makcu::protocol {

enum class EventKind { None, QueryComplete, Button, ProtocolError };
struct Event {
    EventKind kind = EventKind::None;
    uint8_t buttonMask = 0;
    std::string text;
};

// Text queries and button streaming use separate parsing modes. Streaming
// accepts raw masks and known framed masks/ACKs found on actual devices.
// The caller must disable echo/buttons and finish a query+prompt handshake
// before entering RawButtons. Unframed CR/LF are valid button masks.
class Parser {
public:
    void reset() {
        raw_ = false;
        rawReplySize_ = 0;
        rawTrailerOffset_ = 0;
        framedMaskPending_ = false;
        boundary_ = false;
        query_.clear();
        candidate_.clear();
        line_.clear();
        afterCR_ = false;
        afterLine_ = false;
        discardLine_ = false;
        prompt_ = 0;
    }

    bool isText() const noexcept { return !raw_; }

    bool beginQuery(std::string_view command) {
        if (raw_ || !query_.empty() || command.size() < 6 ||
            command.substr(0, 3) != "km." || command.substr(command.size() - 2) != "()") return false;
        for (char c : command.substr(3, command.size() - 5))
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        query_.assign(command);
        candidate_.clear();
        boundary_ = false;
        return true;
    }

    void cancelQuery() {
        query_.clear();
        candidate_.clear();
        boundary_ = false;
    }

    bool enterRawButtons() {
        if (raw_ || !boundary_ || !query_.empty() || !line_.empty() || afterCR_ || prompt_) return false;
        raw_ = true;
        rawReplySize_ = 0;
        rawTrailerOffset_ = 0;
        framedMaskPending_ = false;
        boundary_ = false;
        return true;
    }

    Event consume(uint8_t byte) {
        if (raw_) {
            // Some real devices keep echoing setters despite echo(0). Separate
            // bounded, recognized command ACKs from the raw mask stream. Only
            // CR/LF inside a valid ACK+prompt are text; standalone 10/13 are masks.
            constexpr std::string_view trailer = "\r\n>>> ";
            if (rawTrailerOffset_) {
                if (byte != static_cast<uint8_t>(trailer[rawTrailerOffset_ - 1]))
                    return {EventKind::ProtocolError, 0, {}};
                if (++rawTrailerOffset_ > trailer.size()) {
                    rawTrailerOffset_ = 0;
                    rawReplySize_ = 0;
                    if (framedMaskPending_) {
                        framedMaskPending_ = false;
                        return {EventKind::Button, framedMask_, {}};
                    }
                }
                return {};
            }
            if (rawReplySize_) {
                const std::string_view prefix(rawReply_, rawReplySize_);
                // Captured hardware uses km.<mask>; the legacy API also
                // documents km.buttons<mask>. Publish only after the full
                // trailer so framing bytes cannot become phantom presses.
                if (byte <= 31 && (prefix == "km." || prefix == "km.buttons")) {
                    framedMask_ = byte;
                    framedMaskPending_ = true;
                    rawTrailerOffset_ = 1;
                    return {};
                }
                if (byte == '\r' && isControlAck({rawReply_, rawReplySize_})) {
                    rawTrailerOffset_ = 2;
                    return {};
                }
                if (byte < 32 || byte > 126 || rawReplySize_ == sizeof(rawReply_))
                    return {EventKind::ProtocolError, 0, {}};
                rawReply_[rawReplySize_++] = static_cast<char>(byte);
                return {};
            }
            if (byte <= 31) return {EventKind::Button, byte, {}};
            if (byte == 'k') {
                rawReply_[0] = 'k';
                rawReplySize_ = 1;
                return {};
            }
            // Unknown text or another framed stream violates the handshake.
            // Fail the connection rather than interpreting its CR/LF as clicks.
            return {EventKind::ProtocolError, 0, {}};
        }

        boundary_ = false;
        if (prompt_) {
            constexpr char promptText[] = ">>> ";
            if (byte == static_cast<uint8_t>(promptText[prompt_])) {
                if (++prompt_ != 4) return {};
                prompt_ = 0;
                const bool terminated = afterLine_;
                afterLine_ = false;
                if (terminated && !candidate_.empty()) {
                    Event event{EventKind::QueryComplete, 0, std::move(candidate_)};
                    candidate_.clear();
                    query_.clear();
                    boundary_ = true;
                    return event;
                }
                return {}; // Startup/echo/old prompts never complete a query.
            }
            prompt_ = 0;
            afterLine_ = false;
            candidate_.clear();
            discardLine_ = true;
        }

        if (afterCR_) {
            afterCR_ = false;
            if (byte == '\n') {
                candidate_ = !discardLine_ && matches(line_) ? line_ : std::string{};
                line_.clear();
                discardLine_ = false;
                afterLine_ = true;
                return {};
            }
            // A raw CR left over before the stop-stream barrier, or malformed
            // text, cannot make an unterminated response eligible for a query.
            line_.clear();
            candidate_.clear();
            discardLine_ = false;
            afterLine_ = false;
        }
        if (byte == '\r') { afterCR_ = true; return {}; }
        if (byte == '\n') {
            line_.clear(); candidate_.clear(); discardLine_ = false; afterLine_ = false;
            return {};
        }
        if (byte < 32 || byte > 126) {
            // Text mode never emits button events. An interrupted text record
            // is rejected; the next well-formed CRLF record can resynchronize.
            if (!line_.empty()) discardLine_ = true;
            candidate_.clear();
            afterLine_ = false;
            return {};
        }
        if (line_.empty() && byte == '>') { prompt_ = 1; return {}; }
        afterLine_ = false;
        candidate_.clear();
        if (!discardLine_) {
            if (line_.size() < 512) line_.push_back(static_cast<char>(byte));
            else discardLine_ = true;
        }
        return {};
    }

private:
    static bool isControlAck(std::string_view line) {
        if (line.size() >= 11 && line.substr(0, 8) == "km.move(" && line.back() == ')') {
            const auto args = line.substr(8, line.size() - 9);
            const auto comma = args.find(',');
            if (comma == std::string_view::npos) return false;
            int32_t x, y;
            const auto a = std::from_chars(args.data(), args.data() + comma, x);
            const auto b = std::from_chars(args.data() + comma + 1, args.data() + args.size(), y);
            return a.ec == std::errc{} && b.ec == std::errc{} &&
                a.ptr == args.data() + comma && b.ptr == args.data() + args.size();
        }
        for (const auto command : {"km.buttons(0)", "km.buttons(1)",
            "km.left(0)", "km.left(1)", "km.right(0)", "km.right(1)",
            "km.middle(0)", "km.middle(1)", "km.side1(0)", "km.side1(1)",
            "km.side2(0)", "km.side2(1)"})
            if (line == command) return true;
        return false;
    }

    bool matches(std::string_view line) const {
        if (query_.empty() || line.empty() || line == query_) return false;
        if (query_ == "km.version()")
            return detail::identifyDevice(line) != detail::DeviceIdentity::Unknown;
        const std::string_view query(query_);
        const auto name = query.substr(3, query.size() - 5);
        const bool button = name == "left" || name == "right" || name == "middle" ||
            name == "side1" || name == "side2";
        if (button) return detail::parseButtonState(line, name) >= 0;
        if (line.size() == 1 && line[0] >= '0' && line[0] <= (button ? '3' : '1'))
            return button || name == "echo" || name == "buttons" || name == "moving";
        // Accept only a result carrying this exact command's name, never a
        // different command's ACK, a prompt, or km.buttons stream decoration.
        const auto prefix = query.substr(0, query.size() - 1);
        if (line.size() <= prefix.size() + 1 || line.substr(0, prefix.size()) != prefix || line.back() != ')') return false;
        const auto value = line.substr(prefix.size(), line.size() - prefix.size() - 1);
        if (button || name == "echo" || name == "buttons" || name == "moving")
            return value.size() == 1 && value[0] >= '0' && value[0] <= (button ? '3' : '1');
        return false; // Unsupported query shapes must not complete arbitrarily.
    }

    bool raw_ = false, boundary_ = false, afterCR_ = false, afterLine_ = false, discardLine_ = false;
    char rawReply_[64]{};
    size_t rawReplySize_ = 0;
    unsigned rawTrailerOffset_ = 0;
    bool framedMaskPending_ = false;
    uint8_t framedMask_ = 0;
    unsigned prompt_ = 0;
    std::string query_, candidate_, line_;
};

} // namespace makcu::protocol
