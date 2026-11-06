// Exercises the production screenshot loop with deterministic frame/time inputs;
// writes only under a new isolated test directory beside this executable.
#include "../src/Capture.cpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <functional>
#include <random>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

AppState g;
CRITICAL_SECTION g_frameLock;
CONDITION_VARIABLE g_publishedFrameReady = CONDITION_VARIABLE_INIT;
std::vector<unsigned char> g_frame;
long long g_frameSeq = 0;
std::atomic<long long> g_lastFrameTime{1000}, g_nowMs{0};
static long long fakeNow = 1000;
static CaptureFrame available;
static unsigned long long availableSequence = 1;
static std::function<void()> afterTick;
namespace saga { void set_capture_log(void (*)(const char*)) { std::abort(); } }
namespace runtime_log { void write(const char*, ...) { std::abort(); } }
void canvasDrawCapture(const CaptureFrame&) { std::abort(); }
void canvasDraw(const std::vector<unsigned char>&) { std::abort(); }
void RunTimer::start() { std::abort(); }
double RunTimer::ms() const { std::abort(); }
std::string ansiToUtf8(const std::string&) { std::abort(); }
void optThread() { std::abort(); }
bool sagaSetCapture(const char*, int, int, const char*, int, int, std::string&) { std::abort(); }
void inferPause(bool) { std::abort(); }
void uiSetShowImg(bool) { std::abort(); }
long long nowMs() { return fakeNow; }
void sleepMs(int) { if (afterTick) afterTick(); else g.running = false; }
bool captureAcquire(CaptureFrame& frame, unsigned long long& sequence, unsigned) {
    if (!available.owner || sequence == availableSequence) return false;
    frame = available; sequence = availableSequence; return true;
}

static std::vector<unsigned char> bmp(int width, bool bottomUp = false) {
    const size_t stride = (width * 3 + 3) & ~size_t(3);
    std::vector<unsigned char> bytes(54 + stride * width, 0);
    BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfSize = static_cast<DWORD>(bytes.size()); file.bfOffBits = 54;
    BITMAPINFOHEADER info{}; info.biSize = 40; info.biWidth = width; info.biHeight = bottomUp ? width : -width;
    info.biPlanes = 1; info.biBitCount = 24; info.biSizeImage = static_cast<DWORD>(stride * width);
    memcpy(bytes.data(), &file, sizeof(file)); memcpy(bytes.data()+14, &info, sizeof(info));
    std::mt19937 random(width);
    for (size_t i = 54; i < bytes.size(); ++i) bytes[i] = static_cast<unsigned char>(random());
    return bytes;
}
static CaptureFrame lease(const std::vector<unsigned char>& bytes) {
    CaptureFrame frame;
    frame.owner = std::shared_ptr<unsigned char>(new unsigned char[bytes.size()], std::default_delete<unsigned char[]>());
    memcpy(frame.owner.get(), bytes.data(), bytes.size()); frame.bmp = frame.owner.get(); frame.size = static_cast<int>(bytes.size());
    encoded_image::BgrView view;
    assert(encoded_image::topDownBgr(bytes.data(), bytes.size(), view));
    frame.width = view.width; frame.height = view.height; frame.stride = view.stride;
    return frame;
}
static std::vector<unsigned char> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static size_t files(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return 0;
    return std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{});
}

int main(int argc, char**) {
    _set_error_mode(_OUT_TO_STDERR);
    InitializeCriticalSection(&g_frameLock);
    const auto root = file_paths::executableDirectory() /
        (L"screenshot-test-中文-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    assert(std::filesystem::create_directory(root));
    const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, 95};
    std::vector<unsigned char> jpeg, reference;
    unsigned cases = 0;
    for (int size : {1, 2, 3, 7, 15, 16, 17, 31, 32, 33, 159, 160, 161, 319, 320, 321, 639, 640}) {
        for (bool bottomUp : {false, true}) {
            const auto bytes = bmp(size, bottomUp);
            auto pixels = cv::imdecode(bytes, cv::IMREAD_COLOR);
            assert(cv::imencode(".jpg", pixels, reference, params));
            assert(screenshot::encode({}, bytes, jpeg) && jpeg == reference);
            if (!bottomUp) assert(screenshot::encode(lease(bytes), {}, jpeg) && jpeg == reference);
            assert(screenshot::encode({}, reference, jpeg));
            std::vector<unsigned char> compressedReference;
            assert(cv::imencode(".jpg", cv::imdecode(reference, cv::IMREAD_COLOR), compressedReference, params));
            assert(jpeg == compressedReference);
            ++cases;
        }
    }
    assert(!screenshot::encode({}, {}, jpeg));
    assert(!screenshot::encode({}, {1,2,3,4}, jpeg));
    auto invalid = lease(bmp(16)); invalid.size = 54;
    assert(!screenshot::encode(invalid, {}, jpeg));

    screenshot::Writer writer;
    const auto bytes = bmp(320);
    assert(screenshot::encode(lease(bytes), {}, jpeg));
    assert(writer.save(root, "截图", 1000, jpeg));
    const auto directory = root / L"截图", saved = directory / L"1000.jpg";
    assert(read(saved) == jpeg);
    assert(SetFileAttributesW(saved.c_str(), FILE_ATTRIBUTE_READONLY));
    assert(!writer.save(root, "截图", 1000, {1,2,3}));
    assert(read(saved) == jpeg);
    assert(SetFileAttributesW(saved.c_str(), FILE_ATTRIBUTE_NORMAL));
    assert(DeleteFileW(saved.c_str()) && RemoveDirectoryW(directory.c_str()));
    assert(writer.save(root, "截图", 1001, jpeg)); // Cached directory can be recreated.
    assert(writer.save(root, "另一个模型", 1002, jpeg));
    const auto longRoot = file_paths::io(root / std::wstring(100, L'a') / std::wstring(100, L'b') / L"深层目录");
    assert(std::filesystem::create_directories(longRoot));
    assert(writer.save(longRoot, "截图", 1003, jpeg));
    assert(read(longRoot / L"截图" / L"1003.jpg") == jpeg);
    assert(!writer.save(root, "不存在的父目录/子目录", 1004, jpeg));
    assert(!writer.save(root, "截图", 1005, {}));

    // Exercise actual eligibility, throttle, repeated-frame suppression and
    // resumption for local capture and UDP, with JPEG bytes verified on disk.
    g.runDir = root;
    auto settings = g.settings.snapshot(); settings.autoShot = true; g.settings.replace(settings);
    for (bool direct : {true, false}) {
        g.modelName = direct ? "本机循环" : "UDP循环";
        g.captureRunning = direct; g.aimKeyDown = true;
        available = lease(bytes); availableSequence = 1;
        g_frame = bytes; g_frameSeq = 1;
        fakeNow = 1000; g.lastDetectTime = 1000; g_lastFrameTime = 1000;
        int tick = 0;
        const auto output = root / file_paths::wide(g.modelName);
        afterTick = [&] {
            ++tick;
            const size_t expected = tick < 5 ? 1 : 2;
            assert(files(output) == expected);
            assert(available.owner.use_count() == 1);
            if (tick == 1) { fakeNow += 80; ++availableSequence; ++g_frameSeq; } // Strictly >80.
            if (tick == 2) { fakeNow += 1; g_lastFrameTime = fakeNow - 125; } // Stale frame.
            if (tick == 3) { g_lastFrameTime = fakeNow; g.lastDetectTime = fakeNow - 125; }
            if (tick == 4) { g.lastDetectTime = fakeNow; Sleep(3); } // Fresh new frame accepted.
            if (tick == 5) { fakeNow += 81; g.lastDetectTime = g_lastFrameTime = fakeNow; }
            if (tick == 6) { g.aimKeyDown = false; ++availableSequence; ++g_frameSeq; }
            if (tick == 7) g.running = false;
        };
        g.running = true; frameThreadShotSave();
        for (const auto& file : std::filesystem::directory_iterator(output)) assert(read(file.path()) == jpeg);
    }
    afterTick = {};
    std::cout << "PASS " << cases << " JPEG95 pixel/byte cases, padded rows, malformed inputs, Unicode/long paths, failed writes, folder recreation, local/UDP freshness, throttle and duplicate frames\n";
    if (argc > 1) {
        for (int size : {160,320,640}) {
            const auto input = bmp(size);
            for (bool fast : {false,true,false,true}) {
                auto begin = std::chrono::steady_clock::now();
                for (int i=0;i<500;++i) {
                    if (fast) screenshot::encode({},input,jpeg);
                    else cv::imencode(".jpg",cv::imdecode(input,cv::IMREAD_COLOR),jpeg,params);
                }
                std::cout << "UDP_BMP_JPEG size="<<size<<" view="<<fast<<" us="
                    << std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/500 << '\n';
            }
        }
    }
    DeleteCriticalSection(&g_frameLock);
}
