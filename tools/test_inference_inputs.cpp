#include "Inference.h"
#include <cassert>
#include <iostream>
#include "detection.hpp"
struct OutputProbe : yolos::det::YOLODetector {
    using YOLODetector::checkedOutputShape;
};
int main() {
    unsigned char pixels[12]{};
    DetectObject output[99]{};
    assert(ncnnDetectBgr(pixels, 2, 2, 6, .5f, .5f, output) < 0);
    assert(onnxDetectBgr(pixels, 2, 2, 6, .5f, .5f, output) < 0);
    assert(saga::ncnn_create(L"__missing__.param", L"__missing__.bin", 0, 0) < 0);
    assert(saga::onnx_create(L"__missing__.onnx", L"", false) < 0);
    assert(ncnnDetectBgr(nullptr, -1, 0, 0, .5f, .5f, nullptr) < 0);
    assert(onnxDetectBgr(nullptr, -1, 0, 0, .5f, .5f, nullptr) < 0);
    saga::ncnn_destroy(); saga::onnx_destroy();
    Ort::AllocatorWithDefaultOptions allocator;
    for (const std::vector<int64_t> shape : {std::vector<int64_t>{1,5}, {2,5,10}, {1,5,10,1}}) {
        std::vector<Ort::Value> tensors;
        tensors.push_back(Ort::Value::CreateTensor<float>(allocator,shape.data(),shape.size()));
        bool rejected=false;
        try { OutputProbe::checkedOutputShape(tensors,0); } catch(const std::runtime_error&) { rejected=true; }
        assert(rejected);
    }
    const int64_t shape[]{1,5,10};
    std::vector<Ort::Value> tensors;
    tensors.push_back(Ort::Value::CreateTensor<float>(allocator,shape,3));
    assert(OutputProbe::checkedOutputShape(tensors,0)==std::vector<int64_t>({1,5,10}));
    bool rejected=false;
    try { OutputProbe::checkedOutputShape(tensors,1); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected);
    std::cout << "PASS unloaded/failed model, invalid direct inputs and output tensor rank/batch validation\n";
}
