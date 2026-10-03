#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <immintrin.h>

inline void storeTensorPixels(float* out, __m256 values) {
    _mm256_storeu_ps(out, values);
}
inline void storeTensorPixels(uint16_t* out, __m256 values) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm256_cvtps_ph(values, 0));
}
template<class T> inline T tensorPixel(float value) {
    if constexpr (std::is_same_v<T, float>) return value;
    else {
        static_assert(std::is_same_v<T, uint16_t>);
        return static_cast<uint16_t>(_mm_extract_epi16(_mm_cvtps_ph(_mm_set_ss(value), 0), 0));
    }
}

// Fuse BGR deinterleave, RGB channel order and normalization into one pass.
// Plane pointers also support ncnn's padded channel stride.
template<class T>
inline void bgrToRgbPlanes(const unsigned char* pixels, int width, int height,
    size_t sourceStride, T* r, T* g, T* b, int targetStride) {
    constexpr float norm = 1.0f / 255.0f;
    const __m256 scale = _mm256_set1_ps(norm);
    const __m256i bm = _mm256_broadcastsi128_si256(_mm_setr_epi8(0,-1,-1,-1,3,-1,-1,-1,6,-1,-1,-1,9,-1,-1,-1));
    const __m256i gm = _mm256_broadcastsi128_si256(_mm_setr_epi8(1,-1,-1,-1,4,-1,-1,-1,7,-1,-1,-1,10,-1,-1,-1));
    const __m256i rm = _mm256_broadcastsi128_si256(_mm_setr_epi8(2,-1,-1,-1,5,-1,-1,-1,8,-1,-1,-1,11,-1,-1,-1));
    // A tight image and tight planes are one contiguous run. This also removes
    // per-row SIMD tails on odd widths without reading source padding.
    if (sourceStride == static_cast<size_t>(width) * 3 && targetStride == width) {
        width *= height;
        height = 1;
    }
    for (int y = 0; y < height; ++y) {
        const auto* src = pixels + y * sourceStride;
        int x = 0;
        for (; x + 8 <= width; x += 8) {
            // Exactly 24 readable bytes are sufficient, including the final
            // row: the overlapping high load ends at pixel 7, not pixel 9.
            const auto* p = reinterpret_cast<const __m128i*>(src + x * 3);
            const __m128i lo = _mm_loadu_si128(p);
            const __m128i hi = _mm_srli_si128(_mm_loadu_si128(
                reinterpret_cast<const __m128i*>(src + x * 3 + 8)), 4);
            const __m256i v = _mm256_set_m128i(hi, lo);
            storeTensorPixels(r + x, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_shuffle_epi8(v, rm)), scale));
            storeTensorPixels(g + x, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_shuffle_epi8(v, gm)), scale));
            storeTensorPixels(b + x, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_shuffle_epi8(v, bm)), scale));
        }
        for (; x < width; ++x) {
            r[x] = tensorPixel<T>(src[x * 3 + 2] * norm);
            g[x] = tensorPixel<T>(src[x * 3 + 1] * norm);
            b[x] = tensorPixel<T>(src[x * 3] * norm);
        }
        r += targetStride; g += targetStride; b += targetStride;
    }
}
