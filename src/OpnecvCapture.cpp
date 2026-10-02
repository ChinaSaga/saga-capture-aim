#include "Inference.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <process.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <atomic>
#include <malloc.h>
#include "CapturePixels.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// qedit.h is absent from current Windows SDKs. Preserve the original COM vtables.
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double time, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double time, BYTE* buffer, long size) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferSamples) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long method) = 0;
};

namespace {
const CLSID kSampleGrabber = {0xc1f400a0, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
const CLSID kNullRenderer = {0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
constexpr DWORD kFirstFrameTimeoutMs = 5000;
// Inference, preview, screenshots and statistics can hold different frames.
// Keep spare slots for engine switches; this remains a latest-frame pool, not a queue.
constexpr unsigned kCaptureSlots = 8;
char g_textBuffer[65536] = {};
SRWLOCK g_apiLock = SRWLOCK_INIT;
SRWLOCK g_snapshotLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_frameReady = CONDITION_VARIABLE_INIT;
CaptureFrame g_captureFrame;
unsigned long long g_captureSequence = 0; // Never reset across restarts.

class Lock {
public:
    explicit Lock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~Lock() { ReleaseSRWLockExclusive(&lock_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SRWLOCK& lock_;
};

template<class T> class ComPtr {
public:
    ~ComPtr() { reset(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    T** put() { reset(); return &ptr_; }
    void reset() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
    void abandon_at_process_exit() { ptr_ = nullptr; }
private:
    T* ptr_ = nullptr;
};

class ComScope {
public:
    ComScope() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(result_)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
    bool needs_mta_thread() const { return result_ == RPC_E_CHANGED_MODE; }
private:
    HRESULT result_;
};

void free_media_type(AM_MEDIA_TYPE& type) {
    if (type.pbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    type = {};
}

struct MediaType {
    AM_MEDIA_TYPE* ptr = nullptr;
    ~MediaType() { if (ptr) { free_media_type(*ptr); CoTaskMemFree(ptr); } }
};

BITMAPINFOHEADER* bitmap_header(AM_MEDIA_TYPE& type, REFERENCE_TIME** interval = nullptr) {
    if (interval) *interval = nullptr;
    if (!type.pbFormat || type.majortype != MEDIATYPE_Video) return nullptr;
    if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* info = reinterpret_cast<VIDEOINFOHEADER*>(type.pbFormat);
        if (interval) *interval = &info->AvgTimePerFrame;
        return &info->bmiHeader;
    }
    if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* info = reinterpret_cast<VIDEOINFOHEADER2*>(type.pbFormat);
        if (interval) *interval = &info->AvgTimePerFrame;
        return &info->bmiHeader;
    }
    return nullptr;
}

bool dimensions(const BITMAPINFOHEADER* bitmap, int& width, int& height) {
    if (!bitmap || bitmap->biSize < sizeof(BITMAPINFOHEADER) || bitmap->biWidth <= 0 ||
        bitmap->biHeight == 0 || bitmap->biHeight == (std::numeric_limits<LONG>::min)()) return false;
    width = bitmap->biWidth;
    height = bitmap->biHeight < 0 ? -bitmap->biHeight : bitmap->biHeight;
    return true;
}

bool device_name(IMoniker* moniker, char* name, int size) {
    ComPtr<IPropertyBag> bag;
    if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(bag.put())))) return false;
    VARIANT value;
    VariantInit(&value);
    HRESULT hr = bag->Read(L"FriendlyName", &value, nullptr);
    bool ok = SUCCEEDED(hr) && value.vt == VT_BSTR && value.bstrVal &&
        WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, name, size, nullptr, nullptr) > 0;
    VariantClear(&value);
    return ok;
}

HRESULT video_devices(IEnumMoniker** result) {
    ComPtr<ICreateDevEnum> devices;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(devices.put()));
    if (FAILED(hr)) return hr;
    hr = devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, result, 0);
    return hr == S_FALSE ? HRESULT_FROM_WIN32(ERROR_NOT_FOUND) : hr;
}

HRESULT bind_device(const char* name, IBaseFilter** result) {
    if (!name || !*name) return E_INVALIDARG;
    ComPtr<IEnumMoniker> devices;
    HRESULT hr = video_devices(devices.put());
    if (FAILED(hr)) return hr;
    ComPtr<IMoniker> moniker;
    while (devices->Next(1, moniker.put(), nullptr) == S_OK) {
        char current[1024];
        if (device_name(moniker.get(), current, sizeof(current)) && std::strcmp(current, name) == 0)
            return moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, reinterpret_cast<void**>(result));
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT capture_pin(ICaptureGraphBuilder2* builder, IBaseFilter* source,
    IPin** pin, IAMStreamConfig** config) {
    HRESULT hr = builder->FindPin(source, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE,
        &MEDIATYPE_Video, FALSE, 0, pin);
    if (FAILED(hr)) hr = builder->FindPin(source, PINDIR_OUTPUT, nullptr,
        &MEDIATYPE_Video, FALSE, 0, pin);
    if (FAILED(hr)) return hr;
    return (*pin)->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(config));
}

void format_name(const GUID& subtype, char (&name)[16]) {
    if (subtype == MEDIASUBTYPE_RGB24) strcpy_s(name, "RGB24");
    else if (subtype == MEDIASUBTYPE_RGB32) strcpy_s(name, "RGB32");
    else if (subtype == MEDIASUBTYPE_ARGB32) strcpy_s(name, "ARGB");
    else {
        for (int i = 0; i < 4; ++i) {
            const char c = static_cast<char>((subtype.Data1 >> (i * 8)) & 0xff);
            name[i] = c >= 32 && c <= 126 && c != '|' && c != '@' ? c : '?';
        }
        name[4] = '\0';
    }
}

bool parse_subtype(const char* text, GUID& subtype) {
    if (!text) return false;
    if (std::strcmp(text, "RGB24") == 0 || std::strcmp(text, "BGR24") == 0) subtype = MEDIASUBTYPE_RGB24;
    else if (std::strcmp(text, "RGB32") == 0 || std::strcmp(text, "BGR32") == 0) subtype = MEDIASUBTYPE_RGB32;
    else if (std::strcmp(text, "ARGB") == 0 || std::strcmp(text, "ARGB32") == 0) subtype = MEDIASUBTYPE_ARGB32;
    else {
        if (std::strlen(text) != 4) return false;
        if (std::strcmp(text, "YUYV") == 0) text = "YUY2";
        subtype = MEDIATYPE_Video; // Standard FOURCC GUID tail.
        subtype.Data1 = MAKEFOURCC(text[0], text[1], text[2], text[3]);
    }
    return true;
}

bool append_text(const char* value) {
    const size_t used = std::strlen(g_textBuffer), length = std::strlen(value);
    if (used + length + (used ? 1u : 0u) >= sizeof(g_textBuffer)) return false;
    if (used) strcat_s(g_textBuffer, "|");
    strcat_s(g_textBuffer, value);
    return true;
}

const char* error_text(const char* step, HRESULT hr) {
    sprintf_s(g_textBuffer, "失败: %s (0x%08lX)", step, static_cast<unsigned long>(hr));
    return g_textBuffer;
}

enum class PixelFormat { Bgr24, Yuy2, Nv12, Nv21, P010, Unsupported };

PixelFormat pixel_format(const GUID& subtype) {
    if (subtype == MEDIASUBTYPE_RGB24) return PixelFormat::Bgr24;
    GUID fourcc = MEDIATYPE_Video;
    fourcc.Data1 = subtype.Data1;
    if (fourcc != subtype) return PixelFormat::Unsupported;
    switch (subtype.Data1) {
    case MAKEFOURCC('Y', 'U', 'Y', '2'):
    case MAKEFOURCC('Y', 'U', 'Y', 'V'): return PixelFormat::Yuy2;
    case MAKEFOURCC('N', 'V', '1', '2'): return PixelFormat::Nv12;
    case MAKEFOURCC('N', 'V', '2', '1'): return PixelFormat::Nv21;
    case MAKEFOURCC('P', '0', '1', '0'): return PixelFormat::P010;
    default: return PixelFormat::Unsupported;
    }
}

using YuvConversion = capture_pixels::Coefficients;

bool yuv_conversion(AM_MEDIA_TYPE& type, bool tenBit, YuvConversion& result) {
    // Without explicit DXVA metadata, preserve OpenCV's common BT.601 limited
    // range convention. Resolution alone does not identify the color matrix.
    unsigned matrix = 0, range = 0;
    if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat);
        if (info->dwControlFlags & AMCONTROL_COLORINFO_PRESENT) {
            // DXVA_ExtendedFormat: NominalRange occupies bits 12..14 and
            // VideoTransferMatrix bits 15..17 of VIDEOINFOHEADER2.dwControlFlags.
            range = (info->dwControlFlags >> 12) & 7;
            matrix = (info->dwControlFlags >> 15) & 7;
        }
    }
    if (range > 2 || matrix > 3) return false;
    const double kr = matrix == 1 ? 0.2126 : (matrix == 3 ? 0.212 : 0.299);
    const double kb = matrix == 1 ? 0.0722 : (matrix == 3 ? 0.087 : 0.114);
    const double kg = 1.0 - kr - kb;
    const bool full = range == 1;
    const int scale = tenBit ? 4 : 1;
    const double yScale = full ? 255.0 / (tenBit ? 1023.0 : 255.0) : 255.0 / (219 * scale);
    const double uvScale = full ? yScale : 255.0 / (224 * scale);
    const auto fixed = [](double value) {
        return static_cast<int>(value * 65536.0 + (value >= 0 ? 0.5 : -0.5));
    };
    result.yOffset = full ? 0 : 16 * scale;
    result.uvOffset = 128 * scale;
    result.y = fixed(yScale);
    result.redV = fixed(2 * (1 - kr) * uvScale);
    result.blueU = fixed(2 * (1 - kb) * uvScale);
    result.greenU = fixed(-2 * kb * (1 - kb) / kg * uvScale);
    result.greenV = fixed(-2 * kr * (1 - kr) / kg * uvScale);
    return true;
}

struct FrameLayout {
    PixelFormat format = PixelFormat::Unsupported;
    size_t pitch = 0, bytes = 0;
    bool bottomUp = false;
    YuvConversion conversion;
};

bool frame_layout(AM_MEDIA_TYPE& type, int width, int height, FrameLayout& layout) {
    int actualWidth = 0, actualHeight = 0;
    auto* bitmap = bitmap_header(type);
    if (!dimensions(bitmap, actualWidth, actualHeight) || actualWidth != width || actualHeight != height ||
        bitmap->biPlanes != 1) return false;
    layout.format = pixel_format(type.subtype);
    const bool planar = layout.format == PixelFormat::Nv12 || layout.format == PixelFormat::Nv21 ||
        layout.format == PixelFormat::P010;
    uint64_t minimumPitch = 0, rows = static_cast<uint64_t>(height);
    if (layout.format == PixelFormat::Bgr24) {
        if (bitmap->biBitCount != 24 || bitmap->biCompression != BI_RGB) return false;
        minimumPitch = (static_cast<uint64_t>(width) * 3 + 3) & ~uint64_t(3);
        layout.bottomUp = bitmap->biHeight > 0;
    } else {
        if (layout.format == PixelFormat::Unsupported || (width & 1) || (planar && (height & 1)) ||
            bitmap->biCompression != type.subtype.Data1) return false;
        if (layout.format == PixelFormat::Yuy2 && bitmap->biBitCount != 16) return false;
        if ((layout.format == PixelFormat::Nv12 || layout.format == PixelFormat::Nv21) && bitmap->biBitCount != 12)
            return false;
        if (layout.format == PixelFormat::P010 && bitmap->biBitCount != 16 && bitmap->biBitCount != 24)
            return false;
        minimumPitch = static_cast<uint64_t>(width) *
            (layout.format == PixelFormat::Yuy2 || layout.format == PixelFormat::P010 ? 2u : 1u);
        if (planar) rows += static_cast<uint64_t>(height) / 2;
        // DirectShow uncompressed YUV is top-down for either biHeight sign.
        layout.bottomUp = false;
        if (!yuv_conversion(type, layout.format == PixelFormat::P010, layout.conversion)) return false;
    }
    uint64_t pitch = minimumPitch;
    if (bitmap->biSizeImage) {
        if (bitmap->biSizeImage % rows != 0) return false;
        pitch = bitmap->biSizeImage / rows;
        if (pitch < minimumPitch) return false;
    }
    // NV12/NV21/P010 require contiguous Y and interleaved UV planes with the
    // same pitch; reject ambiguous sizes instead of guessing a UV-plane offset.
    if ((layout.format == PixelFormat::Bgr24 || layout.format == PixelFormat::Yuy2) ?
        (pitch & 3) != 0 : (pitch & 1) != 0) return false;
    if (pitch * rows > static_cast<uint64_t>((std::numeric_limits<long>::max)())) return false;
    layout.pitch = static_cast<size_t>(pitch);
    layout.bytes = static_cast<size_t>(pitch * rows);
    return true;
}

// Reusable slots; consumers pin the newest completed frame without copying.
class FrameReceiver final : public ISampleGrabberCB {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        *object = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(ISampleGrabberCB)) return E_NOINTERFACE;
        *object = static_cast<ISampleGrabberCB*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs_)); }

    bool configure(AM_MEDIA_TYPE& type, int width, int height, int crop) {
        Lock producer(producerLock_);
        Lock snapshot(g_snapshotLock);
        if (crop <= 0 || crop > width || crop > height || crop > 640) return false;
        if (!frame_layout(type, width, height, layout_)) return false;
        crop_ = crop;
        left_ = (width - crop) / 2;
        stride_ = (static_cast<size_t>(crop) * 3 + 3) & ~size_t(3);
        size_ = static_cast<int>(54 + stride_ * crop);
        rows_.reset(new Row[crop]);
        const int top = (height - crop) / 2;
        unpairedFirst_ = (top & 1) != 0;
        for (int y = 0; y < crop; ++y) {
            const int logicalRow = top + y;
            rows_[y].luma = static_cast<size_t>(layout_.bottomUp ? height - 1 - logicalRow : logicalRow) * layout_.pitch;
            rows_[y].chroma = (static_cast<size_t>(height) + logicalRow / 2) * layout_.pitch;
        }
        for (unsigned i = 0; i < kCaptureSlots; ++i) {
            storage_[i].reset(static_cast<BYTE*>(_aligned_malloc(static_cast<size_t>(size_) + 64, 64)), FreeAligned{});
            if (!storage_[i]) throw std::bad_alloc();
            // BMP pixels begin at +54; shifting the header by ten bytes aligns
            // the first pixel row to 64 bytes (common crop widths keep it so).
            buffers_[i] = storage_[i].get() + 10;
            std::memset(buffers_[i], 0, size_);
            BITMAPFILEHEADER file = {};
            file.bfType = 0x4d42; file.bfSize = size_; file.bfOffBits = 54;
            BITMAPINFOHEADER info = {};
            info.biSize = sizeof(info); info.biWidth = crop; info.biHeight = -crop;
            info.biPlanes = 1; info.biBitCount = 24;
            info.biSizeImage = static_cast<DWORD>(stride_ * crop);
            std::memcpy(buffers_[i], &file, sizeof(file));
            std::memcpy(buffers_[i] + sizeof(file), &info, sizeof(info));
        }
        switch (layout_.format) {
        case PixelFormat::Bgr24: kernel_ = convert<PixelFormat::Bgr24>; break;
        case PixelFormat::Yuy2: kernel_ = convert<PixelFormat::Yuy2>; break;
        case PixelFormat::Nv12: kernel_ = convert<PixelFormat::Nv12>; break;
        case PixelFormat::Nv21: kernel_ = convert<PixelFormat::Nv21>; break;
        case PixelFormat::P010: kernel_ = convert<PixelFormat::P010>; break;
        default: __assume(0);
        }
        g_captureFrame = {};
        return true;
    }

    // SampleCB stays available for in-process tests; production selects BufferCB
    // so there are no IMediaSample method calls in the streaming path.
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample* sample) override {
        BYTE* data;
        sample->GetPointer(&data);
        return BufferCB(0, data, sample->GetActualDataLength());
    }
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE* data, long bytes) override {
        // Only producers and reconfiguration touch the slot array. A free
        // slot has no published/consumer reference, so converting it does not
        // require the snapshot lock. Readers can acquire the last completed
        // frame while the next crop is still being converted.
        Lock producer(producerLock_);
        // Drivers may submit an empty/truncated sample during a format switch.
        // Validate once before the SIMD kernels, which deliberately have no
        // per-row bounds checks; do not publish it as a fresh complete frame.
        if (!data || !kernel_ || bytes < 0 || static_cast<size_t>(bytes) < layout_.bytes)
            return S_OK;
        unsigned slot = 0;
        for (; slot < kCaptureSlots; ++slot) if (storage_[slot].use_count() == 1) break;
        if (slot == kCaptureSlots) return S_OK; // Drop rather than queue stale frames.
        BYTE* completed = buffers_[slot];
        kernel_(*this, data, completed + 54);
        {
            Lock snapshot(g_snapshotLock);
            g_captureFrame = {storage_[slot], completed, size_, crop_, crop_, stride_};
            ++g_captureSequence;
        }
        WakeAllConditionVariable(&g_frameReady);

        if (firstFrame_) {
            SetEvent(firstFrame_);
            firstFrame_ = nullptr; // Signal only once, never a syscall per frame.
        }
        return S_OK;
    }

    void clear() {
        Lock producer(producerLock_);
        Lock snapshot(g_snapshotLock);
        g_captureFrame = {};
        for (auto& slot : storage_) slot.reset();
        rows_.reset();
        WakeAllConditionVariable(&g_frameReady);
    }
    void set_event(HANDLE event) { firstFrame_ = event; }
private:
    struct Row { size_t luma, chroma; };
    struct FreeAligned { void operator()(BYTE* value) const { _aligned_free(value); } };
    using Kernel = void (*)(const FrameReceiver&, const BYTE*, BYTE*);

    template<PixelFormat Format>
    static __forceinline void convert_row(const FrameReceiver& state, const BYTE* data, const Row& row, BYTE* destination) {
        const BYTE* source = data + row.luma;
        if constexpr (Format == PixelFormat::Bgr24) {
            std::memcpy(destination, source + static_cast<size_t>(state.left_) * 3, static_cast<size_t>(state.crop_) * 3);
        } else if constexpr (Format == PixelFormat::Yuy2) {
            capture_pixels::yuy2_row(source, state.left_, state.crop_, destination, state.layout_.conversion);
        } else {
            const BYTE* chroma = data + row.chroma;
            if constexpr (Format == PixelFormat::Nv12)
                capture_pixels::nv12_row(source, chroma, state.left_, state.crop_, destination, state.layout_.conversion);
            else if constexpr (Format == PixelFormat::Nv21)
                capture_pixels::nv21_row(source, chroma, state.left_, state.crop_, destination, state.layout_.conversion);
            else
                capture_pixels::p010_row(source, chroma, state.left_, state.crop_, destination, state.layout_.conversion);
        }
    }

    template<PixelFormat Format>
    static void convert(const FrameReceiver& state, const BYTE* data, BYTE* destination) {
        const Row* row = state.rows_.get();
        if constexpr (Format == PixelFormat::Bgr24 || Format == PixelFormat::Yuy2) {
            for (int y = 0; y < state.crop_; ++y, destination += state.stride_)
                convert_row<Format>(state, data, row[y], destination);
        } else {
            int y = 0;
            // A center crop may begin at the second row of a 4:2:0 chroma pair
            // in BMP order. Peel it once, then every pair shares one UV row.
            if (state.unpairedFirst_) {
                convert_row<Format>(state, data, row[0], destination);
                destination += state.stride_;
                y = 1;
            }
            for (; y + 1 < state.crop_; y += 2, destination += state.stride_ * 2) {
                const BYTE* source0 = data + row[y].luma;
                const BYTE* source1 = data + row[y + 1].luma;
                const BYTE* chroma = data + row[y].chroma;
                BYTE* output1 = destination + state.stride_;
                if constexpr (Format == PixelFormat::Nv12)
                    capture_pixels::nv12_two_rows(source0, source1, chroma, state.left_, state.crop_, destination, output1, state.layout_.conversion);
                else if constexpr (Format == PixelFormat::Nv21)
                    capture_pixels::nv21_two_rows(source0, source1, chroma, state.left_, state.crop_, destination, output1, state.layout_.conversion);
                else
                    capture_pixels::p010_two_rows(source0, source1, chroma, state.left_, state.crop_, destination, output1, state.layout_.conversion);
            }
            if (y < state.crop_) convert_row<Format>(state, data, row[y], destination);
        }
    }
    volatile LONG refs_ = 1;
    SRWLOCK producerLock_ = SRWLOCK_INIT;
    HANDLE firstFrame_ = nullptr;
    std::shared_ptr<BYTE> storage_[kCaptureSlots];
    BYTE* buffers_[kCaptureSlots] = {};
    std::unique_ptr<Row[]> rows_;
    Kernel kernel_ = nullptr;
    FrameLayout layout_;
    size_t stride_ = 0;
    int size_ = 0, crop_ = 0, left_ = 0;
    bool unpairedFirst_ = false;
};
HRESULT set_format(IAMStreamConfig* config, int width, int height, const GUID& subtype, int fps) {
    int count = 0, bytes = 0;
    HRESULT hr = config->GetNumberOfCapabilities(&count, &bytes);
    if (FAILED(hr)) return hr;
    if (count <= 0 || bytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) return VFW_E_INVALIDMEDIATYPE;
    std::unique_ptr<BYTE[]> storage(new (std::nothrow) BYTE[bytes]);
    if (!storage) return E_OUTOFMEMORY;
    HRESULT lastError = VFW_E_INVALIDMEDIATYPE;
    for (int i = 0; i < count; ++i) {
        MediaType type;
        if (FAILED(config->GetStreamCaps(i, &type.ptr, storage.get())) || !type.ptr) continue;
        REFERENCE_TIME* interval = nullptr;
        auto* bitmap = bitmap_header(*type.ptr, &interval);
        int w = 0, h = 0;
        if (!dimensions(bitmap, w, h) || w != width || h != height || type.ptr->subtype != subtype || !interval) continue;
        const auto* caps = reinterpret_cast<const VIDEO_STREAM_CONFIG_CAPS*>(storage.get());
        REFERENCE_TIME requested = (10000000LL + fps / 2) / fps;
        // Enumeration rounds 59.94 FPS to 60; accept that exact round trip.
        if (caps->MinFrameInterval > 0 && requested < caps->MinFrameInterval) {
            if (static_cast<int>(10000000.0 / caps->MinFrameInterval + 0.5) != fps) continue;
            requested = caps->MinFrameInterval;
        }
        if (caps->MaxFrameInterval > 0 && requested > caps->MaxFrameInterval) {
            if (static_cast<int>(10000000.0 / caps->MaxFrameInterval + 0.5) != fps) continue;
            requested = caps->MaxFrameInterval;
        }
        *interval = requested;
        hr = config->SetFormat(type.ptr);
        if (FAILED(hr)) { lastError = hr; continue; }
        MediaType actual;
        hr = config->GetFormat(&actual.ptr);
        REFERENCE_TIME* actualInterval = nullptr;
        auto* actualBitmap = actual.ptr ? bitmap_header(*actual.ptr, &actualInterval) : nullptr;
        if (FAILED(hr) || !dimensions(actualBitmap, w, h) || w != width || h != height ||
            actual.ptr->subtype != subtype || !actualInterval || *actualInterval <= 0 ||
            static_cast<int>(10000000.0 / *actualInterval + 0.5) != fps) {
            lastError = VFW_E_INVALIDMEDIATYPE;
            continue;
        }
        return S_OK;
    }
    return lastError;
}

class CaptureSession {
public:
    ~CaptureSession() {
        // During process termination Windows has already torn down other
        // threads. Calling Stop/COM shutdown here can wait on a dead thread.
        // Resolve the ntdll query without loading a module under the loader lock.
        using ShutdownQuery = BOOLEAN (WINAPI*)();
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        FARPROC address = ntdll ? GetProcAddress(ntdll, "RtlDllShutdownInProgress") : nullptr;
        ShutdownQuery shuttingDown = nullptr;
        static_assert(sizeof(shuttingDown) == sizeof(address));
        std::memcpy(&shuttingDown, &address, sizeof(shuttingDown));
        if (shuttingDown && shuttingDown()) {
            control_.abandon_at_process_exit();
            grabber_.abandon_at_process_exit(); graph_.abandon_at_process_exit();
            return; // The OS reclaims handles and the MTA usage cookie.
        }
        stop();
    }
    void stop() {
        // Stop streaming before releasing the two frame buffers.
        if (control_) control_->Stop();
        if (grabber_) grabber_->SetCallback(nullptr, 0);
        control_.reset(); grabber_.reset(); graph_.reset();
        receiver_.clear(); receiver_.set_event(nullptr);
        if (firstFrame_) { CloseHandle(firstFrame_); firstFrame_ = nullptr; }
        if (mta_) { CoDecrementMTAUsage(mta_); mta_ = nullptr; }
    }

    const char* start(const char* name, int width, int height, const GUID& subtype, int fps, int crop) {
        stop();
        // Keep COM alive between API calls; graph interfaces are free-threaded.
        HRESULT hr = CoIncrementMTAUsage(&mta_);
        if (FAILED(hr)) return error_text("COM 初始化", hr);
        ComPtr<ICaptureGraphBuilder2> builder;
        ComPtr<IBaseFilter> source, grabberFilter, renderer;
        ComPtr<IPin> pin;
        ComPtr<IAMStreamConfig> config;
        const char* step = "创建采集图";
        do {
            hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(graph_.put()));
            if (FAILED(hr)) break;
            hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(builder.put()));
            if (FAILED(hr) || FAILED(hr = builder->SetFiltergraph(graph_.get()))) break;
            step = "打开采集设备";
            hr = bind_device(name, source.put());
            if (FAILED(hr) || FAILED(hr = graph_->AddFilter(source.get(), L"Capture device"))) break;
            step = "设置采集格式或帧率";
            hr = capture_pin(builder.get(), source.get(), pin.put(), config.put());
            if (FAILED(hr) || FAILED(hr = set_format(config.get(), width, height, subtype, fps))) break;
            step = "创建 Sample Grabber";
            hr = CoCreateInstance(kSampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(grabberFilter.put()));
            if (FAILED(hr) || FAILED(hr = grabberFilter->QueryInterface(__uuidof(ISampleGrabber),
                reinterpret_cast<void**>(grabber_.put())))) break;
            if (FAILED(hr = graph_->AddFilter(grabberFilter.get(), L"Frame receiver"))) break;
            AM_MEDIA_TYPE received = {};
            received.majortype = MEDIATYPE_Video;
            const bool native = pixel_format(subtype) != PixelFormat::Unsupported;
            received.subtype = native ? subtype : MEDIASUBTYPE_RGB24;
            if (FAILED(hr = grabber_->SetMediaType(&received)) || FAILED(hr = grabber_->SetOneShot(FALSE)) ||
                FAILED(hr = grabber_->SetBufferSamples(FALSE))) break;
            hr = CoCreateInstance(kNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(renderer.put()));
            if (FAILED(hr) || FAILED(hr = graph_->AddFilter(renderer.get(), L"Null renderer"))) break;
            step = native ? "连接采集卡原始视频流" : "连接采集卡到 BGR24（需驱动或系统支持解码/转换）";
            hr = builder->RenderStream(nullptr, &MEDIATYPE_Video, pin.get(), grabberFilter.get(), renderer.get());
            if (FAILED(hr)) break;
            // Intelligent Connect may renegotiate the source. Do not silently
            // accept a different device format/FPS merely because BGR connects.
            MediaType sourceType;
            hr = config->GetFormat(&sourceType.ptr);
            REFERENCE_TIME* sourceInterval = nullptr;
            auto* sourceBitmap = sourceType.ptr ? bitmap_header(*sourceType.ptr, &sourceInterval) : nullptr;
            int sourceWidth = 0, sourceHeight = 0;
            if (FAILED(hr) || !dimensions(sourceBitmap, sourceWidth, sourceHeight) ||
                sourceWidth != width || sourceHeight != height || sourceType.ptr->subtype != subtype ||
                !sourceInterval || *sourceInterval <= 0 ||
                static_cast<int>(10000000.0 / *sourceInterval + 0.5) != fps) {
                hr = VFW_E_INVALIDMEDIATYPE;
                break;
            }
            AM_MEDIA_TYPE connected = {};
            hr = grabber_->GetConnectedMediaType(&connected);
            const bool valid = SUCCEEDED(hr) && receiver_.configure(connected, width, height, crop);
            free_media_type(connected);
            if (!valid) { hr = VFW_E_INVALIDMEDIATYPE; break; }
            firstFrame_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!firstFrame_) { hr = HRESULT_FROM_WIN32(GetLastError()); break; }
            receiver_.set_event(firstFrame_);
            if (FAILED(hr = grabber_->SetCallback(&receiver_, 1))) break;
            if (FAILED(hr = graph_->QueryInterface(IID_PPV_ARGS(control_.put())))) break;
            ComPtr<IMediaFilter> mediaFilter;
            if (SUCCEEDED(graph_->QueryInterface(IID_PPV_ARGS(mediaFilter.put())))) mediaFilter->SetSyncSource(nullptr);
            step = "启动采集卡";
            if (FAILED(hr = control_->Run())) break;
            step = "等待采集卡首帧";
            const DWORD wait = WaitForSingleObject(firstFrame_, kFirstFrameTimeoutMs);
            if (wait != WAIT_OBJECT_0) {
                hr = HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
                break;
            }
            return "成功";
        } while (false);
        config.reset(); pin.reset(); renderer.reset(); grabberFilter.reset(); source.reset(); builder.reset();
        stop();
        return error_text(step, hr);
    }

private:
    FrameReceiver receiver_;
    ComPtr<IGraphBuilder> graph_;
    ComPtr<ISampleGrabber> grabber_;
    ComPtr<IMediaControl> control_;
    CO_MTA_USAGE_COOKIE mta_ = nullptr;
    HANDLE firstFrame_ = nullptr;
};

CaptureSession g_capture;

struct StartRequest {
    const char* name;
    int width, height;
    GUID subtype;
    int fps, crop;
    const char* result = nullptr;
};

unsigned __stdcall start_in_mta(void* context) {
    auto& request = *static_cast<StartRequest*>(context);
    ComScope com;
    request.result = com.valid()
        ? g_capture.start(request.name, request.width, request.height, request.subtype, request.fps, request.crop)
        : error_text("COM 初始化", CO_E_NOTINITIALIZED);
    return 0;
}
} // namespace

const char* saga::enum_video_devices() {
    Lock lock(g_apiLock);
    g_textBuffer[0] = '\0';
    ComScope com;
    if (!com.valid()) return g_textBuffer;
    ComPtr<IEnumMoniker> devices;
    if (FAILED(video_devices(devices.put()))) return g_textBuffer;
    ComPtr<IMoniker> moniker;
    while (devices->Next(1, moniker.put(), nullptr) == S_OK) {
        char name[1024];
        if (device_name(moniker.get(), name, sizeof(name)) && !append_text(name)) break;
    }
    return g_textBuffer;
}

const char* saga::enum_device_formats(const char* deviceName) {
    Lock lock(g_apiLock);
    g_textBuffer[0] = '\0';
    ComScope com;
    if (!com.valid()) return g_textBuffer;
    ComPtr<IBaseFilter> source;
    ComPtr<IGraphBuilder> graph;
    ComPtr<ICaptureGraphBuilder2> builder;
    ComPtr<IPin> pin;
    ComPtr<IAMStreamConfig> config;
    if (FAILED(bind_device(deviceName, source.put())) ||
        FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(graph.put()))) ||
        FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(builder.put()))) ||
        FAILED(builder->SetFiltergraph(graph.get())) || FAILED(graph->AddFilter(source.get(), L"Capture device")) ||
        FAILED(capture_pin(builder.get(), source.get(), pin.put(), config.put()))) return g_textBuffer;
    int count = 0, bytes = 0;
    if (FAILED(config->GetNumberOfCapabilities(&count, &bytes)) ||
        bytes < static_cast<int>(sizeof(VIDEO_STREAM_CONFIG_CAPS))) return g_textBuffer;
    std::unique_ptr<BYTE[]> storage(new (std::nothrow) BYTE[bytes]);
    if (!storage) return g_textBuffer;
    char unique[256][64] = {};
    int uniqueCount = 0;
    for (int i = 0; i < count && uniqueCount < 256; ++i) {
        MediaType type;
        if (FAILED(config->GetStreamCaps(i, &type.ptr, storage.get())) || !type.ptr) continue;
        int width = 0, height = 0;
        REFERENCE_TIME* interval = nullptr;
        if (!dimensions(bitmap_header(*type.ptr, &interval), width, height)) continue;
        const auto* caps = reinterpret_cast<const VIDEO_STREAM_CONFIG_CAPS*>(storage.get());
        const REFERENCE_TIME fastest = caps->MinFrameInterval > 0 ? caps->MinFrameInterval : (interval ? *interval : 0);
        if (fastest <= 0) continue;
        char format[16], entry[64];
        format_name(type.ptr->subtype, format);
        sprintf_s(entry, "%d@%d@%.0f@%s", width, height, 10000000.0 / fastest, format);
        bool duplicate = false;
        for (int j = 0; j < uniqueCount; ++j) if (std::strcmp(unique[j], entry) == 0) { duplicate = true; break; }
        if (!duplicate) {
            strcpy_s(unique[uniqueCount++], entry);
            if (!append_text(entry)) break;
        }
    }
    return g_textBuffer;
}

const char* saga::setcjk_ex(const char* deviceName, int width, int height, const char* fourcc, int fps, int cropSize) {
    Lock lock(g_apiLock);
    ComScope com;
    if (!com.valid()) return error_text("COM 初始化", CO_E_NOTINITIALIZED);
    // Optional explicit close, using the existing signature. Close before
    // dynamically unloading a DLL, outside DllMain/the Windows loader lock.
    if (!deviceName && !fourcc && width == 0 && height == 0 && fps == 0 && cropSize == 0) {
        g_capture.stop();
        return "成功";
    }
    GUID subtype = {};
    if (width <= 0 || height <= 0 || fps <= 0 || cropSize <= 0 ||
        cropSize > 640 || cropSize > width || cropSize > height || !parse_subtype(fourcc, subtype))
        return error_text("无效采集参数", E_INVALIDARG);
    if (!com.needs_mta_thread())
        return g_capture.start(deviceName, width, height, subtype, fps, cropSize);
    // Some device filters require MTA initialization even though the graph is
    // free-threaded. The short setup thread exits once the graph is running;
    // the MTA usage cookie keeps it alive for later reads from any caller.
    StartRequest request{deviceName, width, height, subtype, fps, cropSize};
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, start_in_mta, &request, 0, nullptr));
    if (!thread) return error_text("创建采集初始化线程", E_OUTOFMEMORY);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return request.result;
}

bool captureAcquire(CaptureFrame& frame, unsigned long long& sequence, unsigned timeoutMs) {
    Lock snapshot(g_snapshotLock);
    if (!g_captureFrame.owner || sequence == g_captureSequence) {
        if (!timeoutMs) return false;
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        do {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) return false;
            if (!SleepConditionVariableSRW(&g_frameReady, &g_snapshotLock,
                static_cast<DWORD>(deadline - now), 0) && GetLastError() == ERROR_TIMEOUT) return false;
        } while (!g_captureFrame.owner || sequence == g_captureSequence);
    }
    frame = g_captureFrame;
    sequence = g_captureSequence;
    return true;
}
