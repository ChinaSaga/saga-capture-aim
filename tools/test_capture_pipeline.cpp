// Standalone capture regression / benchmark (does not initialize inference or mouse).
// From an x64 Visual Studio Developer Prompt, repository root:
// cl /O2 /Ob3 /arch:AVX2 /std:c++17 /EHsc /source-charset:utf-8 /execution-charset:.936 tools/test_capture_pipeline.cpp /Fe:.workbuddy/capture-perf/test_capture_pipeline.exe
// Modes: --validate (default), --cpu, --list, --live [device [width height format fps crop seconds]].
// Live mode opens the selected capture device; close the main app first.
#include "../src/OpnecvCapture.cpp"
#include <chrono>
#include <vector>
#include <random>
#include <algorithm>
#include <cstdlib>

namespace {
using Clock = std::chrono::steady_clock;
volatile unsigned checksum;

int validate() {
    std::mt19937 random(13);
    unsigned cases = 0;
    for (int ten = 0; ten < 2; ++ten) {
        for (int swap = 0; swap < (ten ? 1 : 2); ++swap) {
            for (int left : {0, 1, 6, 7}) {
                for (int width = 1; width <= 640; ++width) {
                    std::vector<BYTE> y0(1400), y1(1400), uv(1400);
                    std::vector<BYTE> a(width * 3 + 16, 42), b(a.size(), 42), reference(a.size(), 42);
                    for (auto& value : y0) value = static_cast<BYTE>(random());
                    for (auto& value : y1) value = static_cast<BYTE>(random());
                    for (auto& value : uv) value = static_cast<BYTE>(random());
                    capture_pixels::Coefficients c;
                    if (ten) {
                        c.yOffset = 64; c.uvOffset = 512; c.y = 19077;
                        c.redV = 26149; c.greenU = -6419; c.greenV = -13320; c.blueU = 33050;
                    }
                    if (ten) capture_pixels::p010_two_rows(y0.data(), y1.data(), uv.data(), left, width, a.data(), b.data(), c);
                    else if (swap) capture_pixels::nv21_two_rows(y0.data(), y1.data(), uv.data(), left, width, a.data(), b.data(), c);
                    else capture_pixels::nv12_two_rows(y0.data(), y1.data(), uv.data(), left, width, a.data(), b.data(), c);
                    for (int row = 0; row < 2; ++row) {
                        const auto* y = row ? y1.data() : y0.data();
                        const auto* output = row ? b.data() : a.data();
                        for (int x = 0; x < width; ++x) {
                            const int col = left + x;
                            const auto* pair = uv.data() + (col & ~1) * (ten ? 2 : 1);
                            const int yy = ten ? capture_pixels::detail::p010(y + col * 2) : y[col];
                            const int u = ten ? capture_pixels::detail::p010(pair) : pair[swap ? 1 : 0];
                            const int v = ten ? capture_pixels::detail::p010(pair + 2) : pair[swap ? 0 : 1];
                            capture_pixels::detail::pixel(yy, u, v, reference.data() + x * 3, c);
                        }
                        // Includes the untouched 16-byte sentinel after each output row.
                        if (std::memcmp(output, reference.data(), reference.size())) {
                            std::printf("FAIL format=%d swap=%d left=%d width=%d row=%d\n", ten, swap, left, width, row);
                            return 1;
                        }
                    }
                    ++cases;
                }
            }
        }
    }
    VIDEOINFOHEADER info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1920; info.bmiHeader.biHeight = 1080;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 12;
    info.bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
    AM_MEDIA_TYPE type{};
    type.majortype = MEDIATYPE_Video; type.subtype = MEDIATYPE_Video;
    type.subtype.Data1 = info.bmiHeader.biCompression;
    type.formattype = FORMAT_VideoInfo; type.pbFormat = reinterpret_cast<BYTE*>(&info); type.cbFormat = sizeof(info);
    FrameReceiver receiver;
    if (!receiver.configure(type, 1920, 1080, 320)) return 2;
    std::vector<BYTE> sample(1920 * 1080 * 3 / 2);
    const auto sequence = g_captureSequence;
    receiver.BufferCB(0, nullptr, static_cast<long>(sample.size()));
    receiver.BufferCB(0, sample.data(), static_cast<long>(sample.size()) - 1);
    receiver.BufferCB(0, sample.data(), -1);
    if (g_captureSequence != sequence) return 3;
    receiver.BufferCB(0, sample.data(), static_cast<long>(sample.size()));
    if (g_captureSequence != sequence + 1) return 4;
    receiver.clear();
    std::printf("PASS %u scalar-exact planar cases, output sentinels and malformed frame publication checks\n", cases);
    return 0;
}

__declspec(noinline) void convert(const BYTE* y, const BYTE* uv, BYTE* output, int crop, int loops) {
    const int left = (1920 - crop) / 2;
    for (int i = 0; i < loops; ++i) {
        for (int row = 0; row < crop; row += 2)
            capture_pixels::nv12_two_rows(y + row * 1920, y + (row + 1) * 1920, uv + row / 2 * 1920,
                left, crop, output + row * crop * 3, output + (row + 1) * crop * 3, {});
    }
    checksum = output[123];
}

int cpu() {
    std::vector<BYTE> y(1920 * 1080), uv(1920 * 540), output(640 * 640 * 3);
    std::mt19937 random(42);
    for (auto& value : y) value = static_cast<BYTE>(random());
    for (auto& value : uv) value = static_cast<BYTE>(random());
    for (int crop : {160, 320, 640}) {
        convert(y.data(), uv.data(), output.data(), crop, 100);
        std::vector<double> times;
        for (int trial = 0; trial < 7; ++trial) {
            auto start = Clock::now();
            convert(y.data(), uv.data(), output.data(), crop, 2000);
            times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count() / 2000);
        }
        std::sort(times.begin(), times.end());
        std::printf("NV12 crop=%d median_us=%.3f min_us=%.3f max_us=%.3f (conversion only, warm memory)\n",
            crop, times[3], times.front(), times.back());
    }
    return 0;
}

int live(int argc, char** argv) {
    const char* device = argc > 2 ? argv[2] : "Live Gamer Ultra 2.1-Video";
    int width = argc > 3 ? std::atoi(argv[3]) : 1920;
    int height = argc > 4 ? std::atoi(argv[4]) : 1080;
    const char* format = argc > 5 ? argv[5] : "NV12";
    int fps = argc > 6 ? std::atoi(argv[6]) : 240;
    int crop = argc > 7 ? std::atoi(argv[7]) : 320;
    int seconds = argc > 8 ? std::atoi(argv[8]) : 8;
    if (seconds < 1 || seconds > 3600) return 2;
    const char* result = saga::setcjk_ex(device, width, height, format, fps, crop);
    if (std::strcmp(result, "成功") != 0) {
        std::printf("FAIL capture start: %s\n", result);
        return 3;
    }
    CaptureFrame frame;
    unsigned long long sequence = 0, first = 0;
    unsigned count = 0;
    const auto start = Clock::now();
    while (Clock::now() - start < std::chrono::seconds(seconds)) {
        if (captureAcquire(frame, sequence, 100)) {
            if (!first) first = sequence;
            ++count;
            frame = {}; // Release each lease immediately; do not copy or process pixels.
        }
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    saga::setcjk_ex(nullptr, 0, 0, nullptr, 0, 0);
    const unsigned long long published = first ? sequence - first + 1 : 0;
    std::printf("device=%s format=%s %dx%d requested_fps=%d crop=%d frames=%u published=%llu skipped=%llu seconds=%.3f observed_fps=%.3f\n",
        device, format, width, height, fps, crop, count, published, published - count, elapsed, count / elapsed);
    return count ? 0 : 4;
}
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "--validate";
    if (std::strcmp(mode, "--validate") == 0) return validate();
    if (std::strcmp(mode, "--cpu") == 0) return cpu();
    if (std::strcmp(mode, "--live") == 0) return live(argc, argv);
    if (std::strcmp(mode, "--list") == 0) {
        std::printf("devices: %s\n", saga::enum_video_devices());
        if (argc > 2) std::printf("formats: %s\n", saga::enum_device_formats(argv[2]));
        return 0;
    }
    std::printf("Modes: --validate | --cpu | --list [device] | --live [device [width height format fps crop seconds]]\n");
    return 2;
}
