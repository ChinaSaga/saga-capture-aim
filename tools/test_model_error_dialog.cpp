#include "ModelStatus.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#pragma comment(lib,"user32.lib")
struct DialogCheck { HWND owner=nullptr; int button=IDCANCEL; std::atomic<bool> seen{false},valid{false}; };
static BOOL CALLBACK inspect(HWND window,LPARAM argument) {
    wchar_t type[64]{};GetClassNameW(window,type,_countof(type));
    if(wcscmp(type,L"#32770")) return TRUE;
    if(!IsWindowVisible(window)) return TRUE; // Enumeration can observe construction before ShowWindow.
    auto& check=*reinterpret_cast<DialogCheck*>(argument);
    std::wstring contents;
    EnumChildWindows(window,[](HWND child,LPARAM arg)->BOOL {
        wchar_t text[8192]{};GetWindowTextW(child,text,_countof(text));
        reinterpret_cast<std::wstring*>(arg)->append(text);return TRUE;
    },reinterpret_cast<LPARAM>(&contents));
    check.valid=IsWindowVisible(window) && GetWindow(window,GW_OWNER)==check.owner &&
        (GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0 &&
        (!check.owner || !IsWindowEnabled(check.owner)) &&
        GetDlgItem(window,IDRETRY) && GetDlgItem(window,IDCANCEL) &&
        contents.find(L"missing.onnx")!=std::wstring::npos && contents.find(L"推理已停止")!=std::wstring::npos &&
        contents.find(L"中文😀")!=std::wstring::npos;
    if(!check.valid) std::cerr<<"dialog visible="<<IsWindowVisible(window)<<" owner="<<(GetWindow(window,GW_OWNER)==check.owner)
        <<" topmost="<<bool(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)<<" ownerDisabled="<<!IsWindowEnabled(check.owner)
        <<" text="<<(contents.find(L"missing.onnx")!=std::wstring::npos)<<" unicode="<<(contents.find(L"中文😀")!=std::wstring::npos)<<'\n';
    check.seen=true;
    PostMessageW(window,WM_COMMAND,MAKEWPARAM(check.button,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(window,check.button)));
    return FALSE;
}
int main() {
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"ModelErrorDialogTest";
    assert(RegisterClassW(&wc));
    HWND owner=CreateWindowW(wc.lpszClassName,L"Model dialog regression",WS_OVERLAPPEDWINDOW,0,0,300,180,nullptr,nullptr,wc.hInstance,nullptr);
    assert(owner);
    const auto message=model_status::failure(L"D:\\中文😀\\模型数据","missing",2,false,"模型文件不存在。");
    for(int button:{IDRETRY,IDCANCEL}) {
        ShowWindow(owner,SW_MINIMIZE);
        DialogCheck check;check.owner=owner;check.button=button;
        std::atomic<DWORD> thread{0};std::atomic<bool> done{false};
        int result=0;
        // The real caller is an inference worker, while the owner lives on the UI thread.
        std::thread prompt([&] {
            thread=GetCurrentThreadId();result=model_status::show(owner,message);done=true;
        });
        std::thread click([&] {
            const auto deadline=GetTickCount64()+5000;
            while(!check.seen && GetTickCount64()<deadline) {
                if(thread) EnumThreadWindows(thread,inspect,reinterpret_cast<LPARAM>(&check));Sleep(10);
            }
            // A broken dialog must not leave a regression process running forever.
            if(!check.seen) TerminateProcess(GetCurrentProcess(),9);
        });
        while(!done) {
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(1);
        }
        prompt.join();
        click.join();
        if(result!=button || !IsWindowEnabled(owner) || IsIconic(owner)) std::cerr<<"result="<<result<<" expected="<<button<<" enabled="<<IsWindowEnabled(owner)<<" iconic="<<IsIconic(owner)<<'\n';
        assert(check.seen && check.valid && result==button && IsWindowEnabled(owner) && !IsIconic(owner));
    }
    DestroyWindow(owner);
    std::cout<<"PASS real model error dialogs: visible, owned, topmost, Unicode path, minimized owner restored, Retry and Cancel\n";
}
