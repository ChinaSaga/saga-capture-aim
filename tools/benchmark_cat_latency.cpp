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
 FILE* out=nullptr; _wfopen_s(&out,argv[2],L"wb"); if(!out)return 4;
 const char* header="round,extra_wait_ms,commands,ack_success,errors,total_ms,commands_per_second,ack_mean_ms,ack_p50_ms,ack_p95_ms,ack_p99_ms,ack_max_ms\n";
 std::fputs(header,out);std::fputs(header,stdout);
 for(int i=0;i<50;++i) if(device.mouseMove(0,0)!=cat::ErrorCode::SUCCESS) {std::fclose(out);return 5;}
 const double waits[]={2,1,.885,.5,.25,.1,0,0,0};
 int round=0;
 for(double wait:waits) {
  std::vector<double> timings;int success=0,errors=0;
  auto start=Clock::now();
  for(int i=0;i<1000;++i) {
   auto begin=Clock::now();
   auto result=device.mouseMove(i%2==0?1:-1,0);
   auto end=Clock::now();timings.push_back(ms(end-begin));
   if(result==cat::ErrorCode::SUCCESS) ++success; else ++errors;
   if(errors>=3) break;
   const auto deadline=end+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double,std::milli>(wait));
   while(Clock::now()<deadline) YieldProcessor();
  }
  double total=ms(Clock::now()-start),sum=0;for(double t:timings)sum+=t;
  std::sort(timings.begin(),timings.end());
  auto percentile=[&](double p){return timings[(size_t)((timings.size()-1)*p)];};
  char row[512]; std::snprintf(row,sizeof(row),"%d,%.3f,%zu,%d,%d,%.3f,%.2f,%.6f,%.6f,%.6f,%.6f,%.6f\n",++round,wait,timings.size(),success,errors,total,timings.size()*1000/total,sum/timings.size(),percentile(.5),percentile(.95),percentile(.99),timings.back());
  std::fputs(row,out);std::fflush(out);std::fputs(row,stdout);std::fflush(stdout);
  if(errors){std::fclose(out);return 6;}
  Sleep(250);
 }
 std::fclose(out);return 0;
}
