#define _WIN32_WINNT 0x0601
#include "../src/Makcu/cat/include/catdevice.h"
#include "../src/Makcu/cat/src/aes_functions.cpp"
#include "../src/Makcu/cat/src/cat_net_core.cpp"
#include "../src/Makcu/cat/src/catdevice.cpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <chrono>
#pragma comment(lib,"ws2_32.lib")
using Clock = std::chrono::steady_clock;
static double ms(Clock::duration d) { return std::chrono::duration<double,std::milli>(d).count(); }
int wmain(int argc, wchar_t** argv) {
 if(argc != 3) return 2;
 wchar_t ip[128]{},port[128]{},uuid[128]{};
 GetPrivateProfileStringW(L"CAT",L"IP",L"",ip,128,argv[1]);
 GetPrivateProfileStringW(L"CAT",L"Port",L"",port,128,argv[1]);
 GetPrivateProfileStringW(L"CAT",L"UUID",L"",uuid,128,argv[1]);
 auto ascii=[](const wchar_t* s) { std::string r; while (*s) r.push_back(static_cast<char>(*s++)); return r; };
 cat::Device device;
 auto connected=device.connect(ascii(ip),_wtoi(port),ascii(uuid),1000);
 std::printf("connect=%d\n",(int)connected); std::fflush(stdout);
 if(connected!=cat::ErrorCode::SUCCESS) return 3;
 int ok=0;
 auto start=Clock::now();
 for(int i=0;i<10;++i) if(device.mouseMove(10,0)==cat::ErrorCode::SUCCESS) ++ok;
 printf("Sent=10 ACK=%d expected_dx=100 extra_delay_ms=0 elapsed_ms=%.3f\n",ok,ms(Clock::now()-start));
 return ok==10?0:6;
}