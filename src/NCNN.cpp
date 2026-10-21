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



// ══════════════════════════════════════════
// 全局状态
// ══════════════════════════════════════════
static ncnn::Net* g_net = nullptr;
static bool          g_gpu_inited = false;
static std::string   g_last_error;
static int           imgWH = 416;

// 可复用缓冲区
static cv::Mat                    g_letterbox;
static ncnn::Mat g_input;
static NcnnPostprocess g_postprocess;
static int g_inputIndex = -1, g_outputIndex = -1;

// letterbox ROI 缓存：ROI 不变时灰边不需要重填
static int g_last_nw = -1;
static int g_last_nh = -1;

// ══════════════════════════════════════════
// 解析输入尺寸
// ══════════════════════════════════════════
static void parse_input_size(const wchar_t* param_path)
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
        imgWH = static_cast<int>(std::sqrt(anchor_total * 1024.0 / 21.0));
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
        try {
        // 清理旧实例
        if (g_net) { delete g_net; g_net = nullptr; }
        if (g_gpu_inited)
        {
            ncnn::destroy_gpu_instance();
            g_gpu_inited = false;
        }

        bool want_gpu = (use_GPU != 0);
        if (want_gpu)
        {
            ncnn::create_gpu_instance();
            g_gpu_inited = true;
        }
        g_net = new ncnn::Net();
        g_net->opt.use_vulkan_compute = want_gpu;
        if (want_gpu && index_GPU > 0)
            g_net->opt.vulkan_device_index = index_GPU;

        g_net->opt.use_packing_layout = true;
        g_net->opt.use_winograd_convolution = true;
        g_net->opt.use_sgemm_convolution = true;
        g_net->opt.use_shader_local_memory = true;
        g_net->opt.use_cooperative_matrix = true;
        g_net->opt.use_subgroup_ops = true;
        g_net->opt.use_int8_inference = true;
        g_net->opt.flush_denormals = 3;
        g_net->opt.num_threads = 1;            // 单线程
        g_net->opt.use_fp16_packed = true;
        g_net->opt.use_fp16_storage = true;
        g_net->opt.use_fp16_arithmetic = true;

        std::unique_ptr<FILE, decltype(&fclose)> param(file_paths::open(param_path, L"rb"), fclose);
        if (!param || g_net->load_param(param.get()) != 0)
        {
            g_last_error = "param load failed: " + file_paths::narrow(param_path, CP_UTF8);
            delete g_net; g_net = nullptr;
            return -1;
        }
        std::unique_ptr<FILE, decltype(&fclose)> bin(file_paths::open(bin_path, L"rb"), fclose);
        if (!bin || g_net->load_model(bin.get()) != 0)
        {
            g_last_error = "bin load failed: " + file_paths::narrow(bin_path, CP_UTF8);
            delete g_net; g_net = nullptr;
            return -2;
        }

        g_inputIndex = g_outputIndex = -1;
        const auto& blobs = g_net->blobs();
        for (size_t i = 0; i < blobs.size(); ++i) {
            if (blobs[i].name == "in0") g_inputIndex = static_cast<int>(i);
            if (blobs[i].name == "out0") g_outputIndex = static_cast<int>(i);
        }
        if (g_inputIndex < 0 || g_outputIndex < 0) {
            g_last_error = "Model requires in0 and out0 blobs";
            delete g_net; g_net = nullptr;
            return -3;
        }

        // 重置尺寸标记，下次 detect 会重新预分配
        g_postprocess.reset();
        g_last_nw = -1;
        g_last_nh = -1;

        imgWH = 416;
        parse_input_size(param_path);
        g_last_error = "";
        return 0;
        } catch (const std::exception& error) {
            g_last_error = error.what();
            ncnn_destroy();
            return -1;
        }
    }

    
        int ncnn_detect(const unsigned char* bmp_data, int bmp_size,
            float conf_thres, float nms_thres,
            DetectObject* out_objects)
    {
        if (!g_net || !bmp_data || bmp_size <= 0 || !out_objects) return -1;

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
        if (g_net) { delete g_net; g_net = nullptr; }
        if (g_gpu_inited)
        {
            ncnn::destroy_gpu_instance();
            g_gpu_inited = false;
        }
        g_postprocess.reset();
        g_input.release();
        g_letterbox.release();
        g_last_nw = -1;
        g_last_nh = -1;
    }

    
        const char* ncnn_last_error()
    {
        return g_last_error.c_str();
    }

} // namespace saga

int ncnnDetectBgr(const unsigned char* pixels, int width, int height, size_t stride, float conf_thres, float nms_thres, DetectObject* out_objects) {
    if (!g_net || !pixels || !out_objects || width <= 0 || height <= 0 || stride < size_t(width) * 3) {
        g_last_error = "Model is not ready or input frame is invalid";
        return -1;
    }
    try {
    cv::Mat img(height, width, CV_8UC3, const_cast<unsigned char*>(pixels), stride);
        const int img_w = img.cols;
        const int img_h = img.rows;

        // ── 2. Letterbox ───────────────────
        float scale = std::min((float)imgWH / img_w, (float)imgWH / img_h);
        int nw = static_cast<int>(img_w * scale);
        int nh = static_cast<int>(img_h * scale);
        int pad_x = (imgWH - nw) / 2;
        int pad_y = (imgWH - nh) / 2;

        const bool tensorChanged = g_input.w != imgWH || g_input.h != imgWH || g_input.c != 3;
        g_input.create(imgWH, imgWH, 3);
        if ((nw != imgWH || nh != imgWH) &&
            (tensorChanged || nw != g_last_nw || nh != g_last_nh)) {
            // Cache normalized padding in the tensor itself: unchanged gray
            // pixels no longer pass through BGR deinterleave every frame.
            // Keep the same rounded reciprocal multiply as the SIMD pixel
            // kernel. /fp:fast can fold constant 114/255 one ULP differently.
            volatile float paddingByte = 114.f;
            g_input.fill(paddingByte * (1.f / 255.f));
        }
        g_last_nw = nw; g_last_nh = nh;

        const cv::Mat* prepared = &img;
        if (img_w != nw || img_h != nh) {
            cv::resize(img, g_letterbox, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
            prepared = &g_letterbox;
        }
        const int offset = pad_y * imgWH + pad_x;
        bgrToRgbPlanes(prepared->data, nw, nh, prepared->step,
            static_cast<float*>(g_input.channel(0)) + offset,
            static_cast<float*>(g_input.channel(1)) + offset,
            static_cast<float*>(g_input.channel(2)) + offset, imgWH);

        ncnn::Extractor ex = g_net->create_extractor();
        if (ex.input(g_inputIndex, g_input) != 0) return -3;
        ncnn::Mat out;
        if (ex.extract(g_outputIndex, out) != 0) return -3;

        return g_postprocess.run(out, scale, pad_x, pad_y, img_w, img_h,
            conf_thres, nms_thres, out_objects);
    } catch (const std::exception& error) {
        g_last_error = error.what();
        return -1;
    }
}
