#include "EncodedImage.h"
#include "FilePaths.h"
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
    int onnx_create(const char* model, const char* labels, bool gpu) {
        return onnx_create(file_paths::wide(model ? model : "", CP_ACP).c_str(), file_paths::wide(labels ? labels : "", CP_ACP).c_str(), gpu);
    }
     int onnx_create(const wchar_t* MODEL_PATH, const wchar_t* labelsPath, bool useGPU)
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
        encoded_image::BgrView view;
        if (encoded_image::topDownBgr(bmp_data, static_cast<size_t>(bmp_size), view))
            img = cv::Mat(view.height, view.width, CV_8UC3,
                const_cast<unsigned char*>(bmp_data + view.offset), view.stride);
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
    if (!g_detector || !pixels || !out_objects || width <= 0 || height <= 0 || stride < size_t(width) * 3) {
        g_last_error = "Model is not ready or input frame is invalid";
        return -1;
    }
    try {
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
    } catch (const std::exception& error) {
        g_last_error = error.what();
        return -1;
    }
}
