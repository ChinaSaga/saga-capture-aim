#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace coefficient_move {
struct Point { double x=0, y=0; };
inline bool finite(double v) {
    return (std::bit_cast<std::uint64_t>(v)&0x7ff0000000000000ull)!=0x7ff0000000000000ull;
}
inline Point calculate(Point point, double factor) {
    if (!finite(factor)||factor<=0||!finite(point.x)||!finite(point.y)) return {};
    const double scale=(std::max)(1.0,std::hypot(point.x,point.y)/100.0);
    const double gain=(std::min)(factor,10.0)/scale;
    return {std::round(point.x*gain),std::round(point.y*gain)};
}
}
