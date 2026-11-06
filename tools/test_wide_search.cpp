#include "WideSearch.h"
#include <cassert>
#include <vector>
#include <iostream>
int main() {
    constexpr int stride=1920*3+32;
    std::vector<unsigned char> pixels(stride*640,0);
    DetectObject out[99];
    std::vector<int> calls;
    const auto run=[&](const unsigned char* p,int w,int h,size_t step,float,float,DetectObject* boxes) {
        assert(w==640 && h==640 && step==stride);
        calls.push_back(int((p-pixels.data())/3));
        boxes[0]={100,200,40,80,1,.9f};return 1;
    };
    assert(wide_search::detect(pixels.data(),1920,640,stride,.2f,.5f,out,run)==3);
    assert((calls==std::vector<int>{640,0,1280}));
    assert(out[0].x==740 && out[1].x==100 && out[2].x==1380);
    for(int i=0;i<3;++i) assert(out[i].y==200 && out[i].width==40 && out[i].height==80);
    calls.clear();
    assert(wide_search::detect(pixels.data(),640,640,stride,.2f,.5f,out,run)==1 && calls.size()==1);
    // Overlapping tiles of a narrow source must not duplicate the same box.
    const auto overlap=[&](const unsigned char* p,int,int,size_t,float,float,DetectObject* boxes) {
        const int left=int((p-pixels.data())/3);
        boxes[0]={float(400-left),200,40,80,1,.9f};return left==640?0:1;
    };
    assert(wide_search::detect(pixels.data(),1280,640,stride,.2f,.5f,out,overlap)==1 && out[0].x==400);
    // Capacity is bounded even when all tiles are busy; a side-tile failure
    // invalidates the whole frame rather than publishing the center alone.
    const auto many=[](const unsigned char*,int,int,size_t,float,float,DetectObject* boxes) {
        for(int i=0;i<99;++i) boxes[i]={float(i*6),200,2,2,1,.9f};return 99;
    };
    assert(wide_search::detect(pixels.data(),1920,640,stride,.2f,.5f,out,many)==99);
    const auto fail=[&](const unsigned char* p,int,int,size_t,float,float,DetectObject*) { return p==pixels.data()?-1:0; };
    assert(wide_search::detect(pixels.data(),1920,640,stride,.2f,.5f,out,fail)==-1);
    assert(wide_search::detect(pixels.data(),1920,640,100,.2f,.5f,out,run)==-1);
    std::cout<<"PASS wide search: native tiles, shared stride, global coordinates, narrow sources, NMS, capacity, failure\n";
}
