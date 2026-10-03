#pragma once
#include <memory>
#include <cstddef>
struct DetectObject {
    float x, y, width, height;
    int label;
    float prob;
};

namespace saga
{

    // ---- Makcu 串口鼠标（Makcu\Makcu.cpp）----
    int  makcuConnect(int port);
    int  makcuMove(int x, int y);
    int  makcuClick(int key);
    int  makcuMouseDown(int key);
    int  makcuMouseUp(int key);
    int  makcuMouseButtonState(int key);
    int  makcuIsConnected();

    // ---- 模拟轨迹神经网络（Mouse.cpp）----
    int  mc_create(const char* path);
    void mc_calc(float x, float y, float* out);
    void mc_destroy();

    // ---- 采集卡 DirectShow 枚举与取帧（OpnecvCapture.cpp）----
    //     注意：返回文本是 **UTF-8**（内部走 WideCharToMultiByte(CP_UTF8)），
    //     应用层显示/比较前要 utf8ToAnsi，设备名回传前要 ansiToUtf8。
    const char*    enum_video_devices();
    const char*    enum_device_formats(const char* deviceName);
    const char*    setcjk_ex(const char* deviceName, int width, int height,
                             const char* fourcc, int fps, int cropSize);

    // ---- NCNN 推理（NCNN.cpp）----
    int         ncnn_create(const char* param_path, const char* bin_path,
                            int use_GPU, int index_GPU);
    int         ncnn_detect(const unsigned char* bmp_data, int bmp_size,
                            float conf_thres, float nms_thres, DetectObject* out_objects);
    void        ncnn_destroy();
    const char* ncnn_last_error();

    // ---- ONNXRuntime(DirectML) 推理（ONNX.cpp）----
    int         onnx_create(const char* model_path, const char* labelsPath, bool useGPU);
    int         onnx_detect(const unsigned char* bmp_data, int bmp_size,
                            float conf_thres, float nms_thres, DetectObject* out_objects);
    void        onnx_destroy();
    const char* onnx_last_error();

    // ---- NVIDIA TensorRT inference (TensorRT.cpp), optional native runtime ----
    int         trt_create(const char* model_path);
    int         trt_detect(const unsigned char* bmp_data, int bmp_size,
                           float conf_thres, float nms_thres, DetectObject* out_objects);
    void        trt_destroy();
    const char* trt_last_error();
}

struct CaptureFrame {
    std::shared_ptr<unsigned char> owner;
    const unsigned char* bmp = nullptr;
    int size = 0, width = 0, height = 0;
    size_t stride = 0;
};
bool captureAcquire(CaptureFrame& frame, unsigned long long& sequence, unsigned timeoutMs);
void canvasDrawCapture(const CaptureFrame& frame);
int ncnnDetectBgr(const unsigned char*, int, int, size_t, float, float, DetectObject*);
int onnxDetectBgr(const unsigned char*, int, int, size_t, float, float, DetectObject*);
int trtDetectBgr(const unsigned char*, int, int, size_t, float, float, DetectObject*);
