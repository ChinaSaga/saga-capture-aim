#pragma once
#include <cstdint>
#include <cstring>
#include <limits>

namespace encoded_image {
struct BgrView { int width = 0, height = 0; size_t offset = 0, stride = 0; };
inline bool topDownBgr(const unsigned char* bytes, size_t size, BgrView& view) {
    if (!bytes || size < 54 || bytes[0] != 'B' || bytes[1] != 'M') return false;
    std::uint32_t offset, header, compression;
    std::int32_t width, height;
    std::uint16_t planes, bits;
    std::memcpy(&offset, bytes + 10, 4); std::memcpy(&header, bytes + 14, 4);
    std::memcpy(&width, bytes + 18, 4); std::memcpy(&height, bytes + 22, 4);
    std::memcpy(&planes, bytes + 26, 2); std::memcpy(&bits, bytes + 28, 2);
    std::memcpy(&compression, bytes + 30, 4);
    if (header < 40 || header > size - 14 || offset < 14ull + header || offset > size ||
        width <= 0 || height >= 0 || height == (std::numeric_limits<int>::min)() ||
        planes != 1 || bits != 24 || compression != 0) return false;
    const std::uint64_t stride = (std::uint64_t(width) * 3 + 3) & ~std::uint64_t(3);
    if (stride * std::uint64_t(-height) > size - offset) return false;
    view = {width, -height, offset, static_cast<size_t>(stride)};
    return true;
}
}
