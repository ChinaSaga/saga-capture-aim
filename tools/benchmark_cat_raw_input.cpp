#define _WIN32_WINNT 0x0601
#include "../src/Makcu/cat/include/catdevice.h"
#include "../src/Makcu/cat/src/aes_functions.cpp"
#include "../src/Makcu/cat/src/cat_net_core.cpp"
#include "../src/Makcu/cat/src/catdevice.cpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"user32.lib")
using Clock=std::chrono::steady_clock;
struct Counts { long long x=0,y=0;unsigned events=0; };
std::mutex recordsMutex;std::map<HANDLE,Counts> records;
LRESULT CALLBACK proc(HWND w,UINT m,WPARAM a,LPARAM b) {
 if(m==WM_INPUT) {
  UINT size=0;GetRawInputData((HRAWINPUT)b,RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER));
  std::vector<unsigned char> data(size);
  if(GetRawInputData((HRAWINPUT)b,RID_INPUT,data.data(),&size,sizeof(RAWINPUTHEADER))==size) {
   auto* raw=(RAWINPUT*)data.data();
   if(raw->header.dwType==RIM_TYPEMOUSE) {std::lock_guard<std::mutex> lock(recordsMutex);auto& c=records[raw->header.hDevice];c.x+=raw->data.mouse.lLastX;c.y+=raw->data.mouse.lLastY;++c.events;}
  }
 }
 return DefWindowProcW(w,m,a,b);
}
double elapsed(Clock::time_point t) {return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
int wmain(int argc,wchar_t** argv) {
 if(argc!=3)return 2;
 wchar_t ip[128]{},port[128]{},uuid[128]{};
 GetPrivateProfileStringW(L"CAT",L"IP",L"",ip,128,argv[1]);GetPrivateProfileStringW(L"CAT",L"Port",L"",port,128,argv[1]);GetPrivateProfileStringW(L"CAT",L"UUID",L"",uuid,128,argv[1]);
 auto ascii=[](const wchar_t* s){std::string r;while(*s)r.push_back(static_cast<char>(*s++));return r;};
 cat::Device device;auto result=device.connect(ascii(ip),_wtoi(port),ascii(uuid),1000);
 if(result!=cat::ErrorCode::SUCCESS){printf("connect=%d\n",(int)result);return 3;}
 WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"CatRawInputBenchmark";RegisterClassW(&wc);
 HWND window=CreateWindowW(wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,wc.hInstance,nullptr);
 RAWINPUTDEVICE input{1,2,RIDEV_INPUTSINK,window};if(!RegisterRawInputDevices(&input,1,sizeof(input)))return 4;
 FILE* out=nullptr;_wfopen_s(&out,argv[2],L"wb");if(!out)return 5;
 const DWORD mainThread=GetCurrentThreadId();std::atomic<int> status{0};
 std::thread sender([&]{
  Sleep(200);{std::lock_guard<std::mutex> lock(recordsMutex);records.clear();}
  for(int i=0;i<20;++i){device.mouseMove(1,0);Sleep(5);}Sleep(200);
  HANDLE target=nullptr;bool found=false;
  {std::lock_guard<std::mutex> lock(recordsMutex);for(auto& [handle,c]:records){printf("probe device=%p dx=%lld dy=%lld events=%u\n",handle,c.x,c.y,c.events);if(c.x==20&&c.y==0){target=handle;found=true;}}}
  if(!found){puts("No unambiguous CAT raw input device found; aborted.");status=6;PostThreadMessageW(mainThread,WM_QUIT,0,0);return;}
  wchar_t name[1024]{};UINT nameSize=1024;GetRawInputDeviceInfoW(target,RIDI_DEVICENAME,name,&nameSize);printf("Selected raw mouse: %ls\n",name);fflush(stdout);
  device.mouseMove(-20,0);Sleep(200);
  const char* header="round,extra_wait_ms,sent,ack_success,errors,total_ms,expected_dx,received_dx,received_dy,raw_events,pass\n";fputs(header,out);fputs(header,stdout);
  const double waits[]={2,1,.885,.5,.25,.1,0,0,0,0};int round=0;
  for(double wait:waits){
   const int direction=(round%2==0?1:-1);{std::lock_guard<std::mutex> lock(recordsMutex);records.clear();}
   int ok=0,errors=0,sent=0;auto start=Clock::now();
   for(int i=0;i<1000;++i){auto code=device.mouseMove(direction,0);++sent;if(code==cat::ErrorCode::SUCCESS)++ok;else ++errors;if(errors>=3)break;auto end=Clock::now();while(elapsed(end)<wait)YieldProcessor();}
   double total=elapsed(start);Sleep(250);Counts c;{std::lock_guard<std::mutex> lock(recordsMutex);c=records[target];}
   char line[512];snprintf(line,sizeof(line),"%d,%.3f,%d,%d,%d,%.3f,%d,%lld,%lld,%u,%s\n",++round,wait,sent,ok,errors,total,sent*direction,c.x,c.y,c.events,errors==0&&c.x==sent*direction&&c.y==0?"PASS":"FAIL");fputs(line,out);fflush(out);fputs(line,stdout);fflush(stdout);
   if(errors){status=7;break;}
  }
  PostThreadMessageW(mainThread,WM_QUIT,0,0);
 });
 MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}sender.join();fclose(out);DestroyWindow(window);return status;
}
