#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

// Planar YOLO [1, 4+classes, candidates]. Visit accepted candidates in their
// original order, preserving the first class on ties and skipping nonfinite
// winning scores. No temporary candidate array or per-frame allocation.
#pragma float_control(precise, on, push)
template<class Append>
inline void forEachYoloCandidate(const float* values, size_t candidates,
                                int classes, float threshold, Append&& append) {
    if (classes <= 0) return;
    size_t d = 0;
#if defined(__AVX2__)
    const __m256 limit = _mm256_set1_ps(threshold);
    const __m256i exponent = _mm256_set1_epi32(0x7f800000);
    for (; d + 8 <= candidates; d += 8) {
        __m256 best = _mm256_loadu_ps(values + 4 * candidates + d);
        __m256i labels = _mm256_setzero_si256();
        for (int c = 1; c < classes; ++c) {
            const __m256 next = _mm256_loadu_ps(values + (size_t(c) + 4) * candidates + d);
            const __m256 higher = _mm256_cmp_ps(next, best, _CMP_GT_OQ);
            best = _mm256_blendv_ps(best, next, higher);
            labels = _mm256_blendv_epi8(labels, _mm256_set1_epi32(c), _mm256_castps_si256(higher));
        }
        const __m256i nonfinite = _mm256_cmpeq_epi32(
            _mm256_and_si256(_mm256_castps_si256(best), exponent), exponent);
        const unsigned accepted = unsigned(_mm256_movemask_ps(_mm256_andnot_ps(
            _mm256_castsi256_ps(nonfinite), _mm256_cmp_ps(best, limit, _CMP_GT_OQ))));
        if (!accepted) continue;
        alignas(32) float scores[8];
        alignas(32) int ids[8];
        _mm256_store_ps(scores, best);
        _mm256_store_si256(reinterpret_cast<__m256i*>(ids), labels);
        for (unsigned lane = 0; lane < 8; ++lane)
            if (accepted & (1u << lane)) append(d + lane, scores[lane], ids[lane]);
    }
#endif
    for (; d < candidates; ++d) {
        float score = values[4 * candidates + d];
        int label = 0;
        for (int c = 1; c < classes; ++c) {
            const float next = values[(size_t(c) + 4) * candidates + d];
            if (next > score) { score = next; label = c; }
        }
        if ((std::bit_cast<uint32_t>(score) & 0x7f800000u) != 0x7f800000u && score > threshold)
            append(d, score, label);
    }
}
#pragma float_control(pop)
