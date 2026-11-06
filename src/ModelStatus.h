#pragma once
#include "FilePaths.h"

namespace model_status {
enum class State { Idle, Loading, Ready, Failed };
inline const wchar_t* label(State state) {
    switch (state) {
    case State::Loading: return L"模型加载中…";
    case State::Ready: return L"模型已加载";
    case State::Failed: return L"模型不可用，推理已停止";
    default: return L"模型尚未加载";
    }
}
inline std::wstring displayPath(const std::filesystem::path& path) {
    auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + value.substr(8);
    if (value.starts_with(L"\\\\?\\")) return value.substr(4);
    return value;
}
inline std::wstring failure(const std::filesystem::path& directory, const std::string& name,
    int engine, bool group, const std::string& reason) {
    const auto model = file_paths::wide(name);
    std::wstring text = L"推理已停止。\n\n模型：" + (model.empty() ? std::wstring(L"（未选择）") : model);
    text += L"\n引擎：";
    text += engine == 1 ? L"NCNN" : engine == 2 ? L"ONNX" : L"TensorRT";
    text += L"\n原因：" + file_paths::wide(reason) + L"\n\n实际查找位置：\n";
    const auto base = group ? directory / model / L"<尺寸>" : directory / model;
    if (model.empty()) text += displayPath(directory);
    else if (engine == 1) text += displayPath(base) + L".param\n" + displayPath(base) + L".bin";
    else text += displayPath(base) + (engine == 2 ? L".onnx" : L".trt");
    if (group) text += L"\n尺寸为 160～640，步长 32；至少需要一个可用档位。";
    text += L"\n\n修复文件后点“重试”重新加载；点“取消”保持停止，可在网页重新选择模型或引擎。\n采集启动成功不代表模型加载成功。";
    return text;
}
inline int show(HWND owner, const std::wstring& message) {
    if (!IsWindow(owner)) owner = nullptr;
    if (owner && IsIconic(owner)) ShowWindowAsync(owner, SW_RESTORE);
    return MessageBoxW(owner, message.c_str(), L"模型错误 · 推理已停止",
        MB_RETRYCANCEL | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}
}
