#include "Inference.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <vector>
#include <string>

#include <opencv2/opencv.hpp>
#include "detection.hpp"



// ============================================================================
// Global state
// ============================================================================
static yolos::det::YOLODetector* g_detector = nullptr;
static std::string               g_last_error;

namespace saga
{
     int onnx_create(const char* MODEL_PATH, const char* labelsPath, bool useGPU)
    {
        // 无条件销毁旧实例
        delete g_detector;
        g_detector = nullptr;
        try {
            g_last_error.clear();
            g_detector = new yolos::det::YOLODetector(MODEL_PATH, labelsPath, useGPU);
            g_detector->setMaxDetections(99);
            return 0;
        } catch (const std::exception& e) {
            g_last_error = e.what();
            return -1;
        }
    }
     int onnx_detect(
        const unsigned char* bmp_data,
        int                  bmp_size,
        float                conf_thres,
        float                nms_thres,
        DetectObject* out_objects)
    {
        if (!g_detector || !bmp_data || bmp_size <= 0 || !out_objects) return -1;
        try {
        cv::Mat img;

        // ---- 快速路径: 24位BGR top-down BMP 直接包像素, 零拷贝零解码 ----
        if (bmp_size >= 54 && bmp_data[0] == 'B' && bmp_data[1] == 'M') {
            const int biSize = *(const int*)(bmp_data + 14);
            const int offBits = *(const int*)(bmp_data + 10);
            const int width = *(const int*)(bmp_data + 18);
            const int height = *(const int*)(bmp_data + 22);   // 负 = top-down
            const int bpp = *(const short*)(bmp_data + 28);
            const int comp = *(const int*)(bmp_data + 30);
            const int stride = (width * 3 + 3) & ~3;

            if (biSize >= 40 && offBits >= 54 && width > 0 && height < 0 &&
                bpp == 24 && comp == 0 &&
                (size_t)offBits + (size_t)stride * (-height) <= (size_t)bmp_size)
            {
                img = cv::Mat(-height, width, CV_8UC3,
                    (void*)(bmp_data + offBits), stride);
            }
        }

        // ---- 兜底: 其他格式/其他来源的 BMP 走通用解码 ----
        if (img.empty()) {
            cv::Mat buf(1, bmp_size, CV_8UC1, const_cast<unsigned char*>(bmp_data));
            img = cv::imdecode(buf, cv::IMREAD_COLOR);
            if (img.empty())
                return -2;
        }

        return onnxDetectBgr(img.data, img.cols, img.rows, img.step, conf_thres, nms_thres, out_objects);

        } catch (const std::exception& e) {
            g_last_error = e.what();
            return -1;
        }
    }

     void onnx_destroy()
    {
        delete g_detector;
        g_detector = nullptr;
    }

     const char* onnx_last_error()
    {
        return g_last_error.c_str();
    }
}

int onnxDetectBgr(const unsigned char* pixels, int width, int height, size_t stride, float conf_thres, float nms_thres, DetectObject* out_objects) {
    cv::Mat img(height, width, CV_8UC3, const_cast<unsigned char*>(pixels), stride);
        const auto& dets = g_detector->detectBorrowed(img, conf_thres, nms_thres);

        int count = 0;
        for (const auto& d : dets) {
            if (count >= 99) break; // Application's fixed DetectObject[99] buffer.
            out_objects[count].x = static_cast<float>(d.box.x);
            out_objects[count].y = static_cast<float>(d.box.y);
            out_objects[count].width = static_cast<float>(d.box.width);
            out_objects[count].height = static_cast<float>(d.box.height);
            out_objects[count].label = d.classId;
            out_objects[count].prob = d.conf;
            ++count;
        }
        return count;
}
