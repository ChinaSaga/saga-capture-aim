// Pure protocol regression test: no serial ports or hardware commands.
#include "../src/Makcu/include/serialprotocol.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using makcu::protocol::EventKind;
using makcu::protocol::Parser;
static int failures = 0;
static void check(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
static Parser streaming() {
    Parser p;
    check(p.beginQuery("km.version()"), "identity query starts");
    bool complete = false;
    for (const unsigned char b : std::string("MAKCU v4.036\r\n>>> "))
        if (p.consume(b).kind == EventKind::QueryComplete) complete = true;
    check(complete && p.enterRawButtons(), "identity handshake enters stream");
    return p;
}
static std::string frame(unsigned mask, bool legacy = false, bool longPrefix = false) {
    std::string s = longPrefix ? "km.buttons" : "km.";
    s.push_back(static_cast<char>(mask));
    s += "\r\n";
    if (legacy) s += ">>> ";
    return s;
}
static std::string decoratedFrame(unsigned mask, bool headerCRLF = false, bool prompt = true) {
    std::string s = headerCRLF ? "km.buttons()\r\n" : "km.buttons()\n";
    s.push_back(static_cast<char>(mask));
    s += "\r\n";
    if (prompt) s += ">>> ";
    return s;
}
static void feed(Parser& p, std::string_view bytes, std::vector<unsigned>& masks, int& errors) {
    for (const unsigned char b : bytes) {
        auto e = p.consume(b);
        if (e.kind == EventKind::Button) masks.push_back(e.buttonMask);
        if (e.kind == EventKind::ProtocolError) ++errors;
    }
}
static void valid(const std::string& bytes, const std::vector<unsigned>& expected, const char* name) {
    // Each possible two-chunk split, including empty chunks. Parser state must
    // survive ReadFile boundaries; feed always delivers individual bytes.
    for (size_t split = 0; split <= bytes.size(); ++split) {
        auto p = streaming(); std::vector<unsigned> masks; int errors = 0;
        feed(p, std::string_view(bytes).substr(0, split), masks, errors);
        feed(p, std::string_view(bytes).substr(split), masks, errors);
        check(errors == 0 && masks == expected, name);
    }
}
int main() {
    // Official asynchronous frame must publish on LF, without waiting for >>>.
    {
        auto p = streaming(); const auto s = frame(1);
        for (size_t i = 0; i + 1 < s.size(); ++i)
            check(p.consume(static_cast<unsigned char>(s[i])).kind == EventKind::None,
                  "framed mask waits for complete CRLF");
        auto e = p.consume('\n');
        check(e.kind == EventKind::Button && e.buttonMask == 1, "official frame completes at LF");
    }
    std::string official, legacy, raw, longFrames;
    std::vector<unsigned> everyMask;
    for (unsigned mask = 0; mask <= 31; ++mask) {
        everyMask.push_back(mask);
        official += frame(mask); legacy += frame(mask, true);
        longFrames += frame(mask, true, true);
        raw.push_back(static_cast<char>(mask));
    }
    valid(frame(1) + frame(0) + frame(2) + frame(0), {1, 0, 2, 0}, "consecutive left/right press/release");
    valid(official, everyMask, "official frames: all masks including CR/LF");
    valid(raw, everyMask, "raw masks: all masks including CR/LF");
    valid(legacy, everyMask, "legacy framed prompts produce no extra masks");
    valid(longFrames, everyMask, "legacy long prefix produces no extra masks");
    const std::string acks = "km.move(-12,34)\r\n>>> km.left(1)\r\n>>> km.left(0)\r\n>>> "
                             "km.right(1)\r\n>>> km.buttons(1)\r\n>>> km.buttons(0)\r\n>>> ";
    valid(acks, {}, "recognized setter ACKs do not produce buttons");
    valid(frame(1) + acks + frame(0), {1, 0}, "frames interleaved with setter ACKs");
    // Exact bytes captured from the user's device: the query-looking header
    // is decoration on a binary state report, not a new outstanding query.
    valid(decoratedFrame(2), {2}, "captured km.buttons() LF binary right-button report");
    valid(decoratedFrame(1) + decoratedFrame(0) + decoratedFrame(2) + decoratedFrame(0),
          {1, 0, 2, 0}, "decorated left/right press and release");
    std::string decorated, decoratedCRLF, decoratedNoPrompt, decoratedCRLFNoPrompt;
    for (unsigned mask = 0; mask <= 31; ++mask) {
        decorated += decoratedFrame(mask);
        decoratedCRLF += decoratedFrame(mask, true);
        decoratedNoPrompt += decoratedFrame(mask, false, false);
        decoratedCRLFNoPrompt += decoratedFrame(mask, true, false);
    }
    valid(decorated, everyMask, "decorated LF header all masks including CR/LF");
    valid(decoratedCRLF, everyMask, "decorated CRLF header all masks including CR/LF");
    valid(decoratedNoPrompt, everyMask, "decorated frames complete without prompt");
    valid(decoratedCRLFNoPrompt, everyMask, "decorated CRLF headers complete without prompt");
    valid(decoratedFrame(2) + acks + frame(1) + std::string(1, char(0)),
          {2, 1, 0}, "decorated, ACK, prefixed and raw frames interleave");
    {
        auto p = streaming(); const auto bytes = decoratedFrame(2, false, false);
        for (size_t i = 0; i + 1 < bytes.size(); ++i)
            check(p.consume(static_cast<unsigned char>(bytes[i])).kind == EventKind::None,
                  "decorated header LF and mask never publish incomplete frame");
        auto e = p.consume('\n');
        check(e.kind == EventKind::Button && e.buttonMask == 2, "decorated report completes at final LF");
    }
    for (const auto& bad : {std::string("unexpected\r\n>>> "),
                            std::string("km.buttons()\rX"),
                            std::string("km.buttons()\n2\r\n>>> "),
                            std::string("km.buttons()\n") + char(32) + "\r\n>>> ",
                            std::string("km.buttons()\n") + char(2) + "\rX",
                            std::string("km.left()\n") + char(1) + "\r\n>>> ",
                            std::string("km.") + char(1) + "\n\r",
                            std::string("km.") + char(2) + "\rX"}) {
        for (size_t split = 0; split <= bad.size(); ++split) {
            auto p = streaming(); std::vector<unsigned> masks; int errors = 0;
            // Production stops consuming upon first ProtocolError, so do the
            // same: later CR/LF after rejection are never interpreted as masks.
            auto part = [&](std::string_view s) {
                for (const unsigned char b : s) {
                    if (errors) break;
                    auto e = p.consume(b);
                    if (e.kind == EventKind::Button) masks.push_back(e.buttonMask);
                    if (e.kind == EventKind::ProtocolError) ++errors;
                }
            };
            part(std::string_view(bad).substr(0, split));
            part(std::string_view(bad).substr(split));
            check(errors == 1 && masks.empty(), "malformed text/framing rejects without phantom buttons");
        }
    }
    if (failures) { std::fprintf(stderr, "%d regression checks failed\n", failures); return EXIT_FAILURE; }
    std::puts("PASS: MAKCU official/legacy framing, raw masks, ACKs, split boundaries and malformed data");
    return EXIT_SUCCESS;
}
