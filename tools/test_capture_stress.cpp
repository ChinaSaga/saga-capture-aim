// No inference, app startup or mouse transport. Synthetic guarded-buffer tests,
// exact pixel comparisons, concurrent consumers and explicitly selected live capture.
#include "../src/OpnecvCapture.cpp"
#include "../src/Screenshot.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void reference(int y, int u, int v, BYTE* output, const capture_pixels::Coefficients& c) {
    const int base = (std::max)(0, y-c.yOffset) * c.y;
    const int blue = base + (u-c.uvOffset)*c.blueU;
    const int green = base + (u-c.uvOffset)*c.greenU + (v-c.uvOffset)*c.greenV;
    const int red = base + (v-c.uvOffset)*c.redV;
    const int channels[] = {blue,green,red};
    for (int i=0;i<3;++i) output[i] = static_cast<BYTE>(std::clamp((channels[i]+32768)>>16,0,255));
}
struct GuardBuffer {
    BYTE* allocation = static_cast<BYTE*>(VirtualAlloc(nullptr,12288,MEM_RESERVE,PAGE_NOACCESS));
    GuardBuffer() { require(allocation && VirtualAlloc(allocation+4096,4096,MEM_COMMIT,PAGE_READWRITE),"guard allocation"); }
    ~GuardBuffer() { VirtualFree(allocation,0,MEM_RELEASE); }
    BYTE* end(size_t bytes) { require(bytes+32<4096,"guard capacity"); return allocation+8192-bytes; }
};

static void rows(int format, bool fast, const BYTE* y0,const BYTE* y1,const BYTE* uv,int left,int width,
    BYTE* a,BYTE* b,const capture_pixels::Coefficients& c) {
    using namespace capture_pixels;
    if (format == 3) {
        if (fast) { yuy2_row(y0,left,width,a,c); yuy2_row(y1,left,width,b,c); }
        else { yuy2_row_avx2(y0,left,width,a,c); yuy2_row_avx2(y1,left,width,b,c); }
    } else if (fast) {
        if (format==0) nv12_two_rows(y0,y1,uv,left,width,a,b,c);
        if (format==1) nv21_two_rows(y0,y1,uv,left,width,a,b,c);
        if (format==2) p010_two_rows(y0,y1,uv,left,width,a,b,c);
    } else {
        if (format==0) detail::two_rows<false,false>(y0,y1,uv,left,width,a,b,c);
        if (format==1) detail::two_rows<false,true>(y0,y1,uv,left,width,a,b,c);
        if (format==2) detail::two_rows<true,false>(y0,y1,uv,left,width,a,b,c);
    }
}

static void guardedRows() {
    GuardBuffer y0mem,y1mem,uvmem,amem,bmem;
    std::mt19937 random(20261025);
    unsigned cases=0;
    for (int format=0;format<4;++format) for(int matrix=0;matrix<=3;++matrix) for(int range=0;range<=2;++range) {
        VIDEOINFOHEADER2 info{};
        info.dwControlFlags=AMCONTROL_COLORINFO_PRESENT|(range<<12)|(matrix<<15);
        AM_MEDIA_TYPE type{};type.formattype=FORMAT_VideoInfo2;type.cbFormat=sizeof(info);type.pbFormat=reinterpret_cast<BYTE*>(&info);
        capture_pixels::Coefficients c;
        require(yuv_conversion(type,format==2,c),"color metadata");
        for (int left : {0,1,6,7}) for(int width=1;width<=640;++width) {
            const int bpp=format>=2?2:1;
            const int pixels=(left+width+1)&~1;
            BYTE* y0=y0mem.end(pixels*bpp),*y1=y1mem.end(pixels*bpp),*uv=uvmem.end(pixels*bpp);
            BYTE* a=amem.end(width*3),*b=bmem.end(width*3);
            memset(a-32,0xa5,width*3+32);memset(b-32,0xa5,width*3+32);
            for(int i=0;i<pixels*bpp;++i) {y0[i]=static_cast<BYTE>(random());y1[i]=static_cast<BYTE>(random());uv[i]=static_cast<BYTE>(random());}
            rows(format,true,y0,y1,uv,left,width,a,b,c);
            std::vector<BYTE> expected0(width*3),expected1(width*3);
            rows(format,false,y0,y1,uv,left,width,expected0.data(),expected1.data(),c);
            require(memcmp(a,expected0.data(),width*3)==0 && memcmp(b,expected1.data(),width*3)==0,"wide/AVX2 mismatch");
            for(int row=0;row<2;++row) for(int x=0;x<width;++x) {
                const BYTE* y=row?y1:y0;const int col=left+x;
                int yy,u,v;
                if(format==3) {const BYTE* p=y+(col&~1)*2;yy=p[(col&1)*2];u=p[1];v=p[3];}
                else if(format==2) {yy=capture_pixels::detail::p010(y+col*2);u=capture_pixels::detail::p010(uv+(col&~1)*2);v=capture_pixels::detail::p010(uv+(col&~1)*2+2);}
                else {yy=y[col];u=uv[(col&~1)+(format==1?1:0)];v=uv[(col&~1)+(format==1?0:1)];}
                BYTE expected[3];reference(yy,u,v,expected,c);
                require(memcmp((row?b:a)+x*3,expected,3)==0,"scalar pixel mismatch");
            }
            for(int i=1;i<=32;++i)require(a[-i]==0xa5&&b[-i]==0xa5,"write before output");
            ++cases;
        }
    }
    std::printf("PASS %u guarded row cases: all widths 1..640, odd/even offsets, 4 formats, 12 color metadata combinations, scalar + AVX2 exact\n",cases);
}

static void exhaustive() {
    std::array<BYTE,256> y0,y1,uv;
    std::array<BYTE,768> a,b;
    for(int y=0;y<256;++y){y0[y]=static_cast<BYTE>(y);y1[y]=static_cast<BYTE>(255-y);}
    for(int u=0;u<256;++u)for(int v=0;v<256;++v){
        for(int x=0;x<256;x+=2){uv[x]=static_cast<BYTE>(u);uv[x+1]=static_cast<BYTE>(v);}
        capture_pixels::nv12_two_rows(y0.data(),y1.data(),uv.data(),0,256,a.data(),b.data(),{});
        for(int y=0;y<256;++y){BYTE expected[3];reference(y,u,v,expected,{});
            require(memcmp(a.data()+y*3,expected,3)==0 && memcmp(b.data()+(255-y)*3,expected,3)==0,"exhaustive YUV mismatch");}
    }
    std::puts("PASS all 16,777,216 8-bit Y/U/V combinations on both output rows");
}

static void frames() {
    unsigned cases=0;
    for(int format=0;format<6;++format) {
        const int w=642,h=646;
        const DWORD fourccs[]={MAKEFOURCC('N','V','1','2'),MAKEFOURCC('N','V','2','1'),MAKEFOURCC('P','0','1','0'),MAKEFOURCC('Y','U','Y','2')};
        const int bpp=format>=4?3:format>=2?2:1;
        const size_t pitch=((w*bpp+3)&~size_t(3))+32;
        VIDEOINFOHEADER info{};auto& bi=info.bmiHeader;
        bi.biSize=40;bi.biWidth=w;bi.biHeight=format==5?h:-h;bi.biPlanes=1;
        bi.biBitCount=format>=4?24:format>=2?16:12;
        bi.biCompression=format>=4?BI_RGB:fourccs[format];
        bi.biSizeImage=static_cast<DWORD>(pitch*(format<3?h+h/2:h));
        AM_MEDIA_TYPE type{};type.majortype=MEDIATYPE_Video;type.formattype=FORMAT_VideoInfo;type.cbFormat=sizeof(info);type.pbFormat=reinterpret_cast<BYTE*>(&info);
        type.subtype=format>=4?MEDIASUBTYPE_RGB24:MEDIATYPE_Video;if(format<4)type.subtype.Data1=fourccs[format];
        std::vector<BYTE> data(bi.biSizeImage);std::mt19937 rng(format+71);for(auto& v:data)v=static_cast<BYTE>(rng());
        FrameReceiver receiver;require(receiver.configure(type,w,h,160),"frame configure");
        require(receiver.QueryInterface(IID_IUnknown,nullptr)==E_POINTER,"null COM output");
        const auto beforeInvalid=g_captureSequence;
        receiver.SampleCB(0,nullptr);
        receiver.BufferCB(0,nullptr,static_cast<long>(data.size()));
        receiver.BufferCB(0,data.data(),-1);
        receiver.BufferCB(0,data.data(),static_cast<long>(data.size()-1));
        require(g_captureSequence==beforeInvalid,"invalid sample was published");
        FrameLayout layout;require(frame_layout(type,w,h,layout),"frame layout");
        for(int crop=1;crop<=640;++crop){
            require(receiver.setCrop(crop),"crop update");receiver.BufferCB(0,data.data(),static_cast<long>(data.size()));
            CaptureFrame frame;unsigned long long seq=0;require(captureAcquire(frame,seq,0),"frame missing");
            require(frame.width==crop&&frame.height==crop&&frame.size==54+frame.stride*crop,"crop/header size mismatch");
            const int left=(w-crop)/2,top=(h-crop)/2;
            for(int y=0;y<crop;++y)for(int x=0;x<crop;++x){
                BYTE expected[3];const int sx=left+x,sy=top+y;const BYTE* row=data.data()+(format==5?h-1-sy:sy)*pitch;
                if(format>=4)memcpy(expected,row+sx*3,3);
                else if(format==3){const BYTE* p=row+(sx&~1)*2;reference(p[(sx&1)*2],p[1],p[3],expected,layout.conversion);}
                else {const BYTE* uv=data.data()+(h+sy/2)*pitch+(sx&~1)*(format==2?2:1);
                    if(format==2)reference(capture_pixels::detail::p010(row+sx*2),capture_pixels::detail::p010(uv),capture_pixels::detail::p010(uv+2),expected,layout.conversion);
                    else reference(row[sx],uv[format==1?1:0],uv[format==1?0:1],expected,layout.conversion);}
                require(memcmp(frame.bmp+54+y*frame.stride+x*3,expected,3)==0,"complete crop mismatch");
            }
            for(int y=0;y<crop;++y)for(size_t x=crop*3;x<frame.stride;++x)require(frame.bmp[54+y*frame.stride+x]==0,"nonzero BMP padding");
            ++cases;
        }
        // Pin all slots, verify drop-without-overwrite and recovery after release.
        std::vector<CaptureFrame> held;std::vector<std::vector<BYTE>> copies;
        for(unsigned i=0;i<kCaptureSlots;++i){
            receiver.BufferCB(0,data.data(),static_cast<long>(data.size()));CaptureFrame f;unsigned long long s=0;
            require(captureAcquire(f,s,0),"pool acquisition");held.push_back(f);copies.emplace_back(f.bmp,f.bmp+f.size);
        }
        const auto before=g_captureSequence;receiver.BufferCB(0,data.data(),static_cast<long>(data.size()));
        require(g_captureSequence==before,"exhausted pool must drop");held[0]={};
        receiver.BufferCB(0,data.data(),static_cast<long>(data.size()));require(g_captureSequence==before+1,"released slot reuse");
        receiver.clear();
        for(size_t i=1;i<held.size();++i)require(memcmp(held[i].bmp,copies[i].data(),copies[i].size())==0,"held frame mutated");
    }
    std::printf("PASS %u full-frame crops: 1..640, padded source rows, six formats/orientations, zero padding, pool exhaustion/recovery, held leases across clear\n",cases);
}

static void concurrent() {
    VIDEOINFOHEADER info{};auto& bi=info.bmiHeader;bi.biSize=40;bi.biWidth=640;bi.biHeight=-640;
    bi.biPlanes=1;bi.biBitCount=24;bi.biCompression=BI_RGB;bi.biSizeImage=640*640*3;
    AM_MEDIA_TYPE type{};type.majortype=MEDIATYPE_Video;type.subtype=MEDIASUBTYPE_RGB24;type.formattype=FORMAT_VideoInfo;type.cbFormat=sizeof(info);type.pbFormat=reinterpret_cast<BYTE*>(&info);
    FrameReceiver receiver;require(receiver.configure(type,640,640,320),"concurrent configure");
    std::atomic<bool> done{false},failed{false};std::atomic<unsigned> reads{0},changes{0};
    std::vector<std::thread> consumers;
    for(int n=0;n<4;++n)consumers.emplace_back([&,n]{
        unsigned long long sequence=0;
        while(!done){CaptureFrame f;if(!captureAcquire(f,sequence,5))continue;
            const BYTE color=f.bmp[54];
            for(int y=0;y<f.height;++y)for(int x=0;x<f.width*3;++x)if(f.bmp[54+y*f.stride+x]!=color)failed=true;
            if(n==3)Sleep(2);++reads;
        }
    });
    std::thread cropper([&]{while(!done){for(int size:{160,161,319,320,639,640}){receiver.setCrop(size);++changes;}std::this_thread::yield();}});
    std::vector<BYTE> data(bi.biSizeImage);const auto start=Clock::now();unsigned callbacks=0;
    while(Clock::now()-start<std::chrono::seconds(3)){
        memset(data.data(),callbacks&255,data.size());receiver.BufferCB(0,data.data(),static_cast<long>(data.size()));++callbacks;
    }
    done=true;cropper.join();for(auto& t:consumers)t.join();receiver.clear();
    require(!failed&&reads>100&&changes>100,"concurrent corruption or stalled consumers");
    std::printf("PASS concurrent callbacks=%u reads=%u crop_changes=%u (four consumers, one deliberately slow)\n",callbacks,reads.load(),changes.load());
}

static void benchmark() {
    std::mt19937 rng(9);std::vector<BYTE> y0(4096),y1(4096),uv(4096),a(1920),b(1920);
    for(auto* v:{&y0,&y1,&uv})for(auto& x:*v)x=static_cast<BYTE>(rng());
    for(int format=0;format<4;++format)for(int size:{160,320,640})for(bool fast:{false,true,true,false}){
        capture_pixels::Coefficients c;if(format==2){c.yOffset=64;c.uvOffset=512;c.y=19077;c.redV=26149;c.greenU=-6419;c.greenV=-13320;c.blueU=33050;}
        const auto start=Clock::now();
        for(int i=0;i<2000;++i)for(int row=0;row<size;row+=2)rows(format,fast,y0.data(),y1.data(),uv.data(),0,size,a.data(),b.data(),c);
        std::printf("BENCH format=%d crop=%d wide=%d us=%.3f checksum=%d\n",format,size,fast,
            std::chrono::duration<double,std::micro>(Clock::now()-start).count()/2000,a[17]);
    }
}

static void live(int argc,char** argv) {
    const char* format=argc>2?argv[2]:"NV12";
    const int fps=argc>3?std::atoi(argv[3]):240;
    const int crop=argc>4?std::atoi(argv[4]):640;
    const int seconds=argc>5?std::atoi(argv[5]):30;
    const bool dynamic=argc>6&&std::strstr(argv[6],"dynamic")!=nullptr;
    const bool slow=argc>6&&std::strstr(argv[6],"slow")!=nullptr;
    const int width=argc>7?std::atoi(argv[7]):1920,height=argc>8?std::atoi(argv[8]):1080;
    require(seconds>=1&&seconds<=300,"live duration must be 1..300 seconds");
    const auto directory=file_paths::executableDirectory()/(L"live-capture-"+std::to_wstring(GetCurrentProcessId()));
    require(std::filesystem::create_directory(directory),"isolated live directory");
    const char* result=saga::setcjk_ex("Live Gamer Ultra 2.1-Video",width,height,format,fps,crop);
    require(std::strcmp(result,"成功")==0,result);
    struct Close { ~Close(){saga::setcjk_ex(nullptr,0,0,nullptr,0,0);} } close;
    CaptureFrame frame;unsigned long long sequence=0;
    for(int i=0;i<20;++i)require(captureAcquire(frame,sequence,2000),"live warm-up timeout");
    frame={};
    const auto first=sequence;
    std::atomic<bool> done{false},failed{false};std::atomic<unsigned> shots{0},previews{0},changes{0};
    std::vector<double> shotTimes;
    std::thread screenshots([&]{
        try {
            screenshot::Writer writer;std::vector<BYTE> jpeg;unsigned long long seq=0;
            while(!done){
                CaptureFrame f;
                if(captureAcquire(f,seq,20)){
                    const auto begin=Clock::now();require(screenshot::encode(f,{},jpeg),"live encode");f={};
                    if(slow)Sleep(150); // Simulated stalled disk, after releasing the capture lease.
                    require(writer.save(directory,"截图压力",shots.load()%5,jpeg),"live write");
                    shotTimes.push_back(std::chrono::duration<double,std::milli>(Clock::now()-begin).count());++shots;
                }
                Sleep(80);
            }
        }catch(...){failed=true;}
    });
    std::thread preview([&]{
        unsigned long long seq=0;
        while(!done){CaptureFrame f;if(captureAcquire(f,seq,20)){
            encoded_image::BgrView view;
            if(!encoded_image::topDownBgr(f.bmp,f.size,view)||view.width!=f.width||view.height!=f.height)failed=true;
            const BYTE firstPixel=f.bmp[54];Sleep(20);if(f.bmp[54]!=firstPixel)failed=true;++previews;
        }}
    });
    std::thread cropper([&]{
        if(!dynamic)return;
        const int sizes[]={160,161,320,321,639,640};unsigned index=0;
        while(!done){if(!captureSetCrop(sizes[index++%6]))failed=true;++changes;Sleep(80);}
    });
    const auto started=Clock::now();unsigned count=0;std::uint64_t hash=1469598103934665603ull;
    while(Clock::now()-started<std::chrono::seconds(seconds)) {
        if(!captureAcquire(frame,sequence,100))continue;
        encoded_image::BgrView view;
        if(!encoded_image::topDownBgr(frame.bmp,frame.size,view)||view.width!=frame.width||view.height!=frame.height)failed=true;
        for(int y=0;y<frame.height;y+=17)for(int x=0;x<frame.width*3;x+=37){hash^=frame.bmp[54+y*frame.stride+x];hash*=1099511628211ull;}
        frame={};++count;
    }
    const auto elapsed=std::chrono::duration<double>(Clock::now()-started).count();
    unsigned long long published=0;{Lock lock(g_snapshotLock);published=g_captureSequence-first;}
    done=true;screenshots.join();preview.join();cropper.join();
    require(!failed&&count>0&&shots>0&&previews>0,"live corruption/encode/write failure");
    std::sort(shotTimes.begin(),shotTimes.end());
    std::printf("LIVE format=%s width=%d height=%d fps=%d crop=%d dynamic=%d slow_disk_ms=%d seconds=%.3f frames=%u published=%llu skipped=%llu observed_fps=%.3f screenshots=%u preview=%u crop_changes=%u shot_p50_ms=%.3f shot_p95_ms=%.3f shot_max_ms=%.3f hash=%llu\n",
        format,width,height,fps,crop,dynamic,slow?150:0,elapsed,count,published,published-count,count/elapsed,shots.load(),previews.load(),changes.load(),
        shotTimes[shotTimes.size()/2],shotTimes[(shotTimes.size()-1)*95/100],shotTimes.back(),static_cast<unsigned long long>(hash));
}

int main(int argc,char** argv) {
    try {
        std::printf("AVX512_enabled=%d\n",capture_pixels::detail::wide_vectors_available());
        if(argc>1&&strcmp(argv[1],"--list")==0){std::printf("%s\n",saga::enum_device_formats("Live Gamer Ultra 2.1-Video"));return 0;}
        if(argc>1&&strcmp(argv[1],"--bench")==0){benchmark();return 0;}
        if(argc>1&&strcmp(argv[1],"--live")==0){live(argc,argv);return 0;}
        guardedRows();exhaustive();frames();concurrent();return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
