#include "HttpRequest.h"
#include "RuntimeSettings.h"
#include "EncodedImage.h"
#include "UdpFrameAssembler.h"
#include "ConfigValue.h"
#include "FileIO.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>

int main() {
    using http_request::State;
    const std::string header = "POST /api HTTP/1.1\r\nHost: localhost\r\nCoNtEnT-LeNgTh: 3\r\n\r\n";
    for (size_t length = 0; length < header.size() + 3; ++length)
        assert(http_request::inspect((header + "abc").substr(0, length)).state == State::incomplete);
    assert(http_request::inspect(header + "abc").state == State::complete);
    assert(http_request::inspect(header + "abcd").state == State::invalid);
    for (const auto value : {"-1", "x", "3junk", "184467440737095516160"})
        assert(http_request::inspect(std::string("POST / HTTP/1.1\r\nContent-Length: ") + value + "\r\n\r\n").state == State::invalid);
    assert(http_request::inspect("POST / HTTP/1.1\r\nContent-Length: 65537\r\n\r\n").state == State::tooLarge);
    assert(http_request::inspect(std::string(16384, 'x')).state == State::tooLarge);
    assert(http_request::inspect("POST / HTTP/1.1\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n").state == State::invalid);
    assert(http_request::inspect("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").state == State::invalid);

    std::vector<unsigned char> bmp(70, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    auto put = [&](size_t offset, int value) { std::memcpy(bmp.data() + offset, &value, 4); };
    put(10, 54); put(14, 40); put(18, 2); put(22, -2); bmp[26] = 1; bmp[28] = 24;
    encoded_image::BgrView view;
    assert(encoded_image::topDownBgr(bmp.data(), bmp.size(), view) && view.stride == 8 && view.height == 2);
    assert(!encoded_image::topDownBgr(bmp.data(), bmp.size()-1, view));
    put(18, INT_MAX); assert(!encoded_image::topDownBgr(bmp.data(), bmp.size(), view));
    put(18, 2); put(22, INT_MIN); assert(!encoded_image::topDownBgr(bmp.data(), bmp.size(), view));
    put(22, -2); put(10, 14); assert(!encoded_image::topDownBgr(bmp.data(), bmp.size(), view));

    UdpFrameAssembler assembler;
    std::vector<unsigned char> packet(1472, 7), frame;
    packet[0] = 0xff; packet[1] = 0xd8; packet[1470] = 0xff; packet[1471] = 0xd9;
    assert(assembler.push(packet.data(), packet.size(), 1, 10, frame) && frame == packet);
    packet.assign(1472, 7);
    assert(!assembler.push(packet.data(), packet.size(), 1, 20, frame));
    const unsigned char tail[]{1, 2, 3};
    assert(assembler.push(tail, 3, 2, 21, frame) && frame.size() == 3); // Peer cannot append to another image.
    for (size_t i = 0; i < UdpFrameAssembler::maxBytes / 1472 + 2; ++i)
        assert(!assembler.push(packet.data(), packet.size(), 1, 30, frame));
    assert(!assembler.push(tail, 3, 1, 31, frame)); // Discard oversized frame through its delimiter.
    assert(assembler.push(tail, 3, 1, 32, frame) && frame.size() == 3);

    RuntimeSettingsStore store;
    std::atomic<bool> done{false};
    std::thread writer([&] { for (int i=1; i<=100000; ++i) { RuntimeSettings s; s.lockFreq=i; s.delayFreq=-i; store.replace(s); } done=true; });
    do { const auto s=store.snapshot(); assert((s.lockFreq == 100 && s.delayFreq == 200) || s.lockFreq == -s.delayFreq); } while (!done);
    writer.join();
    assert(config_value::integer("2147483648") == 0 && config_value::integer("42x") == 0);
    for (const auto value : {"nan", "inf", "-inf", "1e999"}) assert(config_value::number(value) == 0);
    assert(config_value::number("0.618") == .618f);
    for (const auto name : {"../escape", "CON", "LPT1.txt", "x.", "x\ny", "a:b"}) assert(!config_value::leafName(name));
    assert(config_value::leafName("中文模型"));

    const auto dir = std::filesystem::temp_directory_path() / (L"saga-atomic-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(dir));
    const auto path = dir / L"配置.js";
    assert(file_io::write(path, "original"));
    HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(held != INVALID_HANDLE_VALUE);
    assert(!file_io::write(path, "replacement"));
    CloseHandle(held);
    assert(file_io::read(path) == "original");
    assert(file_io::write(path, "replacement") && file_io::read(path) == "replacement");
    assert(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator{}) == 1);
    std::filesystem::remove_all(dir);
    std::cout << "PASS HTTP fragmentation/limits, BMP overflow, UDP bounds, coherent settings, numeric inputs and atomic save failure\n";
}
