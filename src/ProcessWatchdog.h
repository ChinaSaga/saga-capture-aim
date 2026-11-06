#pragma once
#include <windows.h>
#include <algorithm>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdint>
#pragma comment(lib,"shell32.lib")

namespace process_watchdog {
inline HANDLE stopEvent=nullptr;
inline bool supervised=false;
inline std::wstring executable() {
    wchar_t path[32768]{};
    DWORD size=GetModuleFileNameW(nullptr,path,32768);
    return size && size<32768?std::wstring(path,size):std::wstring{};
}
inline std::wstring number(HANDLE handle) { return std::to_wstring(reinterpret_cast<uintptr_t>(handle)); }
inline bool spawn(const std::wstring& exe, const std::wstring& arguments,
    const std::vector<HANDLE>& inherited, bool hidden, PROCESS_INFORMATION& process) {
    SIZE_T bytes=0;
    InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes,1,0,&bytes)) return false;
    if (!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        const_cast<HANDLE*>(inherited.data()),inherited.size()*sizeof(HANDLE),nullptr,nullptr)) {
        DeleteProcThreadAttributeList(attributes); return false;
    }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow=hidden?SW_HIDE:SW_SHOWNORMAL;
    startup.lpAttributeList=attributes;
    std::wstring command=L"\""+exe+L"\" "+arguments;
    const auto directory=exe.substr(0,exe.find_last_of(L"\\/"));
    const BOOL ok=CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT | (hidden?CREATE_NO_WINDOW:0),nullptr,directory.c_str(),
        &startup.StartupInfo,&process);
    DeleteProcThreadAttributeList(attributes);
    return ok!=FALSE;
}
inline void normalExit() { if (stopEvent) SetEvent(stopEvent); }
inline int monitor(HANDLE parent, HANDLE stop, const std::wstring& exe, DWORD delayMs=2000) {
    unsigned failures=0;
    for (;;) {
        const ULONGLONG began=GetTickCount64();
        HANDLE handles[]={stop,parent};
        const DWORD wait=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
        if (wait!=WAIT_OBJECT_0+1) { CloseHandle(parent); CloseHandle(stop); return 0; }
        DWORD code=0;
        const bool known=GetExitCodeProcess(parent,&code)!=FALSE;
        CloseHandle(parent);
        if (!known || code==0 || WaitForSingleObject(stop,0)==WAIT_OBJECT_0) { CloseHandle(stop); return 0; }
        // Bounded backoff avoids repeatedly hammering a failing driver on startup.
        if (GetTickCount64()-began>=60000) failures=0;
        if (failures<15) ++failures;
        const DWORD delay=(std::min)(DWORD(30000),delayMs*failures);
        if (WaitForSingleObject(stop,delay)==WAIT_OBJECT_0) { CloseHandle(stop); return 0; }
        PROCESS_INFORMATION child{};
        if (!spawn(exe,L"--supervised "+number(stop),{stop},false,child)) { CloseHandle(stop); return 1; }
        CloseHandle(child.hThread);
        parent=child.hProcess;
    }
}
// Called before application/hardware initialization. The watchdog uses this EXE
// in a separate hidden process; it never creates the UI or initializes devices.
inline bool commandLine(int& result) {
    int count=0;
    auto argv=CommandLineToArgvW(GetCommandLineW(),&count);
    if (!argv) return false;
    const bool watcher=count>1 && std::wstring(argv[1])==L"--watchdog";
    const bool child=count>1 && std::wstring(argv[1])==L"--supervised";
    if (!watcher && !child) { LocalFree(argv); return false; }
    HANDLE process=nullptr, stop=nullptr;
    try {
        if (count!=(watcher?4:3)) throw 0;
        auto parse=[](const wchar_t* value) {
            size_t used=0; const auto number=std::stoull(value,&used);
            if (!number || value[used]) throw 0;
            auto handle=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number));
            DWORD flags=0;if(!GetHandleInformation(handle,&flags)) throw 0;
            return handle;
        };
        if (watcher) { process=parse(argv[2]);stop=parse(argv[3]); }
        else stop=parse(argv[2]);
    } catch (...) { LocalFree(argv);result=2;return true; }
    LocalFree(argv);
    if (watcher) { result=monitor(process,stop,executable());return true; }
    stopEvent=stop;supervised=true;
    return false;
}
inline bool start() {
    if (supervised) return true;
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    stopEvent=CreateEventW(&security,TRUE,FALSE,nullptr);
    if (!stopEvent) return false;
    HANDLE parent=nullptr;
    if (!DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&parent,
        SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,TRUE,0)) {
        CloseHandle(stopEvent);stopEvent=nullptr;return false;
    }
    PROCESS_INFORMATION watcher{};
    const bool ok=spawn(executable(),L"--watchdog "+number(parent)+L" "+number(stopEvent),{parent,stopEvent},true,watcher);
    CloseHandle(parent);
    if(ok) { CloseHandle(watcher.hThread);CloseHandle(watcher.hProcess); }
    else { CloseHandle(stopEvent);stopEvent=nullptr; }
    return ok;
}
}
