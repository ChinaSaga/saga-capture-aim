// Uses the official NVIDIA NVAPI headers (MIT), fetched into .deps/nvapi.
// This tool changes only one executable's power-management setting, never
// base/global profiles. Existing third-party application profiles are refused.
#include <windows.h>
#include <nvapi.h>
#include <NvApiDriverSettings.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using Query = void* (__cdecl*)(unsigned int);
static Query query;
template<class T> T bind(unsigned id) {
    auto address=query(id); if(!address) throw std::runtime_error("NVAPI function unavailable");
    return reinterpret_cast<T>(address);
}
static void check(NvAPI_Status status,const char* stage) {
    if(status!=NVAPI_OK) throw std::runtime_error(std::string(stage)+": NVAPI status="+std::to_string(status));
}
static void copyName(NvAPI_UnicodeString& dest,const std::wstring& text) {
    if(text.size()>=NVAPI_UNICODE_STRING_MAX) throw std::runtime_error("Name too long");
    wcsncpy_s(reinterpret_cast<wchar_t*>(dest),NVAPI_UNICODE_STRING_MAX,text.c_str(),_TRUNCATE);
}
int wmain(int argc,wchar_t** argv) {
    if(argc<3) { std::cerr<<"Usage: NvPowerProfile query|set|remove executable [backup_file]\n"; return 2; }
    try {
        auto dll=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!dll) throw std::runtime_error("NVIDIA NVAPI is not installed");
        query=reinterpret_cast<Query>(GetProcAddress(dll,"nvapi_QueryInterface"));
        if(!query) throw std::runtime_error("Missing NVAPI interface");
        check(bind<decltype(&NvAPI_Initialize)>(0x0150e828)(),"Initialize");
        NvDRSSessionHandle session{};
        check(bind<decltype(&NvAPI_DRS_CreateSession)>(0x0694d52e)(&session),"Create session");
        struct Guard { NvDRSSessionHandle h; ~Guard(){ bind<decltype(&NvAPI_DRS_DestroySession)>(0xdad9cff8)(h); } } guard{session};
        check(bind<decltype(&NvAPI_DRS_LoadSettings)>(0x375dbd6b)(session),"Load settings");
        const std::wstring action=argv[1];
        const std::wstring exe=std::filesystem::path(argv[2]).filename().wstring();
        NvAPI_UnicodeString appName{},profileName{};
        copyName(appName,exe); copyName(profileName,L"Saga TensorRT performance - "+exe);
        NvDRSProfileHandle profile{},owned{}; NVDRS_APPLICATION app{}; app.version=NVDRS_APPLICATION_VER;
        const auto found=bind<decltype(&NvAPI_DRS_FindApplicationByName)>(0xeee566b2)(session,appName,&profile,&app);
        const auto ownFound=bind<decltype(&NvAPI_DRS_FindProfileByName)>(0x7e4a9a0b)(session,profileName,&owned);
        if(action==L"query") {
            if(found==NVAPI_EXECUTABLE_NOT_FOUND) { std::cout<<"application_profile=absent\n"; return 0; }
            check(found,"Find application");
            NVDRS_SETTING setting{}; setting.version=NVDRS_SETTING_VER;
            check(bind<decltype(&NvAPI_DRS_GetSetting)>(0x73bf8338)(session,profile,PREFERRED_PSTATE_ID,&setting),"Get power setting");
            std::cout<<"power_mode="<<setting.u32CurrentValue<<" location="<<setting.settingLocation<<" owned="<<(ownFound==NVAPI_OK && owned==profile)<<'\n'; return 0;
        }
        if(action!=L"set" && action!=L"remove") return 2;
        if(found!=NVAPI_OK && found!=NVAPI_EXECUTABLE_NOT_FOUND) check(found,"Find application");
        if(ownFound!=NVAPI_OK && ownFound!=NVAPI_PROFILE_NOT_FOUND) check(ownFound,"Find own profile");
        if(found==NVAPI_OK && (ownFound!=NVAPI_OK || owned!=profile)) throw std::runtime_error("Existing application profile belongs to another owner; no changes made");
        if(action==L"remove") {
            if(ownFound==NVAPI_PROFILE_NOT_FOUND) { std::cout<<"Already absent\n"; return 0; }
            check(bind<decltype(&NvAPI_DRS_DeleteProfile)>(0x17093206)(session,owned),"Delete own profile");
        } else {
            if(argc<4) throw std::runtime_error("Backup filename is required before applying settings");
            NvAPI_UnicodeString backup{}; copyName(backup,std::filesystem::absolute(argv[3]).wstring());
            if(!std::filesystem::exists(argv[3])) check(bind<decltype(&NvAPI_DRS_SaveSettingsToFile)>(0x2be25df8)(session,backup),"Back up driver settings");
            if(ownFound==NVAPI_PROFILE_NOT_FOUND) {
                NVDRS_PROFILE info{}; info.version=NVDRS_PROFILE_VER;
                memcpy(info.profileName,profileName,sizeof(profileName));
                check(bind<decltype(&NvAPI_DRS_CreateProfile)>(0xcc176068)(session,&info,&owned),"Create application profile");
            }
            if(found==NVAPI_EXECUTABLE_NOT_FOUND) {
                memcpy(app.appName,appName,sizeof(appName));
                check(bind<decltype(&NvAPI_DRS_CreateApplication)>(0x4347a9de)(session,owned,&app),"Associate executable");
            }
            NVDRS_SETTING setting{}; setting.version=NVDRS_SETTING_VER; setting.settingId=PREFERRED_PSTATE_ID; setting.settingType=NVDRS_DWORD_TYPE;
            setting.u32CurrentValue=PREFERRED_PSTATE_PREFER_MAX;
            check(bind<decltype(&NvAPI_DRS_SetSetting)>(0x577dd202)(session,owned,&setting),"Set prefer maximum performance");
        }
        check(bind<decltype(&NvAPI_DRS_SaveSettings)>(0xfcbc7e14)(session),"Save settings");
        std::cout<<(action==L"set" ? "Per-application maximum performance saved; restart the application\n" : "Own application profile removed; restart the application\n"); return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
