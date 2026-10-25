// Build: ./tools/build_performance.ps1 -Source tools/test_capture_preview.cpp -Name test_capture_preview
// Exercises the production preview painter with real GDI pixels and a stalled
// render thread, without starting inference, networking or mouse control.
#include "../src/Ui.cpp"
#include <stdexcept>

AppState g;
CRITICAL_SECTION g_drawLock;
CRITICAL_SECTION g_moveLock;
namespace runtime_log { void write(const char*, ...) {} }
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

// Unrelated application services must never run in this preview-only test.
int roundToInt(double value) { return static_cast<int>(std::round(value)); }
bool fileExists(const std::filesystem::path&) { std::abort(); }
std::string trimStr(const std::string&) { std::abort(); }
std::string ansiToUtf8(const std::string&) { std::abort(); }
std::string utf8ToAnsi(const std::string&) { std::abort(); }
bool sagaEnumDevices(std::string&) { std::abort(); }
bool sagaEnumFormats(const char*, std::string&) { std::abort(); }
void makcuMove(float, float) { std::abort(); }
bool trajReady() { std::abort(); }
bool trajReloadIfChanged() { std::abort(); }
void trajMove10(float, float) { std::abort(); }
static std::atomic<int> restoredCaptures{0};
bool startCapture(const char* device, int width, int height, const char* codec, int fps) {
    if (std::string(device)!="fixture-device" || width!=1920 || height!=1080 ||
        std::string(codec)!="NV12" || fps!=240) std::abort();
    ++restoredCaptures;return true;
}
bool applyCropSize(int) { std::abort(); }
void boxStopKeyboard() { std::abort(); }
void boxConfigure(HWND) { std::abort(); }
const char* boxStatus() { std::abort(); }
void startHumanTrajectoryTool(HWND, bool) { std::abort(); }
std::wstring webAccessAddress() { std::abort(); }
void webShowDiagnostics(HWND) { std::abort(); }
void webOpenLocalPage(HWND) { std::abort(); }

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static WNDPROC comboProcedure = nullptr;
static int comboWrites = 0;
static WNDPROC labelProcedure = nullptr;
static int labelWrites = 0;
static LRESULT CALLBACK countLabelWrites(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_SETTEXT) ++labelWrites;
    return CallWindowProcA(labelProcedure, window, message, w, l);
}
static LRESULT CALLBACK countComboWrites(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == CB_SETCURSEL) ++comboWrites;
    return CallWindowProcA(comboProcedure, window, message, w, l);
}

struct StalledRender {
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE resume = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    StalledRender() : worker([this] {
        AcquireSRWLockExclusive(&g_canvasLock);
        boardClear();
        g_hasFrame = false;
        SetEvent(ready);
        WaitForSingleObject(resume, 5000);
        ReleaseSRWLockExclusive(&g_canvasLock);
    }) {}
    ~StalledRender() {
        SetEvent(resume);
        worker.join();
        CloseHandle(ready);
        CloseHandle(resume);
    }
};

int main() {
    try {
        InitializeCriticalSection(&g_drawLock);
        InitializeCriticalSection(&g_moveLock);
        g.showImg = true;
        g_panelBrush = CreateSolidBrush(RGB(247,249,252));
        g_uiFont = g_uiFontBold = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "CapturePreviewRegression";
        wc.hbrBackground = g_panelBrush;
        RegisterClassA(&wc);
        HWND window = CreateWindowA(wc.lpszClassName, "", WS_POPUP,
            0, 0, 1080, 792, nullptr, nullptr, wc.hInstance, nullptr);
        require(window != nullptr, "test window creation failed");
        HDC target = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 1080;
        info.bmiHeader.biHeight = -792;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        require(target && bitmap && pixels, "test surface creation failed");
        auto oldBitmap = SelectObject(target, bitmap);
        const auto checkPreview = [&](const std::vector<unsigned char>& expected) {
            GdiFlush();
            for (int y = 0; y < kBoard; ++y) {
                const auto* row = static_cast<unsigned char*>(pixels) + ((y + kViewY) * 1080 + kViewX) * 4;
                // The DIB alpha byte is not part of the displayed GDI color.
                for (int x = 0; x < kBoard; ++x)
                    require(std::memcmp(row + x * 4, expected.data() + (y * kBoard + x) * 4, 3) == 0,
                        "preview pixels were erased, incomplete or not repainted");
            }
        };
        boardPaint(target);
        require(GetPixel(target, kViewX + 10, kViewY + 10) == RGB(17,25,40), "initial placeholder missing");

        // Independent expected pixels, with a deliberately padded input stride.
        // Check every scanline, not just that the window copied its own bitmap.
        std::vector<unsigned char> padded(640 * (640 * 3 + 32), 0xcd);
        std::vector<unsigned char> reference(640 * 640 * 4, 255);
        for (int y = 0; y < 640; ++y) {
            for (int x = 0; x < 640; ++x) {
                auto* src = padded.data() + y * (640 * 3 + 32) + x * 3;
                src[0] = static_cast<BYTE>(x + y * 3);
                src[1] = static_cast<BYTE>(y + x * 5);
                src[2] = static_cast<BYTE>(x ^ y);
                std::memcpy(reference.data() + (y * 640 + x) * 4, src, 3);
            }
        }
        boardInit(); boardClear();
        boardDrawPixels(cv::Mat(640, 640, CV_8UC3, padded.data(), 640 * 3 + 32));
        boardPaint(target);
        checkPreview(reference);

        std::vector<unsigned char> input(54 + 320 * 320 * 3);
        for (int y = 0; y < 320; ++y)
            for (int x = 0; x < 320; ++x) {
                auto* p = input.data() + 54 + (y * 320 + x) * 3;
                p[0] = static_cast<unsigned char>(x); p[1] = static_cast<unsigned char>(y); p[2] = 149;
            }
        CaptureFrame frame;
        frame.bmp = input.data(); frame.size = static_cast<int>(input.size());
        frame.width = frame.height = 320; frame.stride = 320 * 3;
        canvasDrawCapture(frame);
        const auto* rendered = static_cast<unsigned char*>(g_bits);
        std::vector<unsigned char> expected(rendered, rendered + kBoard * kBoard * 4);
        boardPaint(target);
        checkPreview(expected); // Also catches a flipped DIB or wrong channel order.

        require(wndProc(window, WM_ERASEBKGND, reinterpret_cast<WPARAM>(target), 0) == 1, "background erase failed");
        checkPreview(expected);
        require(GetPixel(target, 10, 10) == RGB(247,249,252), "non-preview background was not erased");
        require(PtVisible(target, kViewX + 10, kViewY + 10), "erase left the DC clip changed");
        {
            StalledRender stalled;
            require(WaitForSingleObject(stalled.ready, 2000) == WAIT_OBJECT_0, "render thread did not start");
            const auto started = GetTickCount64();
            for (int i = 0; i < 100; ++i) {
                // Simulate lost window contents on uncover/restore while the
                // producer has cleared its working bitmap but cannot finish.
                std::memset(pixels, 0xee, 1080 * 792 * 4);
                wndProc(window, WM_ERASEBKGND, reinterpret_cast<WPARAM>(target), 0);
                boardPaint(target);
                checkPreview(expected);
            }
            require(GetTickCount64() - started < 2000, "painting waited on the render thread");
            g.showImg = false;
            boardPaint(target);
            require(GetPixel(target, kViewX + 10, kViewY + 10) == RGB(17,25,40), "pause ignored during render stall");
            g.showImg = true;
            boardPaint(target);
            checkPreview(expected);
        }
        boardPaint(target);
        checkPreview(expected); // Failed/missing next frame retains the completed one.
        std::fill(input.begin() + 54, input.end(), 37);
        canvasDrawCapture(frame);
        expected.assign(rendered, rendered + kBoard * kBoard * 4);
        boardPaint(target);
        checkPreview(expected);

        g_cbSize = CreateWindowA("COMBOBOX", "", WS_CHILD | CBS_DROPDOWNLIST,
            0, 0, 200, 200, window, nullptr, GetModuleHandleA(nullptr), nullptr);
        require(g_cbSize != nullptr,"Create size display fixture");
        for (int size=160; size<=640; size+=32) {
            const auto label=std::to_string(size);
            SendMessageA(g_cbSize, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        comboProcedure = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_cbSize,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(countComboWrites)));
        for (int size=160; size<=640; size+=32) {
            g.manualCropSize=size;
            sizeComboShowApplied();
            require(SendMessageA(g_cbSize,CB_GETCURSEL,0,0)==(size-160)/32,"Live crop display mismatch");
            const int writes=comboWrites;
            for(int repeat=0;repeat<100;++repeat) sizeComboShowApplied();
            require(comboWrites==writes,"Unchanged crop must not rewrite the UI");
        }
        require(comboWrites==16,"Exactly one update per new stage expected");
        g.manualCropSize=416; sizeComboShowApplied();
        g_currentCropLabel=CreateWindowA("STATIC","",WS_CHILD,0,210,360,26,window,nullptr,GetModuleHandleA(nullptr),nullptr);
        require(g_currentCropLabel!=nullptr,"Create live crop label");
        labelProcedure=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_currentCropLabel,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(countLabelWrites)));
        const int manualWrites=comboWrites;
        g.adaptiveCapture=true;
        for(int size=160;size<=640;size+=32) {
            g.imgSize=size; currentCaptureShow();
            char actual[96]{},expectedLabel[96]{};
            GetWindowTextA(g_currentCropLabel,actual,sizeof(actual));
            snprintf(expectedLabel,sizeof(expectedLabel),"当前截图范围：%d × %d 像素",size,size);
            require(strcmp(actual,expectedLabel)==0,"Live label must track adaptive size");
            const int writes=labelWrites;
            for(int repeat=0;repeat<100;++repeat) currentCaptureShow();
            require(labelWrites==writes,"Unchanged live label must not repaint");
            require(g.manualCropSize==416 && SendMessageA(g_cbSize,CB_GETCURSEL,0,0)==8,"Adaptive range changed manual selection");
        }
        require(comboWrites==manualWrites && labelWrites==16,"Adaptive updates must write only the live label");
        require(!IsWindowEnabled(g_cbSize),"Adaptive mode must disable manual range");
        sizeComboCommit(); // No task or popup in adaptive mode.
        g.adaptiveCapture=false; currentCaptureShow();
        require(IsWindowEnabled(g_cbSize),"Single model must restore manual range control");
        const auto sessionDir=std::filesystem::temp_directory_path()/(L"saga-restore-ui-"+std::to_wstring(GetCurrentProcessId()));
        require(std::filesystem::create_directory(sessionDir),"Create startup fixture directory");
        g.runDir=sessionDir; g_hwnd=window;
        g_cbDevice=CreateWindowA("COMBOBOX","",WS_CHILD|CBS_DROPDOWNLIST,0,0,200,200,window,nullptr,wc.hInstance,nullptr);
        g_cbFormat=CreateWindowA("COMBOBOX","",WS_CHILD|CBS_DROPDOWNLIST,0,0,200,200,window,nullptr,wc.hInstance,nullptr);
        uiRestoreCapture();
        require(!captureTaskBusy && restoredCaptures==0,"Fresh install must not auto-start capture");
        require(startup_session::save(sessionDir/L"配置保存.ini",{"fixture-device","NV12",1920,1080,240},416),"Save restore fixture");
        for(bool preview:{false,true}) {
            uiSetShowImg(preview);
            uiRestoreCapture();
            require(g.showImg==preview && g.noConvert,"Restore must preserve preview and select capture input");
            const auto deadline=GetTickCount64()+3000;
            bool completed=false;
            while(GetTickCount64()<deadline && !completed) {
                MSG message{};
                if(PeekMessageA(&message,window,kCaptureTaskDone,kCaptureTaskDone,PM_REMOVE)) {
                    require(message.wParam!=0,"Restore task failed");
                    wndProc(window,message.message,message.wParam,message.lParam);
                    completed=true;
                } else Sleep(1);
            }
            require(completed && !captureTaskBusy,"Startup capture task must complete asynchronously");
            require(file_paths::profileInt("默认","选择框_实时刷新",9,(sessionDir/L"配置保存.ini").c_str())==int(preview),"Preview toggle must persist immediately");
        }
        require(restoredCaptures==2,"Each launch must restore capture exactly once");
        DestroyWindow(g_cbDevice);DestroyWindow(g_cbFormat);g_cbDevice=g_cbFormat=nullptr;g_hwnd=nullptr;
        std::filesystem::remove(sessionDir/L"配置保存.ini");std::filesystem::remove(sessionDir);
        DestroyWindow(g_currentCropLabel);g_currentCropLabel=nullptr;
        DestroyWindow(g_cbSize); g_cbSize=nullptr;
        SelectObject(target, oldBitmap);
        DeleteObject(bitmap); DeleteDC(target); DestroyWindow(window);
        DeleteObject(g_panelBrush); DeleteCriticalSection(&g_drawLock);
        std::puts("PASS: erase exclusion, pixel orientation, 100 stalled-render repaints, pause/resume and next-frame refresh");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
