#include "ProcessWatchdog.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cassert>

static std::wstring environment(const wchar_t* name) {
    wchar_t value[32768]{};GetEnvironmentVariableW(name,value,32768);return value;
}
static int countLines(const std::filesystem::path& file) {
    std::ifstream input(file);int count=0;std::string line;
    while(std::getline(input,line)) ++count;
    return count;
}
int wmain(int argc,wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    int result=0;
    if(process_watchdog::commandLine(result)) return result;
    if(process_watchdog::supervised) {
        const auto file=std::filesystem::path(environment(L"SAGA_TEST_WATCH_FILE"));
        const int count=countLines(file)+1;
        {std::ofstream out(file,std::ios::app);out<<GetCurrentProcessId()<<'\n';}
        // Calling start again must not create another supervisor.
        assert(process_watchdog::start());
        const auto mode=environment(L"SAGA_TEST_WATCH_MODE");
        if(mode==L"stop") {process_watchdog::normalExit();TerminateProcess(GetCurrentProcess(),42);}
        if(mode==L"repeat" && count<3) RaiseException(0xE0001234,EXCEPTION_NONCONTINUABLE,0,nullptr);
        return 0;
    }
    if(argc==2) {Sleep(100);return _wtoi(argv[1]);}
    const auto file=std::filesystem::temp_directory_path()/(L"saga-watch-test-"+std::to_wstring(GetCurrentProcessId())+L".txt");
    assert(!std::filesystem::exists(file));
    SetEnvironmentVariableW(L"SAGA_TEST_WATCH_FILE",file.c_str());
    for(const auto* mode:{L"normal",L"crash",L"stop",L"repeat",L"shutdown"}) {
        SetEnvironmentVariableW(L"SAGA_TEST_WATCH_MODE",mode);
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        HANDLE stop=CreateEventW(&security,TRUE,FALSE,nullptr);assert(stop);
        PROCESS_INFORMATION parent{},watcher{};
        const bool normal=std::wstring(mode)==L"normal";
        assert(process_watchdog::spawn(process_watchdog::executable(),normal?L"0":L"37",{stop},true,parent));
        assert(SetHandleInformation(parent.hProcess,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT));
        assert(process_watchdog::spawn(process_watchdog::executable(),L"--watchdog "+
            process_watchdog::number(parent.hProcess)+L" "+process_watchdog::number(stop),{parent.hProcess,stop},true,watcher));
        if(std::wstring(mode)==L"shutdown") SetEvent(stop);
        const DWORD wait=WaitForSingleObject(watcher.hProcess,20000);
        if(wait!=WAIT_OBJECT_0) {SetEvent(stop);WaitForSingleObject(watcher.hProcess,2000);}
        assert(wait==WAIT_OBJECT_0);
        DWORD code=99;assert(GetExitCodeProcess(watcher.hProcess,&code) && code==0);
        WaitForSingleObject(parent.hProcess,2000);
        const int expected=normal || std::wstring(mode)==L"shutdown"?0:std::wstring(mode)==L"repeat"?3:1;
        assert(countLines(file)==expected);
        CloseHandle(watcher.hThread);CloseHandle(watcher.hProcess);
        CloseHandle(parent.hThread);CloseHandle(parent.hProcess);CloseHandle(stop);
        std::filesystem::remove(file);
    }
    std::cout<<"PASS watchdog: normal exit, crash restart, repeated exceptions, stop event, shutdown, no nested supervisor\n";
}
