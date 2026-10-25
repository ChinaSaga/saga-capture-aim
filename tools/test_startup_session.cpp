#include "StartupSession.h"
#include <cassert>
#include <iostream>

int main() {
    const auto ini=std::filesystem::temp_directory_path()/(L"saga-startup-test-"+std::to_wstring(GetCurrentProcessId())+L".ini");
    assert(!std::filesystem::exists(ini));
    assert(!startup_session::load(ini).valid());
    const startup_session::Capture capture{"中文采集卡","NV12",1920,1080,240};
    assert(startup_session::save(ini,capture,416));
    assert(startup_session::savePreview(ini,false));
    auto loaded=startup_session::load(ini);
    assert(loaded.valid() && loaded.device==capture.device && loaded.codec=="NV12" &&
        loaded.width==1920 && loaded.height==1080 && loaded.fps==240);
    assert(file_paths::profileInt("默认","选择框_实时刷新",9,ini.c_str())==0);
    assert(file_paths::profileInt("默认","组合框_视频大小",0,ini.c_str())==8);
    assert(startup_session::savePreview(ini,true));
    assert(file_paths::profileInt("默认","选择框_实时刷新",9,ini.c_str())==1);
    assert(!startup_session::save(ini,{},320));
    assert(startup_session::load(ini).device==capture.device);
    assert(file_paths::writeProfile("采集恢复","已开始","0",ini.c_str()));
    assert(!startup_session::load(ini).valid());
    assert(startup_session::save(ini,capture,320));
    assert(SetFileAttributesW(ini.c_str(),FILE_ATTRIBUTE_READONLY));
    assert(!startup_session::save(ini,{"other","RGB24",1280,720,60},256));
    assert(!startup_session::savePreview(ini,false));
    assert(SetFileAttributesW(ini.c_str(),FILE_ATTRIBUTE_NORMAL));
    assert(startup_session::load(ini).device==capture.device);
    assert(std::filesystem::remove(ini));
    std::cout<<"PASS startup session: capture restore, preview on/off, Unicode, manual range, incomplete/failed saves\n";
}
