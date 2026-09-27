#include "NcnnPostprocess.h"
#include "TensorPixels.h"
#include "Inference.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
static void parse_input_size(const char* param_path)
{
    std::ifstream f(param_path);
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
        imgWH = static_cast<int>(std::sqrt(anchor_total * 1024.0 / 21.0));
        break;
    }
}

// ══════════════════════════════════════════
// Internal model lifecycle and encoded network input
// ══════════════════════════════════════════
namespace saga
{
    
        int ncnn_create(const char* param_path, const char* bin_path,
            int use_GPU, int index_GPU)
    {
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

        if (g_net->load_param(param_path) != 0)
        {
            g_last_error = "param load failed: " + std::string(param_path);
            delete g_net; g_net = nullptr;
            return -1;
        }
        if (g_net->load_model(bin_path) != 0)
        {
            g_last_error = "bin load failed: " + std::string(bin_path);
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
    }

    
        int ncnn_detect(const unsigned char* bmp_data, int bmp_size,
            float conf_thres, float nms_thres,
            DetectObject* out_objects)
    {
        if (!g_net || !bmp_data || bmp_size <= 0 || !out_objects) return -1;

        // ── 1. 解码: 24位BGR top-down BMP 直接包像素, 零拷贝 ----
        cv::Mat img;
        if (bmp_size >= 54 && bmp_data[0] == 'B' && bmp_data[1] == 'M')
        {
            const int biSize = *(const int*)(bmp_data + 14);
            const int offBits = *(const int*)(bmp_data + 10);
            const int w0 = *(const int*)(bmp_data + 18);
            const int h0 = *(const int*)(bmp_data + 22);   // 负 = top-down
            const int bpp = *(const short*)(bmp_data + 28);
            const int comp = *(const int*)(bmp_data + 30);
            const int st = (w0 * 3 + 3) & ~3;

            if (biSize >= 40 && offBits >= 54 && w0 > 0 && h0 < 0 &&
                bpp == 24 && comp == 0 &&
                (size_t)offBits + (size_t)st * (-h0) <= (size_t)bmp_size)
            {
                img = cv::Mat(-h0, w0, CV_8UC3,
                    (void*)(bmp_data + offBits), st);
            }
        }
        if (img.empty())
        {
            // 兜底: 其他格式走通用解码
            cv::Mat buf(1, bmp_size, CV_8UC1, const_cast<unsigned char*>(bmp_data));
            img = cv::imdecode(buf, cv::IMREAD_COLOR);
            if (img.empty()) return -2;
        }

        return ncnnDetectBgr(img.data, img.cols, img.rows, img.step, conf_thres, nms_thres, out_objects);

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
        g_last_nw = -1;
        g_last_nh = -1;
    }

    
        const char* ncnn_last_error()
    {
        return g_last_error.c_str();
    }

} // namespace saga

int ncnnDetectBgr(const unsigned char* pixels, int width, int height, size_t stride, float conf_thres, float nms_thres, DetectObject* out_objects) {
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
        ex.input(g_inputIndex, g_input);
        ncnn::Mat out;
        if (ex.extract(g_outputIndex, out) != 0) return -3;

        return g_postprocess.run(out, scale, pad_x, pad_y, img_w, img_h,
            conf_thres, nms_thres, out_objects);
}
