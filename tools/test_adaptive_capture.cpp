#include "AdaptiveCapture.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace adaptive_capture;
static TargetBox centered(int crop, double width, double height, double x = 0, double y = 0) {
    return {crop * .5 + x - width * .5, crop * .5 + y - height * .5, width, height};
}
int main() {
    std::vector<int> sizes;
    for (int size = minSize; size <= maxSize; size += sizeStep) sizes.push_back(size);
    assert(targetSize(256, centered(256, 250, 250)) == 320);
    assert(targetSize(256, centered(256, 256, 256)) == 320);
    assert(targetSize(320, centered(320, 320, 320)) == 384);
    assert(targetSize(416, centered(416, 320, 320, 10)) == 416);
    assert(targetSize(640, centered(640, 320, 100)) == 384);
    assert(targetSize(640, centered(640, 100, 320)) == 384);
    assert(targetSize(640, centered(640, 100, 100, 100)) == 352);
    assert(targetSize(256, {230, 100, 26, 40}) == 320);
    assert(targetSize(256, {-20, -20, 296, 296}) == 384);
    assert(targetSize(640, centered(640, 900, 900)) == 640);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const auto& box : {TargetBox{}, TargetBox{0, 0, -1, 10}, TargetBox{nan, 0, 10, 10},
            TargetBox{0, inf, 10, 10}, TargetBox{0, 0, inf, 10}})
        assert(targetSize(256, box) == 640);

    Controller state;
    auto next = [&](int crop, double width, double height) {
        return state.nextSize(true, true, 0, 0, centered(crop, width, height), crop, sizes);
    };
    assert(next(256, 250, 250) == 320); // Immediate expansion, despite centered lock point.
    for (int i = 0; i < 100; ++i) assert(next(320, 250, 250) == 320);
    assert(next(320, 320, 320) == 384);
    for (int i = 0; i < 100; ++i) assert(next(384, 320, 320) == 384);
    assert(next(384, 250, 250) == 384);
    assert(next(384, 250, 250) == 384);
    assert(next(384, 250, 250) == 320);

    // A noisy boundary cannot alternate sizes after single-frame smaller boxes.
    for (int i = 0; i < 100; ++i) {
        assert(next(352, 250, 250) == 352);
        assert(next(352, 280, 280) == 352);
    }
    assert(next(352, 250, 250) == 352);
    assert(next(352, 250, 250) == 352);
    assert(next(352, 250, 250) == 320);

    // A large target clipped by the current crop reveals more of itself at each
    // expansion, until the whole target and its surrounding context fit.
    int crop = 256;
    for (int expected : {320, 384, 480, 576, 608}) {
        crop = next(crop, (std::min)(crop, 500), (std::min)(crop, 500));
        assert(crop == expected);
    }
    for (int i = 0; i < 10; ++i) assert(next(crop, 500, 500) == 608);
    assert(next(608, 608, 608) == 640);
    assert(next(640, 640, 640) == 640);

    // Cropping is centered on the screen, even if the crosshair is off-center.
    assert(state.nextSize(true, true, 0, 0, centered(256, 100, 100, 100), 256, sizes) == 352);
    assert(state.nextSize(true, true, 200, 0, centered(256, 40, 40), 256, sizes) == 416);
    assert(state.nextSize(true, true, nan, 0, centered(256, 40, 40), 256, sizes) == 640);

    // Only actual models participate; no intermediate unavailable stage can be chosen.
    const std::vector<int> sparse{256, 320, 416, 640};
    assert(state.nextSize(true, true, 0, 0, centered(256, 250, 250), 256, sparse) == 320);
    assert(state.nextSize(true, true, 0, 0, centered(320, 320, 320), 320, sparse) == 416);
    assert(state.nextSize(true, true, 0, 0, centered(416, 410, 410), 416, sparse) == 640);
    assert(state.nextSize(false, true, 0, 0, centered(256, 40, 40), 256, sparse) == 640);
    assert(state.nextSize(true, false, 0, 0, {}, 256, sparse) == 640);
    assert(next(640, 250, 250) == 640);
    assert(next(640, 250, 250) == 640);
    state.reset(); // Engine/model/profile changes must discard pending shrink confirmation.
    assert(next(640, 250, 250) == 640);
    assert(next(640, 250, 250) == 640);
    assert(next(640, 250, 250) == 320);

    const std::vector<int> capped{320,416,512};
    assert(largestSize(capped)==512 && largestSize({416,320,512})==512 && largestSize({})==0);
    assert(state.nextSize(false,true,0,0,{},320,capped)==512);
    assert(state.nextSize(true,false,0,0,{},320,capped)==512);
    assert(state.nextSize(true,true,1000,0,centered(512,500,500),512,capped)==512);
    assert(state.nextSize(true,true,0,0,centered(320,320,320),320,capped)==416);
    assert(state.nextSize(true,true,0,0,centered(416,410,410),416,capped)==512);
    for (bool key : {false,true}) for (bool target : {false,true}) {
        for (double extent : {20.,250.,320.,900.}) {
            for (int i=0;i<5;++i)
                assert(state.nextSize(key,target,300,0,centered(320,extent,extent),320,{320})==320);
        }
    }
    assert(state.nextSize(true,true,0,0,{},320,{})==0);

    // Translation into a different crop keeps the required range identical once
    // the complete box is visible; distance reductions cannot cut off its edges.
    for (double width = 20; width <= 500; width += 20) {
        for (double offset = -100; offset <= 100; offset += 20) {
            const auto box = centered(640, width, width / 2, offset);
            const int needed = targetSize(640, box);
            assert(validSize(needed));
            if (needed < 640) {
                const double pad = (std::max)(16.0, width * .1);
                assert(needed >= 2 * std::abs(offset) + width + 2 * pad);
                assert(targetSize(needed, centered(needed, width, width / 2, offset)) == needed);
            }
        }
    }
    std::cout << "PASS adaptive target size: padding, clipping, offsets, cached stages, shrink confirmation and recovery\n";
}
