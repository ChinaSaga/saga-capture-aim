#pragma once
#include "Inference.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace wide_search {
inline constexpr int tileSize = 640, maxWidth = 1920;
// Three native-resolution tiles of the SAME frame. Shorter sources overlap
// at the edges; suppress duplicate same-class detections after translation.
template<class Detect>
int detect(const unsigned char* pixels, int width, int height, size_t stride,
    float confidence, float nms, DetectObject* output, Detect&& run) {
    if (!pixels || width < tileSize || width > maxWidth || height != tileSize ||
        stride < size_t(width) * 3) return -1;
    const std::array<int, 3> offsets{(width - tileSize) / 2, 0, width - tileSize};
    std::array<DetectObject, 297> candidates; // Only the initialized [0, count) prefix is read.
    int count = 0;
    for (int tile = 0; tile < 3; ++tile) {
        const int left = offsets[tile];
        if (std::find(offsets.begin(), offsets.begin() + tile, left) != offsets.begin() + tile) continue;
        DetectObject boxes[99];
        const int found = run(pixels + size_t(left) * 3, tileSize, tileSize, stride, confidence, nms, boxes);
        if (found < 0) return found; // Never publish a partially successful search.
        for (int i = 0; i < (std::min)(found, 99); ++i) {
            auto box = boxes[i];
            if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.width) ||
                !std::isfinite(box.height) || !std::isfinite(box.prob) || box.width <= 0 || box.height <= 0) continue;
            const float right = (std::clamp)(box.x + box.width, 0.f, float(tileSize));
            const float bottom = (std::clamp)(box.y + box.height, 0.f, float(tileSize));
            box.x = (std::clamp)(box.x, 0.f, float(tileSize));
            box.y = (std::clamp)(box.y, 0.f, float(tileSize));
            box.width = right - box.x; box.height = bottom - box.y;
            if (box.width <= 0 || box.height <= 0) continue;
            box.x += left;
            candidates[count++] = box;
        }
    }
    // Sort small indices on the stack instead of allocating stable_sort's
    // temporary box buffer on every frame. The original index is the tie
    // breaker, preserving center/left/right order for equal scores.
    std::array<int, 297> order;
    std::iota(order.begin(), order.begin() + count, 0);
    std::sort(order.begin(), order.begin() + count, [&](int a, int b) {
        if (candidates[a].prob > candidates[b].prob) return true;
        if (candidates[b].prob > candidates[a].prob) return false;
        return a < b;
    });
    int kept = 0;
    for (int i = 0; i < count && kept < 99; ++i) {
        const auto& a = candidates[order[i]];
        bool duplicate = false;
        for (int j = 0; j < kept && !duplicate; ++j) {
            const auto& b = output[j];
            if (a.label != b.label) continue;
            const float w = (std::max)(0.f, (std::min)(a.x+a.width,b.x+b.width) - (std::max)(a.x,b.x));
            if (w <= 0) continue; // Disjoint tiles need no Y/area/IoU arithmetic.
            const float h = (std::max)(0.f, (std::min)(a.y+a.height,b.y+b.height) - (std::max)(a.y,b.y));
            const float overlap = w*h;
            duplicate = overlap > 0 && overlap > nms * (a.width*a.height + b.width*b.height - overlap);
        }
        if (!duplicate) output[kept++] = a;
    }
    return kept;
}
}
