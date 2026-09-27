#pragma once

#include <immintrin.h>
#include <cstdint>
#include <cstring>

// The DLL targets AVX2 machines. All converters preserve the original 16.16
// coefficients and rounding exactly; no floating-point approximation is used.
namespace capture_pixels {

struct Coefficients {
    int yOffset = 16, uvOffset = 128;
    int y = 76309, redV = 104597, greenU = -25675, greenV = -53279, blueU = 132201;
};

namespace detail {

__forceinline unsigned char channel(int value) {
    const int rounded = (value + 32768) >> 16;
    return static_cast<unsigned char>(rounded < 0 ? 0 : (rounded > 255 ? 255 : rounded));
}

__forceinline void pixel(int y, int u, int v, unsigned char* output, const Coefficients& c) {
    const int yy = (y > c.yOffset ? y - c.yOffset : 0) * c.y;
    u -= c.uvOffset;
    v -= c.uvOffset;
    output[0] = channel(yy + u * c.blueU);
    output[1] = channel(yy + u * c.greenU + v * c.greenV);
    output[2] = channel(yy + v * c.redV);
}

__forceinline unsigned p010(const unsigned char* source) {
    return *reinterpret_cast<const uint16_t*>(source) >> 6;
}

__forceinline __m256i pack_bgr(__m256i b, __m256i g, __m256i r) {
    b = _mm256_srai_epi32(b, 16);
    g = _mm256_srai_epi32(g, 16);
    r = _mm256_srai_epi32(r, 16);
    const __m256i bg = _mm256_packs_epi32(b, g);
    const __m256i rr = _mm256_packs_epi32(r, r);
    const __m256i bytes = _mm256_packus_epi16(bg, rr);
    return _mm256_shuffle_epi8(bytes, _mm256_setr_epi8(
        0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11, -1, -1, -1, -1,
        0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11, -1, -1, -1, -1));
}

__forceinline void store_eight(__m256i packed, unsigned char* output) {
    const __m128i low = _mm256_castsi256_si128(packed);
    const __m128i high = _mm256_extracti128_si256(packed, 1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output), _mm_or_si128(low, _mm_slli_si128(high, 12)));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(output + 16), _mm_srli_si128(high, 4));
}

// a contains pixels 0..3/8..11, d contains pixels 4..7/12..15. Join the
// four 12-byte groups directly into three full stores instead of permuting
// two 8-pixel vectors and issuing four mixed-width stores.
__forceinline void store_sixteen(__m256i a, __m256i d, unsigned char* output) {
    const __m128i a0 = _mm256_castsi256_si128(a);
    const __m128i d0 = _mm256_castsi256_si128(d);
    const __m128i a1 = _mm256_extracti128_si256(a, 1);
    const __m128i d1 = _mm256_extracti128_si256(d, 1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output), _mm_or_si128(a0, _mm_slli_si128(d0, 12)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 16),
        _mm_or_si128(_mm_srli_si128(d0, 4), _mm_slli_si128(a1, 8)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output + 32),
        _mm_or_si128(_mm_srli_si128(a1, 8), _mm_slli_si128(d1, 4)));
}

__forceinline void eight(__m256i y, __m256i u, __m256i v, unsigned char* output,
    const Coefficients& c) {
    y = _mm256_max_epi32(_mm256_sub_epi32(y, _mm256_set1_epi32(c.yOffset)), _mm256_setzero_si256());
    y = _mm256_add_epi32(_mm256_mullo_epi32(y, _mm256_set1_epi32(c.y)), _mm256_set1_epi32(32768));
    u = _mm256_sub_epi32(u, _mm256_set1_epi32(c.uvOffset));
    v = _mm256_sub_epi32(v, _mm256_set1_epi32(c.uvOffset));
    store_eight(pack_bgr(
        _mm256_add_epi32(y, _mm256_mullo_epi32(u, _mm256_set1_epi32(c.blueU))),
        _mm256_add_epi32(y, _mm256_add_epi32(
            _mm256_mullo_epi32(u, _mm256_set1_epi32(c.greenU)),
            _mm256_mullo_epi32(v, _mm256_set1_epi32(c.greenV)))),
        _mm256_add_epi32(y, _mm256_mullo_epi32(v, _mm256_set1_epi32(c.redV)))), output);
}

__forceinline void sixteen(__m256i y, __m256i u, __m256i v, unsigned char* output,
    const Coefficients& c) {
    y = _mm256_subs_epu16(y, _mm256_set1_epi16(static_cast<short>(c.yOffset)));
    __m256i ya = _mm256_unpacklo_epi16(y, _mm256_setzero_si256());
    __m256i yb = _mm256_unpackhi_epi16(y, _mm256_setzero_si256());
    ya = _mm256_add_epi32(_mm256_mullo_epi32(ya, _mm256_set1_epi32(c.y)), _mm256_set1_epi32(32768));
    yb = _mm256_add_epi32(_mm256_mullo_epi32(yb, _mm256_set1_epi32(c.y)), _mm256_set1_epi32(32768));
    u = _mm256_sub_epi32(u, _mm256_set1_epi32(c.uvOffset));
    v = _mm256_sub_epi32(v, _mm256_set1_epi32(c.uvOffset));
    const __m256i b = _mm256_mullo_epi32(u, _mm256_set1_epi32(c.blueU));
    const __m256i r = _mm256_mullo_epi32(v, _mm256_set1_epi32(c.redV));
    const __m256i g = _mm256_add_epi32(_mm256_mullo_epi32(u, _mm256_set1_epi32(c.greenU)),
        _mm256_mullo_epi32(v, _mm256_set1_epi32(c.greenV)));
    const __m256i a = pack_bgr(
        _mm256_add_epi32(ya, _mm256_unpacklo_epi32(b, b)),
        _mm256_add_epi32(ya, _mm256_unpacklo_epi32(g, g)),
        _mm256_add_epi32(ya, _mm256_unpacklo_epi32(r, r)));
    const __m256i d = pack_bgr(
        _mm256_add_epi32(yb, _mm256_unpackhi_epi32(b, b)),
        _mm256_add_epi32(yb, _mm256_unpackhi_epi32(g, g)),
        _mm256_add_epi32(yb, _mm256_unpackhi_epi32(r, r)));
    store_sixteen(a, d, output);
}

template<bool SwapUV>
__forceinline void nv_row(const unsigned char* source, const unsigned char* chroma,
    int left, int count, unsigned char* output, const Coefficients& c) {
    if ((left & 1) && count) {
        const unsigned char* pair = chroma + (left & ~1);
        pixel(source[left], pair[SwapUV ? 1 : 0], pair[SwapUV ? 0 : 1], output, c);
        ++left;
        --count;
        output += 3;
    }
    source += left;
    chroma += left;
    while (count >= 16) {
        const __m128i uv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(chroma));
        const __m128i u = _mm_shuffle_epi8(uv, _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1));
        const __m128i v = _mm_shuffle_epi8(uv, _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1));
        sixteen(_mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source))),
            _mm256_cvtepu8_epi32(SwapUV ? v : u), _mm256_cvtepu8_epi32(SwapUV ? u : v), output, c);
        source += 16;
        chroma += 16;
        output += 48;
        count -= 16;
    }
    const __m128i uMask = _mm_setr_epi8(0, 0, 2, 2, 4, 4, 6, 6, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i vMask = _mm_setr_epi8(1, 1, 3, 3, 5, 5, 7, 7, -1, -1, -1, -1, -1, -1, -1, -1);
    while (count >= 8) {
        const __m128i uv = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(chroma));
        eight(_mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(source))),
            _mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv, SwapUV ? vMask : uMask)),
            _mm256_cvtepu8_epi32(_mm_shuffle_epi8(uv, SwapUV ? uMask : vMask)), output, c);
        source += 8;
        chroma += 8;
        output += 24;
        count -= 8;
    }
    for (int x = 0; x < count; ++x) {
        const unsigned char* pair = chroma + (x & ~1);
        pixel(source[x], pair[SwapUV ? 1 : 0], pair[SwapUV ? 0 : 1], output + x * 3, c);
    }
}

} // namespace detail

__forceinline void yuy2_row(const unsigned char* source, int left, int count,
    unsigned char* output, const Coefficients& c) {
    if ((left & 1) && count) {
        const unsigned char* pair = source + (left & ~1) * 2;
        detail::pixel(pair[2], pair[1], pair[3], output, c);
        ++left;
        --count;
        output += 3;
    }
    source += left * 2;
    while (count >= 16) {
        const __m256i packed = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source));
        const __m256i uv = _mm256_srli_epi32(packed, 8);
        detail::sixteen(_mm256_and_si256(packed, _mm256_set1_epi16(255)),
            _mm256_and_si256(uv, _mm256_set1_epi32(255)),
            _mm256_srli_epi32(uv, 16), output, c);
        source += 32;
        output += 48;
        count -= 16;
    }
    const __m128i yMask = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i uMask = _mm_setr_epi8(1, 1, 5, 5, 9, 9, 13, 13, -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i vMask = _mm_setr_epi8(3, 3, 7, 7, 11, 11, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1);
    while (count >= 8) {
        const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
        detail::eight(_mm256_cvtepu8_epi32(_mm_shuffle_epi8(packed, yMask)),
            _mm256_cvtepu8_epi32(_mm_shuffle_epi8(packed, uMask)),
            _mm256_cvtepu8_epi32(_mm_shuffle_epi8(packed, vMask)), output, c);
        source += 16;
        output += 24;
        count -= 8;
    }
    for (int x = 0; x < count; ++x) {
        const unsigned char* pair = source + (x & ~1) * 2;
        detail::pixel(pair[(x & 1) * 2], pair[1], pair[3], output + x * 3, c);
    }
}

__forceinline void nv12_row(const unsigned char* source, const unsigned char* chroma,
    int left, int count, unsigned char* output, const Coefficients& c) {
    detail::nv_row<false>(source, chroma, left, count, output, c);
}

__forceinline void nv21_row(const unsigned char* source, const unsigned char* chroma,
    int left, int count, unsigned char* output, const Coefficients& c) {
    detail::nv_row<true>(source, chroma, left, count, output, c);
}

__forceinline void p010_row(const unsigned char* source, const unsigned char* chroma,
    int left, int count, unsigned char* output, const Coefficients& c) {
    if ((left & 1) && count) {
        const unsigned char* pair = chroma + (left & ~1) * 2;
        detail::pixel(detail::p010(source + left * 2), detail::p010(pair), detail::p010(pair + 2), output, c);
        ++left;
        --count;
        output += 3;
    }
    source += left * 2;
    chroma += left * 2;
    while (count >= 16) {
        const __m256i uv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(chroma));
        detail::sixteen(_mm256_srli_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(source)), 6),
            _mm256_and_si256(_mm256_srli_epi32(uv, 6), _mm256_set1_epi32(1023)),
            _mm256_srli_epi32(uv, 22), output, c);
        source += 32;
        chroma += 32;
        output += 48;
        count -= 16;
    }
    const __m128i uMask = _mm_setr_epi8(0, 1, 0, 1, 4, 5, 4, 5, 8, 9, 8, 9, 12, 13, 12, 13);
    const __m128i vMask = _mm_setr_epi8(2, 3, 2, 3, 6, 7, 6, 7, 10, 11, 10, 11, 14, 15, 14, 15);
    while (count >= 8) {
        const __m128i uv = _mm_srli_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(chroma)), 6);
        detail::eight(_mm256_cvtepu16_epi32(_mm_srli_epi16(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(source)), 6)),
            _mm256_cvtepu16_epi32(_mm_shuffle_epi8(uv, uMask)),
            _mm256_cvtepu16_epi32(_mm_shuffle_epi8(uv, vMask)), output, c);
        source += 16;
        chroma += 16;
        output += 24;
        count -= 8;
    }
    for (int x = 0; x < count; ++x) {
        const unsigned char* pair = chroma + (x & ~1) * 2;
        detail::pixel(detail::p010(source + x * 2), detail::p010(pair), detail::p010(pair + 2), output + x * 3, c);
    }
}

namespace detail {

// Both luma rows share these already-computed chroma contributions.
template<bool ThreeStores>
__forceinline void sixteen_y(__m256i y, __m256i b, __m256i g, __m256i r,
    unsigned char* output, const Coefficients& c) {
    y = _mm256_subs_epu16(y, _mm256_set1_epi16(static_cast<short>(c.yOffset)));
    __m256i ya = _mm256_unpacklo_epi16(y, _mm256_setzero_si256());
    __m256i yb = _mm256_unpackhi_epi16(y, _mm256_setzero_si256());
    ya = _mm256_add_epi32(_mm256_mullo_epi32(ya, _mm256_set1_epi32(c.y)), _mm256_set1_epi32(32768));
    yb = _mm256_add_epi32(_mm256_mullo_epi32(yb, _mm256_set1_epi32(c.y)), _mm256_set1_epi32(32768));
    const __m256i a = pack_bgr(
        _mm256_add_epi32(ya, _mm256_unpacklo_epi32(b, b)),
        _mm256_add_epi32(ya, _mm256_unpacklo_epi32(g, g)),
        _mm256_add_epi32(ya, _mm256_unpacklo_epi32(r, r)));
    const __m256i d = pack_bgr(
        _mm256_add_epi32(yb, _mm256_unpackhi_epi32(b, b)),
        _mm256_add_epi32(yb, _mm256_unpackhi_epi32(g, g)),
        _mm256_add_epi32(yb, _mm256_unpackhi_epi32(r, r)));
    if constexpr (ThreeStores) store_sixteen(a, d, output);
    else {
        store_eight(_mm256_permute2x128_si256(a, d, 0x20), output);
        store_eight(_mm256_permute2x128_si256(a, d, 0x31), output + 24);
    }
}

template<bool TenBit>
__forceinline __m256i load_y(const unsigned char* source) {
    if constexpr (TenBit)
        return _mm256_srli_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(source)), 6);
    else
        return _mm256_cvtepu8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(source)));
}

template<bool TenBit, bool SwapUV>
__forceinline void planar_row(const unsigned char* source, const unsigned char* chroma,
    int left, int count, unsigned char* output, const Coefficients& c) {
    if constexpr (TenBit) p010_row(source, chroma, left, count, output, c);
    else nv_row<SwapUV>(source, chroma, left, count, output, c);
}

template<bool TenBit, bool SwapUV>
__forceinline void two_rows(const unsigned char* source0, const unsigned char* source1,
    const unsigned char* chroma, int left, int count, unsigned char* output0, unsigned char* output1,
    const Coefficients& c) {
    constexpr int bpp = TenBit ? 2 : 1;
    if ((left & 1) && count) {
        planar_row<TenBit, SwapUV>(source0, chroma, left, 1, output0, c);
        planar_row<TenBit, SwapUV>(source1, chroma, left, 1, output1, c);
        ++left;
        --count;
        output0 += 3;
        output1 += 3;
    }
    source0 += left * bpp;
    source1 += left * bpp;
    chroma += left * bpp;
    while (count >= 16) {
        __m256i u, v;
        if constexpr (TenBit) {
            const __m256i uv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(chroma));
            u = _mm256_and_si256(_mm256_srli_epi32(uv, 6), _mm256_set1_epi32(1023));
            v = _mm256_srli_epi32(uv, 22);
        } else {
            const __m128i uv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(chroma));
            const __m128i first = _mm_shuffle_epi8(uv, _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1));
            const __m128i second = _mm_shuffle_epi8(uv, _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1));
            u = _mm256_cvtepu8_epi32(SwapUV ? second : first);
            v = _mm256_cvtepu8_epi32(SwapUV ? first : second);
        }
        u = _mm256_sub_epi32(u, _mm256_set1_epi32(c.uvOffset));
        v = _mm256_sub_epi32(v, _mm256_set1_epi32(c.uvOffset));
        const __m256i b = _mm256_mullo_epi32(u, _mm256_set1_epi32(c.blueU));
        const __m256i r = _mm256_mullo_epi32(v, _mm256_set1_epi32(c.redV));
        const __m256i g = _mm256_add_epi32(_mm256_mullo_epi32(u, _mm256_set1_epi32(c.greenU)),
            _mm256_mullo_epi32(v, _mm256_set1_epi32(c.greenV)));
        // Three full stores benefit the 10-bit path; keep the faster measured
        // 8-bit instruction schedule (including its existing register layout).
        sixteen_y<TenBit>(load_y<TenBit>(source0), b, g, r, output0, c);
        sixteen_y<TenBit>(load_y<TenBit>(source1), b, g, r, output1, c);
        source0 += 16 * bpp;
        source1 += 16 * bpp;
        chroma += 16 * bpp;
        output0 += 48;
        output1 += 48;
        count -= 16;
    }
    if (count) {
        planar_row<TenBit, SwapUV>(source0, chroma, 0, count, output0, c);
        planar_row<TenBit, SwapUV>(source1, chroma, 0, count, output1, c);
    }
}

} // namespace detail

// source0 and source1 must belong to the same 4:2:0 chroma row. The caller
// handles an odd first source row and an unpaired final row with the row APIs.
__forceinline void nv12_two_rows(const unsigned char* source0, const unsigned char* source1,
    const unsigned char* chroma, int left, int count, unsigned char* output0, unsigned char* output1,
    const Coefficients& c) {
    detail::two_rows<false, false>(source0, source1, chroma, left, count, output0, output1, c);
}

__forceinline void nv21_two_rows(const unsigned char* source0, const unsigned char* source1,
    const unsigned char* chroma, int left, int count, unsigned char* output0, unsigned char* output1,
    const Coefficients& c) {
    detail::two_rows<false, true>(source0, source1, chroma, left, count, output0, output1, c);
}

__forceinline void p010_two_rows(const unsigned char* source0, const unsigned char* source1,
    const unsigned char* chroma, int left, int count, unsigned char* output0, unsigned char* output1,
    const Coefficients& c) {
    detail::two_rows<true, false>(source0, source1, chroma, left, count, output0, output1, c);
}

} // namespace capture_pixels
