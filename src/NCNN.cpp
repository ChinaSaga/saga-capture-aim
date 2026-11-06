#include "EncodedImage.h"
#include "NcnnPostprocess.h"
#include "TensorPixels.h"
#include "Inference.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "FilePaths.h"
#include <opencv2/opencv.hpp>
#include <immintrin.h>
#include "net.h"
#include <vector>
#include <algorithm>
#include <string>
#include <fstream>
#include <cmath>
#include <map>



// ══════════════════════════════════════════
// 全局状态
// ══════════════════════════════════════════
struct NcnnModel {
    std::unique_ptr<ncnn::Net> net;
    int imgWH = 416;
    cv::Mat letterbox;
    ncnn::Mat input;
    NcnnPostprocess postprocess;
    int inputIndex = -1, outputIndex = -1, last_nw = -1, last_nh = -1;
};
static std::map<std::wstring, std::unique_ptr<NcnnModel>> models;
static NcnnModel* activeModel = nullptr;
static bool g_gpu_inited = false;
static std::string g_last_error;

// ══════════════════════════════════════════
// 解析输入尺寸
// ══════════════════════════════════════════
static void parse_input_size(const wchar_t* param_path, int expectedSize)
{
    std::ifstream f(file_paths::io(param_path));
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line))
    {
        if (line.find("pnnx_189") == std::string::npos &&
            line.find("pnnx_fold_anchor_points") == std::string::npos)
            continue;
        auto pos = line.find("0=");
        if (pos == std::string::npos) continue;
        int anchor_total = std::stoi(line.substr(pos + 2));
        if (anchor_total <= 0 || anchor_total > 1376256) throw std::runtime_error("Invalid NCNN input dimensions");
        // Standard P3/P4/P5 exports use 21/1024 anchors per input pixel;
        // P2/P3/P4/P5 exports (including the supplied group) use 85/1024.
        bool matched = false;
        for (int factor : {21, 85}) {
            const int size = static_cast<int>(std::lround(std::sqrt(anchor_total * 1024.0 / factor)));
            if (size > 0 && size <= 4096 && size % 32 == 0 &&
                int64_t(size)*size*factor == int64_t(anchor_total)*1024 && (!expectedSize || size == expectedSize)) {
                activeModel->imgWH = size; matched = true; break;
            }
        }
        if (!matched) throw std::runtime_error("NCNN anchor layout does not match the input size (P3/P4/P5 or P2/P3/P4/P5 required)");
        break;
    }
}

// ══════════════════════════════════════════
// Internal model lifecycle and encoded network input
// ══════════════════════════════════════════
namespace saga
{
    int ncnn_create(const char* param, const char* bin, int gpu, int index) {
        return ncnn_create(file_paths::wide(param ? param : "", CP_ACP).c_str(), file_paths::wide(bin ? bin : "", CP_ACP).c_str(), gpu, index);
    }
    
        int ncnn_create(const wchar_t* param_path, const wchar_t* bin_path,
            int use_GPU, int index_GPU)
    {
        ncnn_destroy();
        return ncnn_select_cached(param_path, bin_path, use_GPU, index_GPU, 0);
    }
    int ncnn_select_cached(const wchar_t* param_path, const wchar_t* bin_path,
        int use_GPU, int index_GPU, int expectedSize)
    {
        activeModel = nullptr;
        try {
        auto found = models.find(param_path);
        if (found != models.end()) {
            if (expectedSize && found->second->imgWH != expectedSize)
                throw std::runtime_error("Cached NCNN input dimensions do not match the requested size");
            activeModel = found->second.get(); g_last_error.clear(); return 0;
        }
        auto value = std::make_unique<NcnnModel>();
        activeModel = value.get();
        bool want_gpu = (use_GPU != 0);
        if (want_gpu && !g_gpu_inited)
        {
            ncnn::create_gpu_instance();
            g_gpu_inited = true;
        }
        activeModel->net = std::make_unique<ncnn::Net>();
        activeModel->net->opt.use_vulkan_compute = want_gpu;
        if (want_gpu && index_GPU > 0)
            activeModel->net->opt.vulkan_device_index = index_GPU;

        activeModel->net->opt.use_packing_layout = true;
        activeModel->net->opt.use_winograd_convolution = true;
        activeModel->net->opt.use_sgemm_convolution = true;
        activeModel->net->opt.use_shader_local_memory = true;
        activeModel->net->opt.use_cooperative_matrix = true;
        activeModel->net->opt.use_subgroup_ops = true;
        activeModel->net->opt.use_int8_inference = true;
        activeModel->net->opt.flush_denormals = 3;
        activeModel->net->opt.num_threads = 1;            // 单线程
        activeModel->net->opt.use_fp16_packed = true;
        activeModel->net->opt.use_fp16_storage = true;
        activeModel->net->opt.use_fp16_arithmetic = true;

        std::unique_ptr<FILE, decltype(&fclose)> param(file_paths::open(param_path, L"rb"), fclose);
        if (!param || activeModel->net->load_param(param.get()) != 0)
        {
            g_last_error = "param load failed: " + file_paths::narrow(param_path, CP_UTF8);
            activeModel = nullptr;
            return -1;
        }
        std::unique_ptr<FILE, decltype(&fclose)> bin(file_paths::open(bin_path, L"rb"), fclose);
        if (!bin || activeModel->net->load_model(bin.get()) != 0)
        {
            g_last_error = "bin load failed: " + file_paths::narrow(bin_path, CP_UTF8);
            activeModel = nullptr;
            return -2;
        }

        activeModel->inputIndex = activeModel->outputIndex = -1;
        const auto& blobs = activeModel->net->blobs();
        for (size_t i = 0; i < blobs.size(); ++i) {
            if (blobs[i].name == "in0") activeModel->inputIndex = static_cast<int>(i);
            if (blobs[i].name == "out0") activeModel->outputIndex = static_cast<int>(i);
        }
        if (activeModel->inputIndex < 0 || activeModel->outputIndex < 0) {
            g_last_error = "Model requires in0 and out0 blobs";
            activeModel = nullptr;
            return -3;
        }

        // 重置尺寸标记，下次 detect 会重新预分配
        activeModel->postprocess.reset();
        activeModel->last_nw = -1;
        activeModel->last_nh = -1;

        activeModel->imgWH = 416;
        if (expectedSize) activeModel->imgWH = expectedSize;
        parse_input_size(param_path, expectedSize);
        if (expectedSize && activeModel->imgWH != expectedSize) throw std::runtime_error("NCNN dimensions do not match the numeric model filename");
        models.emplace(param_path, std::move(value));
        g_last_error = "";
        return 0;
        } catch (const std::exception& error) {
            g_last_error = error.what();
            activeModel = nullptr;
            return -1;
        }
    }

    
        int ncnn_detect(const unsigned char* bmp_data, int bmp_size,
            float conf_thres, float nms_thres,
            DetectObject* out_objects)
    {
        if (!activeModel || !activeModel->net || !bmp_data || bmp_size <= 0 || !out_objects) return -1;

        // ── 1. 解码: 24位BGR top-down BMP 直接包像素, 零拷贝 ----
        try {
        cv::Mat img;
        encoded_image::BgrView view;
        if (encoded_image::topDownBgr(bmp_data, static_cast<size_t>(bmp_size), view))
            img = cv::Mat(view.height, view.width, CV_8UC3,
                const_cast<unsigned char*>(bmp_data + view.offset), view.stride);
        if (img.empty())
        {
            // 兜底: 其他格式走通用解码
            cv::Mat buf(1, bmp_size, CV_8UC1, const_cast<unsigned char*>(bmp_data));
            img = cv::imdecode(buf, cv::IMREAD_COLOR);
            if (img.empty()) return -2;
        }

        return ncnnDetectBgr(img.data, img.cols, img.rows, img.step, conf_thres, nms_thres, out_objects);
        } catch (const std::exception& error) {
            g_last_error = error.what();
            return -1;
        }
    }

    
        void ncnn_destroy()
    {
        activeModel = nullptr;
        models.clear();
        if (g_gpu_inited) { ncnn::destroy_gpu_instance(); g_gpu_inited = false; }
    }

    
        const char* ncnn_last_error()
    {
        return g_last_error.c_str();
    }

} // namespace saga

int ncnnDetectBgr(const unsigned char* pixels, int width, int height, size_t stride, float conf_thres, float nms_thres, DetectObject* out_objects) {
    if (!activeModel || !activeModel->net || !pixels || !out_objects || width <= 0 || height <= 0 || stride < size_t(width) * 3) {
        g_last_error = "Model is not ready or input frame is invalid";
        return -1;
    }
    try {
    cv::Mat img(height, width, CV_8UC3, const_cast<unsigned char*>(pixels), stride);
        const int img_w = img.cols;
        const int img_h = img.rows;

        // ── 2. Letterbox ───────────────────
        float scale = std::min((float)activeModel->imgWH / img_w, (float)activeModel->imgWH / img_h);
        int nw = static_cast<int>(img_w * scale);
        int nh = static_cast<int>(img_h * scale);
        int pad_x = (activeModel->imgWH - nw) / 2;
        int pad_y = (activeModel->imgWH - nh) / 2;

        const bool tensorChanged = activeModel->input.w != activeModel->imgWH || activeModel->input.h != activeModel->imgWH || activeModel->input.c != 3;
        activeModel->input.create(activeModel->imgWH, activeModel->imgWH, 3);
        if ((nw != activeModel->imgWH || nh != activeModel->imgWH) &&
            (tensorChanged || nw != activeModel->last_nw || nh != activeModel->last_nh)) {
            // Cache normalized padding in the tensor itself: unchanged gray
            // pixels no longer pass through BGR deinterleave every frame.
            // Keep the same rounded reciprocal multiply as the SIMD pixel
            // kernel. /fp:fast can fold constant 114/255 one ULP differently.
            volatile float paddingByte = 114.f;
            activeModel->input.fill(paddingByte * (1.f / 255.f));
        }
        activeModel->last_nw = nw; activeModel->last_nh = nh;

        const cv::Mat* prepared = &img;
        if (img_w != nw || img_h != nh) {
            cv::resize(img, activeModel->letterbox, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
            prepared = &activeModel->letterbox;
        }
        const int offset = pad_y * activeModel->imgWH + pad_x;
        bgrToRgbPlanes(prepared->data, nw, nh, prepared->step,
            static_cast<float*>(activeModel->input.channel(0)) + offset,
            static_cast<float*>(activeModel->input.channel(1)) + offset,
            static_cast<float*>(activeModel->input.channel(2)) + offset, activeModel->imgWH);

        ncnn::Extractor ex = activeModel->net->create_extractor();
        if (ex.input(activeModel->inputIndex, activeModel->input) != 0) return -3;
        ncnn::Mat out;
        if (ex.extract(activeModel->outputIndex, out) != 0) return -3;

        return activeModel->postprocess.run(out, scale, pad_x, pad_y, img_w, img_h,
            conf_thres, nms_thres, out_objects);
    } catch (const std::exception& error) {
        g_last_error = error.what();
        return -1;
    }
}
