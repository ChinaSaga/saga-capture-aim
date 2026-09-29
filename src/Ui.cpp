#include "App.h"
#include "AppExit.h"
#include "HumanTrajectory.h"
#include "WebAccess.h"
#include "GpuIdTable.h"     // 硬件 PCI 设备 ID → 真实显卡型号（注册表改名也不认）
#include "GpuNameTable.h"   // 显卡名称简化（面子工程）

#include <opencv2/opencv.hpp>
#include <mmsystem.h>
#include <shellapi.h>
#include <ctime>
#include <commctrl.h>
#include <uxtheme.h>
#include <dxgi.h>          // 显卡名称：直接问驱动要适配器描述（不读注册表字符串）
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

#pragma comment(lib, "winmm.lib")

// ============================================================================
//  画板（替代易语言 内存画板，640x640，32 位，top-down）
// ============================================================================
static const int kBoard = 640;
static const int kViewX = 24, kViewY = 104;

static HWND   g_hwnd = nullptr;
static HDC    g_memDC = nullptr;
static HBITMAP g_dib = nullptr;
static void*  g_bits = nullptr;
static HFONT  g_font = nullptr;
static HFONT  g_uiFont = nullptr;
static HFONT  g_uiFontBold = nullptr;
static HBRUSH g_panelBrush = nullptr;
static HFONT g_titleFont = nullptr;
static SRWLOCK g_canvasLock = SRWLOCK_INIT;
static bool g_hasFrame = false;
static double g_imageScale = 1.0;
static int g_imageX = 0, g_imageY = 0;
static constexpr UINT kSyncPreview = WM_APP + 41;
static constexpr UINT kSyncSizeCombo = WM_APP + 42;   // 取消时把下拉框显示退回生效值
static RECT previewRect() { return {kViewX, kViewY, kViewX + kBoard, kViewY + kBoard}; }
static void textAt(HDC dc, const char* text, RECT rect, HFONT font, COLORREF color,
    UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextA(dc, text, -1, &rect, flags);
    SelectObject(dc, old);
}
static void rounded(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius = 10) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    DeleteObject(brush); DeleteObject(pen);
}
static LRESULT CALLBACK controlProc(HWND h, UINT m, WPARAM w, LPARAM l,
    UINT_PTR subclass, DWORD_PTR) {
    if (m == WM_MOUSEMOVE) {
        if (!GetPropA(h, "SagaHover")) {
            SetPropA(h, "SagaHover", (HANDLE)1);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
            TrackMouseEvent(&track);
            InvalidateRect(h, nullptr, FALSE);
        }
    } else if (m == WM_MOUSELEAVE) {
        RemovePropA(h, "SagaHover");
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_ENABLE) {
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_NCDESTROY) {
        RemovePropA(h, "SagaHover");
        RemoveWindowSubclass(h, controlProc, subclass);
    }
    if (subclass == 2 && m == WM_PAINT) {
        // Retain the native combo interaction and popup, paint only its closed face.
        LRESULT result = DefSubclassProc(h, m, w, l);
        HDC dc = GetDC(h);
        RECT r; GetClientRect(h, &r);
        FillRect(dc, &r, g_panelBrush);
        bool enabled = IsWindowEnabled(h) != FALSE;
        bool focus = GetFocus() == h;
        rounded(dc, r, enabled ? RGB(255,255,255) : RGB(239,242,247),
            focus ? RGB(49,102,218) : RGB(207,216,229), 8);
        char value[512] = {};
        GetWindowTextA(h, value, sizeof(value));
        if (!value[0]) strcpy_s(value, GetDlgCtrlID(h) == 200 ? "请先初始化采集卡" : "请选择视频格式");
        RECT text = r; text.left += 12; text.right -= 34;
        textAt(dc, value, text, g_uiFont, enabled ? RGB(35,50,72) : RGB(143,154,171),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        HPEN pen = CreatePen(PS_SOLID, 2, enabled ? RGB(91,111,139) : RGB(170,180,194));
        auto old = SelectObject(dc, pen);
        int x = r.right - 20, y = (r.bottom + r.top) / 2;
        MoveToEx(dc, x - 4, y - 2, nullptr); LineTo(dc, x, y + 2); LineTo(dc, x + 4, y - 2);
        SelectObject(dc, old); DeleteObject(pen);
        ReleaseDC(h, dc);
        return result;
    }
    return DefSubclassProc(h, m, w, l);
}

static HWND   g_cbDevice = nullptr;
static HWND   g_cbFormat = nullptr;
static HWND   g_cbSize = nullptr;
static HWND   g_chkRefresh = nullptr;


static void boardInit()
{
    if (g_memDC) return;

    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);

    BITMAPINFOHEADER bi;
    std::memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = kBoard;
    bi.biHeight = -kBoard;              // 负值 = top-down
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    g_dib = CreateDIBSection(g_memDC, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    SelectObject(g_memDC, g_dib);

    // Fixed native-pixel label font; never stretch the text with the source image.
    int fontHeight = -14;
    g_font = CreateFontA(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
}

static void boardClear()
{
    if (!g_bits) return;
    std::memset(g_bits, 0, (size_t)kBoard * kBoard * 4);
}

// Fit BGR pixels into the fixed preview. The canvas lock protects the reusable
// resize buffer, and cvtColor writes directly into the DIB destination stride.
static void boardDrawPixels(const cv::Mat& image)
{
    g_hasFrame = false;
    if (!g_bits || image.empty()) return;
    try {
        g_imageScale = std::min(double(kBoard) / image.cols, double(kBoard) / image.rows);
        int width = (std::max)(1, (int)std::round(image.cols * g_imageScale));
        int height = (std::max)(1, (int)std::round(image.rows * g_imageScale));
        g_imageX = (kBoard - width) / 2; g_imageY = (kBoard - height) / 2;
        static cv::Mat resized;
        const cv::Mat* pixels = &image;
        if (image.cols != width || image.rows != height) {
            cv::resize(image, resized, cv::Size(width, height), 0, 0,
                g_imageScale < 1 ? cv::INTER_AREA : cv::INTER_LINEAR);
            pixels = &resized;
        }
        cv::Mat destination(height, width, CV_8UC4,
            (BYTE*)g_bits + ((size_t)g_imageY * kBoard + g_imageX) * 4,
            (size_t)kBoard * 4);
        cv::cvtColor(*pixels, destination, cv::COLOR_BGR2BGRA);
        g_hasFrame = true;
    } catch (const cv::Exception&) {
        OutputDebugStringA("Preview decode failed.\n");
    }
}

static void boardDrawImage(const std::vector<unsigned char>& encoded)
{
    g_hasFrame = false;
    if (!g_bits || encoded.empty()) return;
    try {
        boardDrawPixels(cv::imdecode(encoded, cv::IMREAD_COLOR));
    } catch (const cv::Exception&) {
        OutputDebugStringA("Preview decode failed.\n");
    }
}

// 接口_画板画个大点
static void drawBigPoint(int x, int y, COLORREF c)
{
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            SetPixel(g_memDC, x + dx, y + dy, c);

    SetPixel(g_memDC, x - 2, y, c);
    SetPixel(g_memDC, x + 2, y, c);
    SetPixel(g_memDC, x, y - 2, c);
    SetPixel(g_memDC, x, y + 2, c);

    HBRUSH old = (HBRUSH)SelectObject(g_memDC, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN oldPen = (HPEN)SelectObject(g_memDC, pen);
    for (int r = 2; r <= 4; ++r)
        Ellipse(g_memDC, x - r, y - r, x + r, y + r);
    SelectObject(g_memDC, oldPen);
    DeleteObject(pen);
    SelectObject(g_memDC, old);
}

static void canvasDrawImpl(const std::vector<unsigned char>* encoded, const CaptureFrame* raw)
{
    AcquireSRWLockExclusive(&g_canvasLock);
    boardInit();
    boardClear();
    if (raw)
        boardDrawPixels(cv::Mat(raw->height, raw->width, CV_8UC3,
            const_cast<unsigned char*>(raw->bmp + 54), raw->stride));
    else
        boardDrawImage(*encoded);
    if (g_hasFrame) {
        const auto px = [](double value) { return g_imageX + (int)std::round(value * g_imageScale); };
        const auto py = [](double value) { return g_imageY + (int)std::round(value * g_imageScale); };
        HBRUSH oldBrush = (HBRUSH)SelectObject(g_memDC, GetStockObject(NULL_BRUSH));
        DrawBox drawSnapshot[99];
        EnterCriticalSection(&g_drawLock);
        const bool canDraw = g.canDraw;
        if (canDraw) std::memcpy(drawSnapshot, g.boxes, sizeof(drawSnapshot));
        LeaveCriticalSection(&g_drawLock);
        // GDI and label formatting must not hold up inference publication.
        if (canDraw) {
            for (int i = 0; i < 99; ++i) {
                const DrawBox& box = drawSnapshot[i];
                if (!box.color) break;
                int left = (std::clamp)(px(box.x1), 0, kBoard - 1);
                int top = (std::clamp)(py(box.y1), 0, kBoard - 1);
                int right = (std::clamp)(px(box.x2), 0, kBoard - 1);
                int bottom = (std::clamp)(py(box.y2), 0, kBoard - 1);
                if (right <= left || bottom <= top) continue;
                HPEN outline = CreatePen(PS_SOLID, 4, RGB(15,23,42));
                auto oldPen = SelectObject(g_memDC, outline);
                Rectangle(g_memDC, left, top, right, bottom);
                SelectObject(g_memDC, oldPen); DeleteObject(outline);
                HPEN pen = CreatePen(PS_SOLID, 2, (COLORREF)box.color);
                oldPen = SelectObject(g_memDC, pen);
                Rectangle(g_memDC, left, top, right, bottom);
                SelectObject(g_memDC, oldPen); DeleteObject(pen);
                char label[80];
                snprintf(label, sizeof(label), "%d  %.2f", box.category,
                    (std::clamp)(box.confidence, 0.0f, 1.0f));
                auto oldFont = SelectObject(g_memDC, g_font);
                SIZE extent; GetTextExtentPoint32A(g_memDC, label, (int)strlen(label), &extent);
                SelectObject(g_memDC, oldFont);
                const int width = extent.cx + 14, height = 24;
                int x = (std::clamp)(left, 0, kBoard - width);
                int y = top >= height ? top - height : top;
                RECT badge{x, y, x + width, y + height};
                rounded(g_memDC, badge, (COLORREF)box.color, (COLORREF)box.color, 4);
                badge.left += 7;
                textAt(g_memDC, label, badge, g_font, RGB(8,18,32));
            }
        }
        const int half = g.imgSize / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(255,222,89));
        auto oldPen = SelectObject(g_memDC, pen);
        Ellipse(g_memDC, px(half - g.lockRange), py(half - g.lockRange),
            px(half + g.lockRange), py(half + g.lockRange));
        MoveToEx(g_memDC, px(half), py(half), nullptr);
        LineTo(g_memDC, px(half + g.moveDx), py(half + g.moveDy));
        SelectObject(g_memDC, oldPen); DeleteObject(pen);
        drawBigPoint(px(half), py(half), RGB(255,222,89));
        drawBigPoint(px(half + g.boxDx), py(half + g.boxDy), RGB(255,222,89));
        SelectObject(g_memDC, oldBrush);
    }
    // Complete GDI operations before allowing the UI thread to read this bitmap.
    GdiFlush();
    ReleaseSRWLockExclusive(&g_canvasLock);
    if (g_hwnd) { RECT r = previewRect(); InvalidateRect(g_hwnd, &r, FALSE); }
}

void canvasDraw(const std::vector<unsigned char>& frame)
{
    canvasDrawImpl(&frame, nullptr);
}

void canvasDrawCapture(const CaptureFrame& frame)
{
    canvasDrawImpl(nullptr, &frame);
}

// ============================================================================
//  声音
// ============================================================================
void soundInit()
{
    // 音频文件已随程序放在运行目录
}

void soundPlayCamp(int camp)
{
    if (camp != 1 && camp != 2) return;
    std::string p = g.runDir + "\\声音_切换阵营" + (camp == 1 ? "1" : "2") + ".wav";
    if (fileExists(p))
        PlaySoundA(p.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

// ============================================================================
//  状态栏显示
//  1) 推理帧率的「显示值」：F1 显示图片时沿用原统计口径（每 100 次推理的实测帧率）；
//     F1 不显示图片时改用亚毫秒精度的单次推理耗时换算（推理 2ms → 500 FPS），
//     只影响界面显示，推理线程本身的节奏完全不变。
//  2) 显卡名称：用 DXGI 直接问驱动要适配器描述，并用硬件侧 PCI VendorId 做校验，
//     注册表被改过名（比如 A 卡伪装成 N 卡）时能识别出来并回退成厂商 + PCI ID。
// ============================================================================
static int displayInferFps()
{
    if (g.showImg.load()) return g.inferFps;        // F1 显示图片：保持原样

    double ms = g.inferMsPrecise;                   // F1 不显示图片：按实时推理耗时换算
    if (ms <= 0.0) ms = (double)g.inferMs;
    if (ms <= 0.0) return 0;                        // 还没跑过推理

    const int fps = roundToInt(1000.0 / ms);        // 2ms→500 / 0.4ms→2500，整数
    return fps > 0 ? fps : 1;
}

static std::string ansiFromWide(const wchar_t* w)
{
    if (!w || !*w) return std::string();
    const int need = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out((size_t)need - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, w, -1, &out[0], need, nullptr, nullptr);
    return out;
}

static char upperAscii(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

static bool textHasNoCase(const std::string& text, const char* key)
{
    const size_t n = strlen(key);
    if (!n || text.size() < n) return false;
    for (size_t i = 0; i + n <= text.size(); ++i)
    {
        size_t k = 0;
        while (k < n && upperAscii(text[i + k]) == upperAscii(key[k])) ++k;
        if (k == n) return true;
    }
    return false;
}

// 硬件侧 PCI 厂商 ID（不可篡改），兼容 N / A / I 三家及常见其它厂商
static const char* gpuVendorById(unsigned vendorId)
{
    switch (vendorId)
    {
    case 0x10DE: return "NVIDIA";
    case 0x1002:
    case 0x1022: return "AMD";
    case 0x8086: return "Intel";
    case 0x13B5: return "ARM";
    case 0x106B: return "Apple";
    default:     return nullptr;
    }
}

// 这句驱动描述「看起来」是哪一家的；都没有命中返回 nullptr
static const char* gpuBrandByText(const std::string& text)
{
    static const char* kKeys[3][7] = {
        { "NVIDIA", "GEFORCE", "RTX", "GTX", "QUADRO", "TESLA", nullptr },
        { "AMD", "RADEON", "ATI", "VEGA", nullptr,  nullptr, nullptr },
        { "INTEL", "ARC", "IRIS", "UHD", nullptr,  nullptr, nullptr },
    };
    static const char* kNames[3] = { "NVIDIA", "AMD", "Intel" };
    for (int gi = 0; gi < 3; ++gi)
        for (int ki = 0; kKeys[gi][ki]; ++ki)
            if (textHasNoCase(text, kKeys[gi][ki])) return kNames[gi];
    return nullptr;
}

static char g_gpuName[160] = {};
static bool g_gpuQueried = false;

static void gpuQueryName()
{
    HMODULE lib = LoadLibraryW(L"dxgi.dll");
    if (!lib) return;

    typedef HRESULT(WINAPI* PFN_CreateFactory1)(REFIID, void**);
    typedef HRESULT(WINAPI* PFN_CreateFactory)(REFIID, void**);
    auto create1 = (PFN_CreateFactory1)GetProcAddress(lib, "CreateDXGIFactory1");
    auto create0 = (PFN_CreateFactory)GetProcAddress(lib, "CreateDXGIFactory");

    IDXGIFactory* factory = nullptr;
    if (create1) create1(__uuidof(IDXGIFactory1), (void**)&factory);
    if (!factory && create0) create0(__uuidof(IDXGIFactory), (void**)&factory);
    if (!factory) { FreeLibrary(lib); return; }

    DXGI_ADAPTER_DESC best{};
    bool found = false;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) != S_OK || !adapter) break;
        DXGI_ADAPTER_DESC desc{};
        // 0x1414 = Microsoft 基本显示适配器（WARP 软件渲染），不是真显卡
        if (SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId != 0 && desc.VendorId != 0x1414
            && (!found || desc.DedicatedVideoMemory > best.DedicatedVideoMemory))
        {
            best = desc;                 // 多显卡时取独立显存最大的那块（独显优先于核显）
            found = true;
        }
        adapter->Release();
    }
    factory->Release();
    FreeLibrary(lib);
    if (!found) return;

    const std::string desc   = trimStr(ansiFromWide(best.Description));
    const char*       vendor = gpuVendorById(best.VendorId);

    // ---- 真实型号：先用硬件 PCI 设备 ID 反查 ----
    // 驱动名（Description）和显存容量都来自注册表（DriverDesc / qwMemorySize），
    // 随手就能改成 "GeForce GTX 750" 之类；PCI 设备 ID 是硬件枚举的，改不了。
    // 驱动名里的型号数字跟硬件对不上（或干脆没有数字）→ 判定被改名了，用硬件表的名字。
    const std::string model = gpuid::resolveModelName(best.VendorId, best.DeviceId, desc);

    // 实在什么名字都没有 → 只能列厂商 + PCI ID
    if (model.empty())
    {
        char fallback[80];
        snprintf(fallback, sizeof(fallback), "%s (PCI %04X:%04X)",
            vendor ? vendor : "GPU", best.VendorId, best.DeviceId);
        strcpy_s(g_gpuName, fallback);
        return;
    }

    // 再兜一层：名字里的品牌跟硬件厂商 ID 对不上（A 卡被改成 N 卡之类）→ 只认硬件
    const char* brand = gpuBrandByText(model);
    if (vendor && brand && strcmp(vendor, brand) != 0)
    {
        char fallback[80];
        snprintf(fallback, sizeof(fallback), "%s (PCI %04X:%04X)",
            vendor, best.VendorId, best.DeviceId);
        strcpy_s(g_gpuName, fallback);
        return;
    }

    // 面子工程：按 GpuNameTable.h 压成短名（表里查不到走通用规则，实在不行保留全名）
    strcpy_s(g_gpuName, gpuname::shorten(model).c_str());
}

static const char* displayGpuName()
{
    if (!g_gpuQueried) { g_gpuQueried = true; gpuQueryName(); }
    return g_gpuName;
}

// 状态栏文字宽度测量 / 截断：显卡名太长时截成 "..."，保证它后面的 ms 不被挤掉
static int textWidth(HDC dc, const std::string& text)
{
    SIZE size{};
    auto old = SelectObject(dc, g_uiFont);
    GetTextExtentPoint32A(dc, text.c_str(), (int)text.size(), &size);
    SelectObject(dc, old);
    return (int)size.cx;
}

static std::string fitTextWidth(HDC dc, const std::string& text, int maxWidth)
{
    if (maxWidth <= 0 || text.empty()) return std::string();
    if (textWidth(dc, text) <= maxWidth) return text;
    std::string cut = text;
    while (!cut.empty())
    {
        cut.pop_back();
        if (!cut.empty() && IsDBCSLeadByte((BYTE)cut.back())) cut.pop_back();  // 别把 GBK 双字节切一半
        if (textWidth(dc, cut + "...") <= maxWidth) return cut + "...";
    }
    return std::string();
}

// 把下拉框的显示退回「真正生效的识别范围」（g.imgSize）——取消时用。
// 双机模式下 g.imgSize 可能是收到的画面宽度（不是 16 个档位之一），这时不动它。
static void sizeComboShowApplied()
{
    const int applied = g.imgSize;
    if (applied >= 160 && applied <= 640 && (applied - 160) % 32 == 0)
        SendMessageA(g_cbSize, CB_SETCURSEL, (WPARAM)((applied - 160) / 32), 0);
}

// 组合框_视频大小「确认」：把当前选中项真正切过去。
// 切换失败（引擎重建失败）时把下拉框显示退回上一个生效值，免得框里显示的和实际用的对不上。
static void sizeComboCommit()
{
    const int sel = (int)SendMessageA(g_cbSize, CB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    if (!applyCropSize(sel * 32 + 160)) sizeComboShowApplied();
}

// ============================================================================
//  窗口
// ============================================================================
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_COMMAND:
    {
        int id = LOWORD(w);
        int code = HIWORD(w);

        if (id == 100 && code == BN_CLICKED)          // 按钮_初始化
        {
            std::string devs;
            sagaEnumDevices(devs);
            if (devs.empty())
            {
                MessageBoxA(h, "未检测到采集卡,请重复拔插", "提示", MB_OK);
            }
            else
            {
                SendMessageA(g_cbDevice, CB_RESETCONTENT, 0, 0);
                size_t start = 0;
                for (;;)
                {
                    size_t p = devs.find('|', start);
                    std::string item = (p == std::string::npos) ? devs.substr(start) : devs.substr(start, p - start);
                    // Saga.dll 返回的设备名是 UTF-8；界面是 GBK(ANSI)，显示前要转码
                    if (!item.empty())
                    {
                        std::string disp = utf8ToAnsi(item);
                        SendMessageA(g_cbDevice, CB_ADDSTRING, 0, (LPARAM)disp.c_str());
                    }
                    if (p == std::string::npos) break;
                    start = p + 1;
                }
                EnableWindow(g_cbDevice, TRUE);
                SendMessageA(g_cbDevice, CB_SETCURSEL, 0, 0);
                // 只有一个设备时，选中第 0 项不会产生 CBN_SELCHANGE；主动枚举格式。
                SendMessageA(h, WM_COMMAND, MAKEWPARAM(200, CBN_SELCHANGE), (LPARAM)g_cbDevice);
            }
        }
        else if (id == 101 && code == BN_CLICKED)     // 按钮_开始采集
        {
            char dev[256] = {}, fmt[256] = {};
            GetWindowTextA(g_cbDevice, dev, sizeof(dev));
            GetWindowTextA(g_cbFormat, fmt, sizeof(fmt));
            if (dev[0] == 0) { MessageBoxA(h, "请选择采集卡后点击!", "提示", MB_OK); break; }
            if (fmt[0] == 0) { MessageBoxA(h, "请选择分辨率和刷新率后点击!", "提示", MB_OK); break; }

            // fmt: "WxH@fps@FOURCC"（原版：先把 x 替换成 @，再按 @ 分割）
            std::string f = fmt;
            for (char& c : f) if (c == 'x') c = '@';
            int w = 0, h2 = 0, fps = 0;
            char fourcc[32] = {};
            if (sscanf(f.c_str(), "%d@%d@%d@%31s", &w, &h2, &fps, fourcc) == 4)
            {
                if (!startCapture(dev, w, h2, fourcc, fps))
                {
                    MessageBoxA(h, "采集初始化失败", "系统提示", MB_OK);
                    break;
                }
            }
            else
            {
                std::string msg = "分辨率格式无法解析：" + std::string(fmt);
                MessageBoxA(h, msg.c_str(), "系统提示", MB_OK);
            }
        }
        else if (id == 102 && code == BN_CLICKED)     // 按钮_移动10次
        {
            trajMove10(100.0f * g.moveFactor, 0.0f);
        }
        else if (id == 103 && code == BN_CLICKED)     // 按钮_移动120像素
        {
            makcuMove(120, 0);
        }
        else if ((id == 104 || id == 105) && code == BN_CLICKED)
        {
            startHumanTrajectoryTool(h, id == 105);
        }
        else if (id == 106 && code == BN_CLICKED) webShowDiagnostics(h);
        else if (id == 107 && code == BN_CLICKED) webOpenLocalPage(h);
        else if (id == 200 && code == CBN_SELCHANGE)  // 组合框_设备名
        {
            char dev[256] = {};
            GetWindowTextA(g_cbDevice, dev, sizeof(dev));
            if (dev[0] == 0) break;
            std::string all;
            std::string devUtf8 = ansiToUtf8(dev);      // 设备名同样需以 UTF-8 传给 DLL
            sagaEnumFormats(devUtf8.c_str(), all);
            SendMessageA(g_cbFormat, CB_RESETCONTENT, 0, 0);
            size_t start = 0;
            for (;;)
            {
                size_t p = all.find('|', start);
                std::string item = (p == std::string::npos) ? all.substr(start) : all.substr(start, p - start);
                if (!item.empty())
                {
                    // DLL 返回：宽@高@帧率@编码；原版会重建成「宽x高@帧率@编码」再加进下拉框，
                    // 「开始采集」里再把 x 换回 @ 按 @ 分割。这里必须照做，
                    // 否则下拉框文本与解析格式不匹配 → 开始采集静默失效。
                    std::vector<std::string> seg;
                    size_t s0 = 0;
                    for (;;)
                    {
                        size_t q = item.find('@', s0);
                        seg.push_back((q == std::string::npos) ? item.substr(s0) : item.substr(s0, q - s0));
                        if (q == std::string::npos) break;
                        s0 = q + 1;
                    }
                    const char* ok[] = { "NV12","YUY2","MJPG","YUYV","H264","H265","HEVC" };
                    bool hit = false;
                    if (seg.size() == 4)
                        for (int k = 0; k < 7; ++k) if (seg[3] == ok[k]) hit = true;
                    if (seg.size() == 4 && hit)
                    {
                        std::string show = seg[0] + "x" + seg[1] + "@" + seg[2] + "@" + seg[3];
                        SendMessageA(g_cbFormat, CB_ADDSTRING, 0, (LPARAM)show.c_str());
                    }
                }
                if (p == std::string::npos) break;
                start = p + 1;
            }
            EnableWindow(g_cbFormat, TRUE);
        }
        // 组合框_视频大小（控件 ID 202；201 是分辨率格式框，改变时无需处理）
        // 列表展开时上下移动只算「浏览」——那些 CBN_SELCHANGE 会被 !CB_GETDROPPEDSTATE 挡掉。
        // 真正切换只认两件事：CBN_SELENDOK（鼠标点选 / 原生回车）与列表收起后的 CBN_SELCHANGE。
        // 取消（ESC、点列表外面）→ CBN_SELENDCANCEL，把下拉框显示退回真正生效的那个。
        // 实测：CB_SHOWDROPDOWN(FALSE) 收起列表发的也是 SELENDCANCEL，所以"取消"这条路
        // 同时也兜住了我们自己替用户收起列表的情况。
        else if (id == 202 && code == CBN_SELENDCANCEL)
        {
            // 推迟一拍再改选中项：等组框自己的收起流程整个跑完
            PostMessageA(h, kSyncSizeCombo, 0, 0);
        }
        else if (id == 202 && code == CBN_SELENDOK)
        {
            sizeComboCommit();
        }
        else if (id == 202 && code == CBN_SELCHANGE &&
                 !SendMessageA(g_cbSize, CB_GETDROPPEDSTATE, 0, 0))
        {
            // 列表本来就是收起的（没展开就用方向键改选）→ 所见即所得，直接生效
            sizeComboCommit();
        }
        else if (id == 300 && code == BN_CLICKED)     // 选择框_实时刷新
        {
            uiSetShowImg(IsDlgButtonChecked(h, 300) == BST_CHECKED);
        }
        break;
    }

    case WM_KEYDOWN:
        // Esc has no action; only the window close control exits.
        if (w == VK_ESCAPE) return 0;
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC dc = (HDC)w;
        SetTextColor(dc, RGB(57, 69, 87));
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_panelBrush;
    }

    case kSyncPreview:
        CheckDlgButton(h, 300, g.showImg.load() ? BST_CHECKED : BST_UNCHECKED);
        { RECT r = previewRect(); InvalidateRect(h, &r, FALSE); }
        return 0;

    case kSyncSizeCombo:
        sizeComboShowApplied();
        return 0;

    case WM_TIMER:
        if (w == 1) {
            static std::wstring previousTitle;
            const std::wstring title = L"圣人视觉 C++ · 采集与推理  |  网页访问IP：" + webAccessAddress();
            if (title != previousTitle) {
                SetWindowTextW(h, title.c_str());
                previousTitle = title;
            }
            if (trajReloadIfChanged()) EnableWindow(GetDlgItem(h, 102), TRUE);
            RECT hardware{696, 26, 1056, 56};
            InvalidateRect(h, &hardware, TRUE);
            RECT status{24, 65, 1056, 96};
            InvalidateRect(h, &status, TRUE);
            RECT footer{24, 748, 664, 780};
            InvalidateRect(h, &footer, TRUE);
        }
        return 0;

    case WM_MEASUREITEM:
    {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        if (item->CtlType == ODT_COMBOBOX) { item->itemHeight = 32; return TRUE; }
        break;
    }

    case WM_DRAWITEM:
    {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (item->CtlType == ODT_COMBOBOX) {
            bool selected = (item->itemState & ODS_SELECTED) != 0;
            HBRUSH brush = CreateSolidBrush(selected ? RGB(228,237,255) : RGB(255,255,255));
            FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
            char value[512] = {};
            if (item->itemID != (UINT)-1) {
                const LRESULT length = SendMessageA(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
                if (length >= 0 && length < sizeof(value))
                    SendMessageA(item->hwndItem, CB_GETLBTEXT, item->itemID, (LPARAM)value);
            }
            RECT r = item->rcItem; r.left += 12; r.right -= 8;
            textAt(item->hDC, value, r, g_uiFont, RGB(35,50,72),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return TRUE;
        }
        if (item->CtlType != ODT_BUTTON) break;
        bool disabled = (item->itemState & ODS_DISABLED) != 0;
        COLORREF color = disabled ? RGB(216,224,237) : RGB(44,99,214);
        if (!disabled && GetPropA(item->hwndItem, "SagaHover")) color = RGB(61,118,234);
        if (item->itemState & ODS_SELECTED) color = RGB(30,73,165);
        FillRect(item->hDC, &item->rcItem, g_panelBrush);
        rounded(item->hDC, item->rcItem, color, color, 10);
        char value[128]; GetWindowTextA(item->hwndItem, value, sizeof(value));
        textAt(item->hDC, value, item->rcItem, g_uiFontBold,
            disabled ? RGB(135,147,167) : RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (item->itemState & ODS_FOCUS) {
            RECT r = item->rcItem; InflateRect(&r, -5, -5); DrawFocusRect(item->hDC, &r);
        }
        return TRUE;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT view = previewRect();
        AcquireSRWLockShared(&g_canvasLock);
        bool ready = g_hasFrame && g.showImg.load();
        if (ready) BitBlt(dc, kViewX, kViewY, kBoard, kBoard, g_memDC, 0, 0, SRCCOPY);
        GdiFlush();
        ReleaseSRWLockShared(&g_canvasLock);
        if (!ready) {
            HBRUSH background = CreateSolidBrush(RGB(17,25,40));
            FillRect(dc, &view, background); DeleteObject(background);
            RECT text = view; text.top += 275; text.bottom = text.top + 36;
            textAt(dc, g.showImg.load() ? "等待采集画面" : "实时预览已暂停", text,
                g_uiFontBold, RGB(209,222,242), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            text.top += 36; text.bottom += 36;
            textAt(dc, g.showImg.load() ? "选择设备后开始采集 · 640 × 640" : "按 F1 或勾选右侧实时显示恢复", text,
                g_uiFont, RGB(119,141,170), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        textAt(dc, "圣人视觉 / C++", {24,18,664,56}, g_titleFont, RGB(27,43,68));
        // 状态栏：中心尺寸 / 采集帧率 / 推理帧率 / 显卡名称 / 推理耗时
        char head[160];
        snprintf(head, sizeof(head), "中心 %d × %d    采集 %d FPS    推理 %d FPS",
            g.imgSize, g.imgSize, g.frameFps, displayInferFps());
        char tail[48];
        snprintf(tail, sizeof(tail), "    %d ms", g.inferMs);

        const RECT statusRect{ 24, 65, 676, 94 };
        std::string line = head;
        if (const char* gpu = displayGpuName(); gpu[0])
        {
            const int budget = (statusRect.right - statusRect.left)
                - textWidth(dc, head) - textWidth(dc, tail) - textWidth(dc, "    ");
            const std::string shown = fitTextWidth(dc, gpu, budget);
            if (!shown.empty()) line += "    " + shown;
        }
        line += tail;
        textAt(dc, line.c_str(), statusRect, g_uiFont, RGB(101,118,142),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::string hardware = std::string("本地推理 · ") + makcuStartupStatus();
        textAt(dc, hardware.c_str(), {696,26,1056,56}, g_uiFont, RGB(101,118,142));
        textAt(dc, "画面等比显示 · F1 快速开关", {696,65,1056,94}, g_uiFont, RGB(101,118,142));
        textAt(dc, "识别范围为中心裁剪尺寸", {696,365,1056,391}, g_uiFont, RGB(119,132,153));
        textAt(dc, "数据：EXE / 人手数据 · 训练后自动生效", {696,738,1056,760}, g_uiFont, RGB(119,132,153));
        textAt(dc, "截图：EXE / 模型名 / 时间戳.jpg · JPEG 95%", {696,763,1056,785}, g_uiFont, RGB(119,132,153));
        char status[192];
        snprintf(status, sizeof(status), "最近距离 %d     优先锁定 %.2f", g.nearestDist.load(), g.prioRatio);
        textAt(dc, status, {24,749,664,779}, g_uiFont, RGB(101,118,142));
        EndPaint(h, &ps);
        return 0;
    }

    case WM_CLOSE:
    {
        // 圣人自用_配置保存：按原版格式写 [默认] 段（键名=控件名，组合框存选中项索引），
        // 这样易语言版读同一个 配置保存.ini 也能正常恢复。
        std::string ini = g.runDir + "\\配置保存.ini";
        int selDev = (int)SendMessageA(g_cbDevice, CB_GETCURSEL, 0, 0);
        int selFmt = (int)SendMessageA(g_cbFormat, CB_GETCURSEL, 0, 0);
        int selSz  = (int)SendMessageA(g_cbSize,   CB_GETCURSEL, 0, 0);
        WritePrivateProfileStringA("默认", "组合框_设备名",   std::to_string(selDev).c_str(), ini.c_str());
        WritePrivateProfileStringA("默认", "组合框_视频码率", std::to_string(selFmt).c_str(), ini.c_str());
        if (selSz >= 0)
            WritePrivateProfileStringA("默认", "组合框_视频大小", std::to_string(selSz).c_str(), ini.c_str());
        WritePrivateProfileStringA("默认", "选择框_实时刷新", g.showImg.load() ? "真" : "假", ini.c_str());

        // 本程序自己的补充记忆（原版没有这一段，不影响易语言读取 [默认]）
        char dev[256] = {}, fmt[256] = {};
        GetWindowTextA(g_cbDevice, dev, sizeof(dev));
        GetWindowTextA(g_cbFormat, fmt, sizeof(fmt));
        WritePrivateProfileStringA("窗口", "设备", dev, ini.c_str());
        WritePrivateProfileStringA("窗口", "格式", fmt, ini.c_str());
        WritePrivateProfileStringA("窗口", "大小", std::to_string(g.imgSize).c_str(), ini.c_str());
        WritePrivateProfileStringA("窗口", "刷新", g.showImg.load() ? "1" : "0", ini.c_str());

        exitApplicationNow();
    }

    case WM_DESTROY:
        exitApplicationNow();
    }
    return DefWindowProcA(h, m, w, l);
}

bool uiCreate(HINSTANCE hInst)
{
    boardInit();

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    g_uiFont = CreateFontA(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Microsoft YaHei UI");
    g_uiFontBold = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Microsoft YaHei UI");
    g_titleFont = CreateFontA(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, "Microsoft YaHei UI");
    g_panelBrush = CreateSolidBrush(RGB(247,249,252));
    WNDCLASSA wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SagaAppWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_panelBrush;
    RegisterClassA(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc{0, 0, 1080, 792};
    AdjustWindowRect(&rc, style, FALSE);
    g_hwnd = CreateWindowA("SagaAppWindow", "圣人视觉 C++ · 采集与推理  |  网页访问IP：正在获取",
        style, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return false;
    HWND h = g_hwnd;
    const auto label = [&](const char* value, int y) {
        HWND control = CreateWindowA("STATIC", value, WS_CHILD | WS_VISIBLE,
            696, y, 360, 26, h, nullptr, hInst, nullptr);
        SendMessageA(control, WM_SETFONT, (WPARAM)g_uiFontBold, TRUE);
        return control;
    };
    label("采集设备", 108);
    label("分辨率 / 帧率 / 编码", 196);
    label("中心识别范围（像素）", 284);
    const auto combo = [&](int id, int y) {
        HWND control = CreateWindowA("COMBOBOX", "",
            CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
            696, y, 360, 320, h, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageA(control, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
        SendMessageA(control, CB_SETITEMHEIGHT, (WPARAM)-1, 32);
        SendMessageA(control, CB_SETITEMHEIGHT, 0, 32);
        SendMessageA(control, CB_SETDROPPEDWIDTH, 360, 0);
        SendMessageA(control, CB_SETMINVISIBLE, 8, 0);
        SetWindowSubclass(control, controlProc, 2, 0);
        return control;
    };
    g_cbDevice = combo(200, 140);
    g_cbFormat = combo(201, 228);
    g_cbSize = combo(202, 316);
    const auto button = [&](int id, const char* value, int x, int y, int width = 174) {
        HWND control = CreateWindowA("BUTTON", value, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            x, y, width, 44, h, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageA(control, WM_SETFONT, (WPARAM)g_uiFontBold, TRUE);
        SetWindowSubclass(control, controlProc, 1, 0);
    };
    button(100, "初始化采集卡", 696, 414);
    button(101, "开始采集", 882, 414);
    g_chkRefresh = CreateWindowA("BUTTON", "实时显示（F1 开关）",
        BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 696, 480, 360, 32,
        h, (HMENU)300, hInst, nullptr);
    SendMessageA(g_chkRefresh, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SetWindowTheme(g_chkRefresh, L"Explorer", nullptr);
    button(106, "本机网络检查", 696, 522);
    button(107, "打开本机调参", 882, 522);
    button(102, "轨迹测试 · 10 次", 696, 576);
    button(103, "移动 · 120 像素", 882, 576);
    button(104, "点我开始记录人手数据", 696, 634, 360);
    button(105, "点我开始训练人手模型", 696, 688, 360);
    EnableWindow(GetDlgItem(h, 102), trajReady());
    SetTimer(h, 1, 500, nullptr);

    // 对齐原版：初始化采集卡成功前，设备和格式下拉框均不可操作。
    EnableWindow(g_cbDevice, FALSE);
    EnableWindow(g_cbFormat, FALSE);

    // 截图档位：160 ~ 640，步长 32，共 16 项
    for (int v = 160; v <= 640; v += 32)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), (v == 160) ? "%dX%d" : "%dx%d", v, v);   // 原版窗体首项是大写 X
        SendMessageA(g_cbSize, CB_ADDSTRING, 0, (LPARAM)buf);
    }
    int sel = (320 - 160) / 32;
    SendMessageA(g_cbSize, CB_SETCURSEL, sel, 0);
    g.imgSize = 320;

    // 恢复上次的窗口配置（1. 原版 [默认] 段  2. 本程序补充的 [窗口] 段）
    std::string ini = g.runDir + "\\配置保存.ini";

    char szBuf[32] = {};
    GetPrivateProfileStringA("默认", "组合框_视频大小", "", szBuf, sizeof(szBuf), ini.c_str());
    int defSz = -1;
    if (szBuf[0]) defSz = atoi(szBuf);
    if (defSz >= 0 && defSz <= 15)
    {
        g.imgSize = defSz * 32 + 160;
        SendMessageA(g_cbSize, CB_SETCURSEL, defSz, 0);
    }

    char rfBuf[32] = {};
    GetPrivateProfileStringA("默认", "选择框_实时刷新", "", rfBuf, sizeof(rfBuf), ini.c_str());
    bool rfSet = false;
    if (rfBuf[0])
    {
        g.showImg = (std::string(rfBuf) == "真" || std::string(rfBuf) == "1");
        rfSet = true;
    }

    char dev[256] = {}, fmt[256] = {};
    GetPrivateProfileStringA("窗口", "设备", "", dev, sizeof(dev), ini.c_str());
    GetPrivateProfileStringA("窗口", "格式", "", fmt, sizeof(fmt), ini.c_str());
    if (dev[0]) { SendMessageA(g_cbDevice, CB_ADDSTRING, 0, (LPARAM)dev); SendMessageA(g_cbDevice, CB_SETCURSEL, 0, 0); }
    if (fmt[0]) { SendMessageA(g_cbFormat, CB_ADDSTRING, 0, (LPARAM)fmt); SendMessageA(g_cbFormat, CB_SETCURSEL, 0, 0); }
    if (!(defSz >= 0 && defSz <= 15))
    {
        int sz = GetPrivateProfileIntA("窗口", "大小", 320, ini.c_str());
        if (sz >= 160 && sz <= 640) { g.imgSize = sz; SendMessageA(g_cbSize, CB_SETCURSEL, (sz - 160) / 32, 0); }
    }
    if (!rfSet)
    {
        int rf = GetPrivateProfileIntA("窗口", "刷新", 0, ini.c_str());
        g.showImg = (rf != 0);
    }
    CheckDlgButton(h, 300, g.showImg.load() ? BST_CHECKED : BST_UNCHECKED);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    return true;
}

// 下拉列表展开时按回车/ESC。
// 必须在 IsDialogMessage 之前拦：它会把回车转成 WM_COMMAND(IDOK) 吃掉，列表收不起来；
// ESC 拦下来也顺便避免落到窗口的「Esc = 关闭程序」上。
//   回车 = 确认 → 先收起列表，再把高亮项设回去并真正切换
//     ⚠ 实测：CB_SHOWDROPDOWN(FALSE) 收起列表会被组框当成「取消」，
//       选中项会被退回展开前的值，所以必须自己再 CB_SETCURSEL 设回来
//   ESC  = 取消 → 只收起列表；组框自己会退回原值，收起时的 SELENDCANCEL 再同步一次显示
// 列表没展开（收起来用方向键改选）时不管，交给控件自己走 CBN_SELCHANGE。
static bool comboConfirmKey(UINT key)
{
    HWND combos[3] = { g_cbDevice, g_cbFormat, g_cbSize };
    for (HWND combo : combos)
    {
        if (!combo || !IsWindow(combo)) continue;
        if (!SendMessageA(combo, CB_GETDROPPEDSTATE, 0, 0)) continue;

        const bool confirmSize = (combo == g_cbSize && key == VK_RETURN);
        const int sel = confirmSize ? (int)SendMessageA(combo, CB_GETCURSEL, 0, 0) : -1;

        SendMessageA(combo, CB_SHOWDROPDOWN, FALSE, 0);      // 收起列表
        if (confirmSize)
        {
            if (sel >= 0) SendMessageA(combo, CB_SETCURSEL, (WPARAM)sel, 0);
            sizeComboCommit();                               // 回车 = 确认
        }
        return true;
    }
    return false;
}

void uiThread()
{
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        // Consume Esc before controls or IsDialogMessage can act on it.
        if ((msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
             msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP ||
             msg.message == WM_CHAR) && msg.wParam == VK_ESCAPE)
            continue;
        // 下拉列表展开时的回车必须在 IsDialogMessage 之前截下来：
        // 否则回车会被它转成 WM_COMMAND(IDOK) 吃掉，列表反而收不起来。
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN
            && comboConfirmKey((UINT)msg.wParam))
            continue;
        if (!IsDialogMessageA(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
}

HWND uiGetHwnd()
{
    return g_hwnd;
}

// ============================================================================
//  选择框_实时刷新 / 组合框_视频大小 的同步
// ============================================================================
void uiSetShowImg(bool v)
{
    g.showImg = v;
    if (g_hwnd) PostMessageA(g_hwnd, kSyncPreview, 0, 0);
}

void uiSetComboSizeIndex(int idx)
{
    if (idx < 0 || idx > 15 || !g_cbSize) return;
    SendMessageA(g_cbSize, CB_SETCURSEL, idx, 0);
}

// ============================================================================
//  输入框（替代易语言 输入框：确定/回车返回真，取消返回假）
// ============================================================================
static HWND g_inEdit = nullptr;
static bool g_inOk = false;
static std::string g_inText;

// 必须在销毁窗口【之前】把编辑框内容取出来（子控件会随父窗口一起销毁）
static void inputAccept(HWND h)
{
    char buf[512] = {};
    if (g_inEdit) GetWindowTextA(g_inEdit, buf, sizeof(buf));
    g_inText = buf;
    g_inOk = true;
    DestroyWindow(h);
}

static LRESULT CALLBACK inputProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COMMAND)
    {
        int id = LOWORD(w);
        if (id == 1) { inputAccept(h); return 0; }
        if (id == 2) exitApplicationNow();
    }
    else if (m == WM_CLOSE)
    {
        g_inOk = false;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

bool uiInputBox(const char* prompt, const char* title, std::string& text)
{
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSA wc;
        std::memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = inputProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "SagaAppInput";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassA(&wc);
        reg = true;
    }

    g_inOk = false;
    g_inText.clear();
    RECT rc = { 0, 0, 360, 112 };
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
    HWND h = CreateWindowA("SagaAppInput", title, WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        g_hwnd, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!h) exitApplicationNow();

    CreateWindowA("STATIC", prompt, WS_CHILD | WS_VISIBLE, 15, 12, 330, 20, h, nullptr, nullptr, nullptr);
    g_inEdit = CreateWindowA("EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        15, 36, 330, 24, h, (HMENU)10, nullptr, nullptr);
    CreateWindowA("BUTTON", "确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        100, 72, 80, 26, h, (HMENU)1, nullptr, nullptr);
    CreateWindowA("BUTTON", "取消", WS_CHILD | WS_VISIBLE,
        200, 72, 80, 26, h, (HMENU)2, nullptr, nullptr);
    SendMessageA(g_inEdit, EM_SETSEL, 0, -1);
    SetFocus(g_inEdit);

    MSG msg;
    while (IsWindow(h) && GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        if ((msg.message == WM_KEYDOWN || msg.message == WM_KEYUP ||
             msg.message == WM_SYSKEYDOWN || msg.message == WM_SYSKEYUP ||
             msg.message == WM_CHAR) && msg.wParam == VK_ESCAPE)
            continue;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == g_inEdit)
        {
            inputAccept(h);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (g_inOk) text = g_inText;
    g_inEdit = nullptr;
    return g_inOk;
}

// ============================================================================
//  _启动子程序 的命令行分支
//  无命令行参数 → 直接打开主界面；本地默认地址为 127.0.0.1
//  有命令行参数 → 取消实时刷新，并用 监控双机.ini 的参数自动开始采集（双机模式）
// ============================================================================
void uiStartupCommandLine()
{
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    int argc = (argv && n > 0) ? n - 1 : 0;      // 取命令行 不包含程序自身路径
    if (argv) LocalFree(argv);

    if (argc == 0)
    {
        uiSetShowImg(true);
    }
    else
    {
        uiSetShowImg(false);
        startCapture("", 0, 0, "", 0);           // 空参 → 读 监控双机.ini
    }
}
