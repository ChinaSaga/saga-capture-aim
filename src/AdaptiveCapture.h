#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace adaptive_capture {
inline constexpr int minSize = 160, maxSize = 640, sizeStep = 32;
inline bool validSize(int size) { return size >= minSize && size <= maxSize && size % sizeStep == 0; }
// Choose the smallest square whose half-width strictly contains the target's
// radial distance. Released keys and missing targets always search wide.
inline int desiredSize(bool keyDown, bool target, double dx, double dy) {
    if (!keyDown || !target || !std::isfinite(dx) || !std::isfinite(dy)) return maxSize;
    const double distance = std::hypot(dx, dy);
    if (distance < minSize * 0.5) return minSize;
    if (distance >= maxSize * 0.5) return maxSize;
    return (static_cast<int>(distance / (sizeStep * 0.5)) + 1) * sizeStep;
}
inline int largestSize(const std::vector<int>& sizes) {
    return sizes.empty() ? 0 : *std::max_element(sizes.begin(), sizes.end());
}
inline int availableSize(int desired, const std::vector<int>& sizes) {
    int result = 0;
    for (int size : sizes)
        if (size >= desired && (!result || size < result)) result = size;
    return result ? result : largestSize(sizes);
}
struct TargetBox {
    double x = 0, y = 0, width = 0, height = 0;
};
inline int roundSizeUp(double size) {
    if (!std::isfinite(size) || size >= maxSize) return maxSize;
    if (size <= minSize) return minSize;
    return static_cast<int>(std::ceil(size / sizeStep)) * sizeStep;
}
// Boxes are in the CURRENT crop's coordinates, independent of the aim point.
// Keep 10% of each dimension (at least 16 px) on every side of the whole box.
inline int targetSize(int currentSize, const TargetBox& box) {
    if (!validSize(currentSize) || !std::isfinite(box.x) || !std::isfinite(box.y) ||
        !std::isfinite(box.width) || !std::isfinite(box.height) || box.width <= 0 || box.height <= 0)
        return maxSize;
    const double right = box.x + box.width, bottom = box.y + box.height;
    if (!std::isfinite(right) || !std::isfinite(bottom)) return maxSize;
    const double center = currentSize * 0.5;
    const double paddingX = (std::max)(16.0, box.width * 0.1);
    const double paddingY = (std::max)(16.0, box.height * 0.1);
    const double radiusX = (std::max)(std::abs(box.x - center), std::abs(right - center)) + paddingX;
    const double radiusY = (std::max)(std::abs(box.y - center), std::abs(bottom - center)) + paddingY;
    double required = 2 * (std::max)(radiusX, radiusY);
    // Detector outputs may already be clipped at the image boundary. Probe a
    // wider crop on the next frame instead of treating the clipped size as complete.
    constexpr double edgeMargin = 8;
    if (box.x <= edgeMargin || box.y <= edgeMargin || right >= currentSize - edgeMargin || bottom >= currentSize - edgeMargin)
        required = (std::max)(required, currentSize * 1.2);
    return roundSizeUp(required);
}
class Controller {
    int smallerSize_ = 0, smallerFrames_ = 0;
public:
    void reset() { smallerSize_ = smallerFrames_ = 0; }
    int nextSize(bool keyDown, bool target, double dx, double dy, const TargetBox& box,
        int currentSize, const std::vector<int>& sizes) {
        if (!keyDown || !target) { reset(); return largestSize(sizes); }
        const int desired = (std::max)(desiredSize(keyDown, target, dx, dy), targetSize(currentSize, box));
        const int next = availableSize(desired, sizes);
        if (!next) { reset(); return 0; }
        // Expansion is immediate. A smaller available model must be suitable
        // for three consecutive detections before we shrink to it.
        if (next >= currentSize) { reset(); return next; }
        if (smallerSize_ != next) { smallerSize_ = next; smallerFrames_ = 0; }
        if (++smallerFrames_ < 3) return currentSize;
        reset();
        return next;
    }
};
}
