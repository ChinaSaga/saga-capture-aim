// Build with tools/build_performance.ps1 -Source tools/test_yolo_hotpath.cpp -Name test_yolo_hotpath.
// CPU-only regression for fused FP16 preprocessing and the TensorRT score scan.
#include "preprocessing.hpp"
#include "YoloScores.h"
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <tuple>

using Result = std::tuple<size_t, uint32_t, int>;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
#pragma float_control(precise, on, push)
static std::vector<Result> reference(const std::vector<float>& values, size_t count, int classes, float threshold) {
    std::vector<Result> result;
    for (size_t d = 0; d < count; ++d) {
        float best = values[4 * count + d];
        int label = 0;
        for (int c = 1; c < classes; ++c)
            if (values[(size_t(c) + 4) * count + d] > best) {
                best = values[(size_t(c) + 4) * count + d]; label = c;
            }
        const auto bits = std::bit_cast<uint32_t>(best);
        if ((bits & 0x7f800000u) != 0x7f800000u && best > threshold)
            result.emplace_back(d, bits, label);
    }
    return result;
}
#pragma float_control(pop)
static void scores() {
    std::mt19937 rng(20261004);
    const uint32_t edges[]{0, 0x80000000u, 0x7fc00000u, 0x7f800000u,
                          0xff800000u, 0x3e800000u, 0x3e800001u, 0x3e7fffffu};
    size_t cases = 0;
    for (int classes : {1, 2, 7, 80, 4096})
        for (size_t count : {size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(31), size_t(32), size_t(33), size_t(3549)}) {
            std::vector<float> values((size_t(classes) + 4) * count);
            for (auto& v : values) v = float(rng() % 1000) / 1000.f;
            for (size_t i = 4 * count; i < values.size(); ++i)
                if (rng() % 5 == 0)
                    values[i] = std::bit_cast<float>(edges[rng() % std::size(edges)]);
            for (float threshold : {-.1f, 0.f, .25f, .5f, 1.f}) {
                std::vector<Result> actual;
                forEachYoloCandidate(values.data(), count, classes, threshold,
                    [&](size_t d, float s, int c) { actual.emplace_back(d, std::bit_cast<uint32_t>(s), c); });
                const auto expected = reference(values, count, classes, threshold);
                if (actual != expected) {
                    std::cerr << "classes=" << classes << " count=" << count << " threshold=" << threshold << " actual=" << actual.size() << " expected=" << expected.size() << '\n';
                    require(false, "Score scan changed order, class or score bits");
                }
                ++cases;
            }
        }
    std::cout << "Score scan: " << cases << " cases match scalar reference, including tails, ties and nonfinite values.\n";
}
static void pixels() {
    using yolos::preprocessing::InferenceBuffer;
    using yolos::preprocessing::letterBoxToBlob;
    InferenceBuffer owned, external, half;
    std::vector<float> a(3 * 640 * 640 + 16, -777.f), b(a);
    std::vector<uint16_t> ha(a.size(), 0xdead), hb(ha), expectedHalf(a.size());
    size_t cases = 0;
    for (int channels : {3, 1}) for (bool dynamic : {false, true})
        for (cv::Size target : {cv::Size(416,416), cv::Size(640,640)})
            for (cv::Size shape : {cv::Size(416,416), cv::Size(320,320), cv::Size(831,419), cv::Size(417,832), cv::Size(1920,1080)}) {
                // ROI deliberately has a non-contiguous stride and unaligned start.
                cv::Mat parent(shape.height + 2, shape.width + 5, CV_MAKETYPE(CV_8U, channels));
                cv::randu(parent, 0, 256);
                cv::Mat input = parent(cv::Rect(1, 1, shape.width, shape.height));
                for (int repeat = 0; repeat < 3; ++repeat) {
                    cv::Size expectedSize, actualSize;
                    letterBoxToBlob(input, owned, channels, target, expectedSize, dynamic);
                    auto& destination = repeat == 1 ? b : a;
                    const size_t n = size_t(expectedSize.area()) * channels;
                    std::fill(destination.begin() + n, destination.end(), -777.f);
                    letterBoxToBlob(input, external, channels, target, actualSize, dynamic,
                                   std::span<float>(destination.data(), n));
                    require(actualSize == expectedSize, "External output shape mismatch");
                    require(!std::memcmp(destination.data(), owned.blob.data(), n * sizeof(float)), "External output changed RGB/gray/padding bits");
                    require(std::all_of(destination.begin() + n, destination.end(), [](float x) { return x == -777.f; }), "Output overrun");
                    require(external.blob.empty(), "External path allocated a redundant blob");
                    size_t i = 0;
                    for (; i + 8 <= n; i += 8)
                        _mm_storeu_si128(reinterpret_cast<__m128i*>(expectedHalf.data() + i),
                            _mm256_cvtps_ph(_mm256_loadu_ps(owned.blob.data() + i), 0));
                    for (; i < n; ++i)
                        expectedHalf[i] = uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(owned.blob[i]), 0), 0));
                    auto& halfDestination = repeat == 1 ? hb : ha;
                    std::fill(halfDestination.begin() + n, halfDestination.end(), 0xdead);
                    letterBoxToBlob(input, half, channels, target, actualSize, dynamic,
                                   std::span<uint16_t>(halfDestination.data(), n));
                    require(!std::memcmp(halfDestination.data(), expectedHalf.data(), n * sizeof(uint16_t)), "Fused FP16 differs from original two-pass conversion");
                    require(std::all_of(halfDestination.begin() + n, halfDestination.end(), [](uint16_t x) { return x == 0xdead; }), "FP16 output overrun");
                    require(half.blob.empty(), "FP16 path allocated float scratch");
                    ++cases;
                }
            }
    cv::Mat input(416,416,CV_8UC3,cv::Scalar(2,17,250));
    cv::Size actual;
    bool rejected = false;
    try { letterBoxToBlob(input, external, 3, input.size(), actual, false, std::span<float>(a.data(), 1)); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Undersized destination was not rejected");
    // Switching back to owned output must also invalidate the padding cache.
    input = cv::Mat(361,640,CV_8UC3,cv::Scalar(2,17,250));
    letterBoxToBlob(input, external, 3, cv::Size(416,416), actual);
    letterBoxToBlob(input, owned, 3, cv::Size(416,416), actual);
    require(!std::memcmp(external.blob.data(), owned.blob.data(), 3*416*416*sizeof(float)), "Switch back to owned output failed");
    // Exercise every byte value and every SIMD tail at exact-size input.
    for (int width = 1; width <= 33; ++width) {
        cv::Mat parent(256, width + 2, CV_8UC3);
        cv::Mat image = parent(cv::Rect(1,0,width,256));
        for (int y=0; y<256; ++y) for (int x=0; x<width; ++x)
            image.at<cv::Vec3b>(y,x) = cv::Vec3b(y,255-y,(x+y)%256);
        letterBoxToBlob(image, owned, 3, image.size(), actual);
        letterBoxToBlob(image, half, 3, image.size(), actual, false,
                       std::span<uint16_t>(ha.data(), size_t(width)*256*3));
        for (size_t i=0; i<size_t(width)*256*3; ++i) {
            const auto expected = uint16_t(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(owned.blob[i]),0),0));
            require(ha[i] == expected, "FP16 pixel/tail mismatch");
        }
    }
    std::cout << "Direct FP32/FP16 output: " << cases << " bit-identical layouts; ROI strides, all pixel values, SIMD tails, alternating destinations, guards and capacity checks passed.\n";
}
int main() {
    try { scores(); pixels(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
