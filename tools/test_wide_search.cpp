#include "WideSearch.h"
#include <cassert>
#include <vector>
#include <iostream>
#include <chrono>
#include <cstring>
#include <random>

// Deliberately simple stable-sort reference for the observable merge contract.
// This also measures the former allocating merge without any device/inference.
template<class Run>
static int reference(const unsigned char* pixels, int width, size_t stride, float nms,
    DetectObject* output, Run run) {
    std::array<DetectObject, 297> candidates{};
    std::array<int, 3> offsets{(width-640)/2, 0, width-640};
    int count = 0;
    for (int tile=0; tile<3; ++tile) {
        const int left=offsets[tile];
        if (std::find(offsets.begin(),offsets.begin()+tile,left)!=offsets.begin()+tile) continue;
        DetectObject boxes[99];
        const int found=run(pixels+size_t(left)*3,640,640,stride,.2f,nms,boxes);
        for (int i=0;i<found;++i) {
            auto box=boxes[i];
            if (box.width<=0 || box.height<=0) continue;
            const float right=std::clamp(box.x+box.width,0.f,640.f);
            const float bottom=std::clamp(box.y+box.height,0.f,640.f);
            box.x=std::clamp(box.x,0.f,640.f); box.y=std::clamp(box.y,0.f,640.f);
            box.width=right-box.x; box.height=bottom-box.y;
            if (box.width<=0 || box.height<=0) continue;
            box.x+=left; candidates[count++]=box;
        }
    }
    std::stable_sort(candidates.begin(),candidates.begin()+count,
        [](const auto& a,const auto& b) { return a.prob>b.prob; });
    int kept=0;
    for (int i=0;i<count && kept<99;++i) {
        const auto& a=candidates[i]; bool duplicate=false;
        for (int j=0;j<kept && !duplicate;++j) {
            const auto& b=output[j]; if(a.label!=b.label) continue;
            const float w=std::max(0.f,std::min(a.x+a.width,b.x+b.width)-std::max(a.x,b.x));
            const float h=std::max(0.f,std::min(a.y+a.height,b.y+b.height)-std::max(a.y,b.y));
            const float overlap=w*h;
            duplicate=overlap>0 && overlap>nms*(a.width*a.height+b.width*b.height-overlap);
        }
        if(!duplicate) output[kept++]=a;
    }
    return kept;
}

static void differential() {
    std::mt19937 random(20261106);
    std::vector<unsigned char> pixels(1920*640*3);
    std::array<std::array<DetectObject,99>,3> tiles;
    std::array<int,3> counts;
    DetectObject actual[99], expected[99];
    int width=1920;
    const auto run=[&](const unsigned char* p,int,int,size_t,float,float,DetectObject* out) {
        const int left=int((p-pixels.data())/3);
        const int tile=left==(width-640)/2?0:left==0?1:2;
        std::copy_n(tiles[tile].data(),counts[tile],out); return counts[tile];
    };
    for (int frame=0;frame<6000;++frame) {
        width=640+int(random()%1281);
        for(int tile=0;tile<3;++tile) {
            counts[tile]=int(random()%100);
            for(auto& box:tiles[tile]) box={float(int(random()%900)-130),float(int(random()%900)-130),
                float(int(random()%400)-10),float(int(random()%400)-10),int(random()%10),float(random()%8)/8.f};
        }
        const float threshold=float(random()%11)/10.f;
        const int a=wide_search::detect(pixels.data(),width,640,1920*3,.2f,threshold,actual,run);
        const int b=reference(pixels.data(),width,1920*3,threshold,expected,run);
        assert(a==b && !std::memcmp(actual,expected,a*sizeof(DetectObject)));
    }
    width=1920;
    size_t checksum=0;
    const auto measure=[&](bool optimized,int repeats) {
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<repeats;++i) checksum += optimized ?
            wide_search::detect(pixels.data(),width,640,1920*3,.2f,.5f,actual,run) :
            reference(pixels.data(),width,1920*3,.5f,actual,run);
        return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/repeats;
    };
    for(int count:{1,8,32,99}) {
        counts.fill(count);
        measure(false,1000); measure(true,1000);
        std::array<double,7> before, after;
        for(int trial=0;trial<7;++trial) {
            if(trial%2) { after[trial]=measure(true,5000); before[trial]=measure(false,5000); }
            else { before[trial]=measure(false,5000); after[trial]=measure(true,5000); }
        }
        std::sort(before.begin(),before.end()); std::sort(after.begin(),after.end());
        std::cout<<"Merge-only median us, "<<count<<" boxes/tile: stable_sort="<<before[3]<<" optimized="<<after[3]<<'\n';
    }
    std::cout<<"PASS 6000 bit-identical randomized merges (ties, clipping, overlap, classes, capacity); checksum="<<checksum<<'\n';
}
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
    differential();
    std::cout<<"PASS wide search: native tiles, shared stride, global coordinates, narrow sources, NMS, capacity, failure\n";
}
