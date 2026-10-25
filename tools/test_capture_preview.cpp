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
bool startCapture(const char*, int, int, const char*, int) { std::abort(); }
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
            g.imgSize=size;
            sizeComboShowApplied();
            require(SendMessageA(g_cbSize,CB_GETCURSEL,0,0)==(size-160)/32,"Live crop display mismatch");
            const int writes=comboWrites;
            for(int repeat=0;repeat<100;++repeat) sizeComboShowApplied();
            require(comboWrites==writes,"Unchanged crop must not rewrite the UI");
        }
        require(comboWrites==16,"Exactly one update per new stage expected");
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
