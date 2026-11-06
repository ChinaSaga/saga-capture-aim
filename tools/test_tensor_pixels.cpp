#include "../src/TensorPixels.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>

// Build with cl /O2 /arch:AVX2 /EHsc tools/test_tensor_pixels.cpp.
// Exact-size source allocations exercise SIMD tails; sentinel destination
// padding checks that conversion respects both source and destination strides.
int main() {
    for (int width : {1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 319, 320, 321, 640}) {
        for (int padding : {0, 1, 5, 31}) {
            constexpr int height = 37;
            const int stride = width + padding;
            const size_t sourceStride = size_t(width) * 3 + padding;
            std::vector<unsigned char> pixels(sourceStride * height);
            for (size_t i = 0; i < pixels.size(); ++i)
                pixels[i] = static_cast<unsigned char>(i * 17 + 53);
            const size_t plane = size_t(stride) * height;
            std::vector<float> expected(plane * 3, -1.0f), actual = expected;
            constexpr float normalization = 1.0f / 255.0f;
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < 3; ++c)
                        expected[plane * c + y * stride + x] =
                            pixels[y * sourceStride + x * 3 + 2 - c] * normalization;
            bgrToRgbPlanes(pixels.data(), width, height, sourceStride,
                actual.data(), actual.data() + plane, actual.data() + 2 * plane, stride);
            if (actual != expected) {
                std::cerr << "conversion mismatch: width=" << width << " padding=" << padding << '\n';
                return 1;
            }
        }
    }
    std::cout << "Exact scalar equivalence and row-padding preservation passed (56 layouts).\n";
    for (int width : {320, 640, 1280}) {
        const size_t plane = size_t(width) * width;
        std::vector<unsigned char> pixels(plane * 3, 117);
        std::vector<float> tensor(plane * 3);
        std::vector<double> measurements;
        for (int repeat = 0; repeat < 7; ++repeat) {
            const auto start = std::chrono::steady_clock::now();
            for (int iteration = 0; iteration < 1000; ++iteration)
                bgrToRgbPlanes(pixels.data(), width, width, size_t(width) * 3,
                    tensor.data(), tensor.data() + plane, tensor.data() + plane * 2, width);
            measurements.push_back(std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count() / 1000);
        }
        std::sort(measurements.begin(), measurements.end());
        std::cout << width << 'x' << width << " median conversion: " << measurements[3] << " us\n";
        // Match the kernel's runtime multiplication; /fp:fast may fold the
        // literal expression to a different last bit.
        volatile float expectedByte = 117.f;
        if (tensor[0] != expectedByte * (1.0f / 255.0f)) return 1;
    }
}
