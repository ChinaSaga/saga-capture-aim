#pragma once
#include "FilePaths.h"
#include <string>

namespace startup_session {
struct Capture {
    std::string device, codec;
    int width=0, height=0, fps=0;
    bool valid() const { return !device.empty() && !codec.empty() && width>0 && height>0 && fps>0; }
};
inline Capture load(const std::filesystem::path& ini) {
    Capture result;
    if (!file_paths::profileInt("采集恢复","已开始",0,ini.c_str())) return result;
    char value[1024]{};
    file_paths::profileString("采集恢复","设备","",value,sizeof(value),ini.c_str()); result.device=value;
    file_paths::profileString("采集恢复","编码","",value,sizeof(value),ini.c_str()); result.codec=value;
    result.width=file_paths::profileInt("采集恢复","宽",0,ini.c_str());
    result.height=file_paths::profileInt("采集恢复","高",0,ini.c_str());
    result.fps=file_paths::profileInt("采集恢复","帧率",0,ini.c_str());
    return result;
}
inline bool save(const std::filesystem::path& ini, const Capture& capture, int manualSize) {
    if (!capture.valid()) return false;
    const auto put=[&](const char* key,const std::string& value) {
        return file_paths::writeProfile("采集恢复",key,value.c_str(),ini.c_str())!=FALSE;
    };
    // Publish the enabled flag last, so partial writes never restore mixed settings.
    return put("已开始","0") && put("设备",capture.device) && put("编码",capture.codec) &&
        put("宽",std::to_string(capture.width)) && put("高",std::to_string(capture.height)) &&
        put("帧率",std::to_string(capture.fps)) &&
        file_paths::writeProfile("默认","组合框_视频大小",std::to_string((manualSize-160)/32).c_str(),ini.c_str()) &&
        put("已开始","1");
}
inline bool savePreview(const std::filesystem::path& ini, bool enabled) {
    return file_paths::writeProfile("默认","选择框_实时刷新",enabled?"1":"0",ini.c_str())!=FALSE;
}
}
