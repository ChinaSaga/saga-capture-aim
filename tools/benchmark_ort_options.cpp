#define NOMINMAX
// Isolated probe; no model argument only enumerates DXGI adapters.
// Usage: benchmark.exe model.onnx adapter iterations [selected]
// adapter=-1 means CPU. mode=0 allocates outputs, mode=1 preallocates CPU outputs,
// mode=2 binds CPU input once (intentionally demonstrates stale GPU upload),
// mode=3 rebinds input every Run. Persistent CPU IoBinding MUST NOT be used in
// production: changingInputMaxDiff detects its stale input despite faster timing.
// GPU timings require an exclusive slot and synchronous CPU output completion.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static double processCpuMs() {FILETIME a,b,k,u; GetProcessTimes(GetCurrentProcess(),&a,&b,&k,&u); ULARGE_INTEGER ki{},ui{};ki.LowPart=k.dwLowDateTime;ki.HighPart=k.dwHighDateTime;ui.LowPart=u.dwLowDateTime;ui.HighPart=u.dwHighDateTime;return double(ki.QuadPart+ui.QuadPart)/10000.;}
static DWORD threadCount() {HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 e{sizeof(e)};DWORD n=0;if(Thread32First(s,&e))do {if(e.th32OwnerProcessID==GetCurrentProcessId())++n;}while(Thread32Next(s,&e));CloseHandle(s);return n;}
static size_t count(const std::vector<int64_t>& shape) { size_t n=1; for(auto v:shape) {if(v<=0) throw std::runtime_error("Dynamic tensor unsupported in isolated probe"); n*=size_t(v);} return n; }
int wmain(int argc,wchar_t** argv) {
  ComPtr<IDXGIFactory1> factory; if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
  for(UINT i=0;;++i) {ComPtr<IDXGIAdapter1> a; if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND) break; DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); std::wcout<<L"adapter="<<i<<L" name="<<d.Description<<L" vendor="<<d.VendorId<<L" device="<<d.DeviceId<<L" vramMiB="<<(d.DedicatedVideoMemory>>20)<<L" software="<<bool(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)<<L"\n"; }
  if(argc<2) return 0;
  int adapter=argc>2?_wtoi(argv[2]):0; int iterations=argc>3?_wtoi(argv[3]):120;
  if (adapter < -1 || iterations < 1) { std::cerr << "Adapter must be >= -1 and iterations >= 1\n"; return 2; }
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING,"ort-options"); Ort::AllocatorWithDefaultOptions allocator;
  std::vector<float> reference;
  struct Config {int threads,spin,opt,capture,mode;};
  std::vector<Config> configs={{6,1,99,0,0},{1,1,99,0,0},{2,1,99,0,0},{4,1,99,0,0},{1,0,99,0,0},{2,0,99,0,0},{6,0,99,0,0},{1,1,2,0,0},{1,1,1,0,0},{1,1,0,0,0},{1,1,99,1,0},{1,1,99,0,1},{1,1,99,0,2},{6,1,99,0,1},{1,1,99,0,0},{6,1,99,0,0}};
  if(argc>4) { configs={{6,1,99,0,1},{1,0,99,0,1},{1,0,99,0,3},{6,1,99,0,3},{1,0,99,1,3},{1,0,99,0,2},{6,1,99,0,1}}; } if(argc>4 && std::wstring(argv[4])==L"abba") configs={{6,1,99,0,1},{1,0,99,0,1},{1,0,99,0,1},{6,1,99,0,1},{1,0,99,0,1},{6,1,99,0,1},{6,1,99,0,1},{1,0,99,0,1}}; for(auto c:configs) try {
    Ort::SessionOptions options; options.DisableMemPattern(); options.SetExecutionMode(ORT_SEQUENTIAL); options.SetIntraOpNumThreads(c.threads); options.SetGraphOptimizationLevel(static_cast<GraphOptimizationLevel>(c.opt)); options.AddConfigEntry("session.intra_op.allow_spinning",c.spin?"1":"0"); options.AddConfigEntry("session.inter_op.allow_spinning",c.spin?"1":"0"); if(c.capture) options.AddConfigEntry("ep.dml.enable_graph_capture","1"); if(adapter>=0) Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options,adapter));
    auto begin=std::chrono::steady_clock::now(); Ort::Session session(env,argv[1],options); auto loadMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    auto inName=session.GetInputNameAllocated(0,allocator); const char* inNames[]={inName.get()}; auto shape=session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape(); std::wcout<<L"input shape="; for(auto d:shape) std::wcout<<d<<L","; std::wcout<<L"\n"; std::vector<float> input(count(shape)); for(size_t i=0;i<input.size();++i) input[i]=float((i*37)%256)/255.f;
    auto memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault); auto in=Ort::Value::CreateTensor<float>(memory,input.data(),input.size(),shape.data(),shape.size());
    std::vector<Ort::AllocatedStringPtr> names; std::vector<const char*> outNames; for(size_t i=0;i<session.GetOutputCount();++i) names.push_back(session.GetOutputNameAllocated(i,allocator)); for(auto& n:names) outNames.push_back(n.get());
    std::vector<Ort::Value> outputs; std::vector<std::vector<float>> storage; for(size_t i=0;i<outNames.size();++i) {auto outputTypeInfo=session.GetOutputTypeInfo(i); auto type=outputTypeInfo.GetTensorTypeAndShapeInfo(); auto s=type.GetShape(); if(type.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) throw std::runtime_error("Nonfloat output"); storage.emplace_back(count(s)); outputs.push_back(Ort::Value::CreateTensor<float>(memory,storage.back().data(),storage.back().size(),s.data(),s.size())); if(reference.empty()) {std::wcout<<L"output="<<i<<L" shape="; for(auto d:s) std::wcout<<d<<L","; std::wcout<<L"\n";} }
    Ort::IoBinding binding(session); if(c.mode>=2) {binding.BindInput(inNames[0],in); for(size_t i=0;i<outNames.size();++i) binding.BindOutput(outNames[i],outputs[i]);}
    auto run=[&] {if(c.mode==0) outputs=session.Run(Ort::RunOptions{nullptr},inNames,&in,1,outNames.data(),outNames.size()); else if(c.mode==1) session.Run(Ort::RunOptions{nullptr},inNames,&in,1,outNames.data(),outputs.data(),outputs.size()); else {if(c.mode==3) binding.BindInput(inNames[0],in); session.Run(Ort::RunOptions{nullptr},binding); binding.SynchronizeOutputs();} };
    for(int i=0;i<40;++i) run(); std::vector<double> times;times.reserve(iterations); double cpuBefore=processCpuMs();auto wallBefore=std::chrono::steady_clock::now(); for(int i=0;i<iterations;++i) {begin=std::chrono::steady_clock::now(); run(); times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());}
    double cpuDelta=processCpuMs()-cpuBefore;double wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wallBefore).count();PROCESS_MEMORY_COUNTERS_EX pm{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),sizeof(pm));std::wcout<<L"cpuMs="<<cpuDelta<<L" wallMs="<<wall<<L" cpuCores="<<cpuDelta/wall<<L" processThreads="<<threadCount()<<L" workingSetMiB="<<double(pm.WorkingSetSize)/1048576.<<L" privateMiB="<<double(pm.PrivateUsage)/1048576.<<L"\n"; double changingDiff=0; for(int v=0;v<4;++v) {std::fill(input.begin(),input.end(),float(v)/4.f); run(); auto check=session.Run(Ort::RunOptions{nullptr},inNames,&in,1,outNames.data(),outNames.size()); for(size_t k=0;k<outputs.size();++k) {auto a=outputs[k].GetTensorData<float>(); auto b=check[k].GetTensorData<float>(); for(size_t j=0;j<outputs[k].GetTensorTypeAndShapeInfo().GetElementCount();++j) changingDiff=std::max(changingDiff,double(std::fabs(a[j]-b[j])));}} for(size_t i=0;i<input.size();++i) input[i]=float((i*37)%256)/255.f; run(); std::wcout<<L"changingInputMaxDiff="<<changingDiff<<L"\n"; std::vector<float> result; for(auto& o:outputs) {auto n=o.GetTensorTypeAndShapeInfo().GetElementCount(); auto p=o.GetTensorData<float>(); result.insert(result.end(),p,p+n);} if(reference.empty()) reference=result; double maxDiff=0; size_t nonfinite=0; if(result.size()!=reference.size()) throw std::runtime_error("Output size changed"); for(size_t i=0;i<result.size();++i) {if(!std::isfinite(result[i])) ++nonfinite; maxDiff=std::max(maxDiff,double(std::fabs(result[i]-reference[i])));}
    auto mean=std::accumulate(times.begin(),times.end(),0.0)/times.size(); std::sort(times.begin(),times.end());
    std::wcout<<L"threads="<<c.threads<<L" spin="<<c.spin<<L" opt="<<c.opt<<L" capture="<<c.capture<<L" mode="<<c.mode<<L" loadMs="<<loadMs<<L" meanMs="<<mean<<L" p50Ms="<<times[times.size()/2]<<L" p95Ms="<<times[size_t(times.size()*0.95)]<<L" maxAbsDiff="<<maxDiff<<L" nonfinite="<<nonfinite<<std::endl;
  } catch(const std::exception& e) {std::cerr<<"FAILED threads="<<c.threads<<" spin="<<c.spin<<" opt="<<c.opt<<" capture="<<c.capture<<" mode="<<c.mode<<" error="<<e.what()<<std::endl;}
}
