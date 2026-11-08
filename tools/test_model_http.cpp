// CPU-only fixture for production routing. It never starts the application,
// driver setup, inference, mouse transport or permanent HTTP listeners.
#include "../src/Net.cpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "FixtureReadDeny.h"
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")

#include "FileIO.h"
AppState g;
std::atomic<long long> g_lastFrameTime{0};
long long nowMs() { return GetTickCount64(); }
void makcuState(int, bool& state) { state=false; }
std::string ansiToUtf8(const std::string& text) { return file_paths::narrow(file_paths::wide(text),CP_UTF8); }
std::string utf8ToAnsi(const std::string& text) { return file_paths::narrow(file_paths::wide(text,CP_UTF8)); }
bool fileExists(const std::filesystem::path& path) { return file_io::exists(path); }
std::string readFileAll(const std::filesystem::path& path) { return file_io::read(path); }
static unsigned fileWrites = 0;
bool writeFileAll(const std::filesystem::path& path,const std::string& data) { ++fileWrites; return file_io::write(path,data); }
namespace runtime_log { void write(const char*, ...) {} void writeWide(std::wstring_view) {} }
bool boxButtonState(int) { return false; }

// Production httpServerThread is deliberately unused and discarded with /Gy
// /OPT:REF. These stubs also ensure accidental fixture calls cannot start it.
WebListeners startWebListeners(unsigned short) { throw std::runtime_error("Forbidden permanent listener"); }
void closeWebListeners(WebListeners&) {}
void webAccessMonitor(WebListeners) { throw std::runtime_error("Forbidden app monitor"); }
void webAccessFailed(const std::wstring&) {}

static void require(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
static std::string request(const std::string& path,const std::string& body="",const std::string& raw="") {
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP),client=INVALID_SOCKET,server=INVALID_SOCKET;
    try {
        require(listener!=INVALID_SOCKET,"Listener creation failed");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Loopback bind failed");
        require(listen(listener,1)==0,"Loopback listen failed");int size=sizeof(address);
        require(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)==0,"Endpoint query failed");
        client=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(client!=INVALID_SOCKET,"Client socket failed");
        require(connect(client,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Loopback connection failed");
        server=accept(listener,nullptr,nullptr);require(server!=INVALID_SOCKET,"Loopback accept failed");
        DWORD timeout=3000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        const auto method=body.empty()?"GET":"POST";
        const auto message=raw.empty()?std::string(method)+" "+path+" HTTP/1.1\r\nHost: localhost\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body:raw;
        std::jthread worker([server] { clientThread(server); });
        server=INVALID_SOCKET;
        for(size_t offset=0;offset<message.size();) {
            const int sent=send(client,message.data()+offset,static_cast<int>((std::min)(size_t(17),message.size()-offset)),0);
            require(sent>0,"Request fragment send failed");offset+=sent;
        }
        shutdown(client,SD_SEND);
        std::string response;char bytes[4096];int count;
        while((count=recv(client,bytes,sizeof(bytes),0))>0)response.append(bytes,count);
        require(count==0,"Response read failed");closesocket(client);closesocket(listener);return response;
    } catch(...) {
        if(server!=INVALID_SOCKET)closesocket(server);
        if(client!=INVALID_SOCKET)closesocket(client);
        if(listener!=INVALID_SOCKET)closesocket(listener);
        throw;
    }
}
static std::string withDirectory(std::string body, const std::filesystem::path& path) {
    body.pop_back();
    return body + ",\"directory\":\"" + model_files::escapeJson(model_files::utf8(file_paths::io(path).wstring())) + "\"}";
}
int main() {
    std::filesystem::path directory;
    std::filesystem::path cleanupDirectory;
    WSADATA winsock{};
    try {
        require(WSAStartup(MAKEWORD(2,2),&winsock)==0,"Winsock startup failed");
        directory=std::filesystem::temp_directory_path()/(L"saga-model-http-中文-\U0001F600-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(directory),"Unique fixture directory failed");
        cleanupDirectory = file_paths::io(directory);
        directory = file_paths::io(directory / std::wstring(100,L'a') / std::wstring(100,L'b') / L"桌面软件");
        require(std::filesystem::create_directories(directory),"Long Unicode fixture directory failed");
        g.runDir=directory;g.cfgPath=directory/L"fixture.js";
        const std::string page="<!doctype html><title>fixture</title>";
        writeFileAll(g.runDir/L"圣人视觉识别系统.html",page);
        // Models in the EXE root must neither appear nor enable an engine.
        for(const auto name:{L"根目录模型.onnx",L"根目录模型.trt",L"任意模型.param",L"任意模型.bin"})
            std::ofstream(directory/name).put('x');
        require(request("/model-catalog.json").find("HTTP/1.1 500 ")==0,"Missing model directory must return a diagnostic");
        const auto modelDirectory = model_paths::directory(directory);
        require(g.modelDirectory()==file_paths::io(modelDirectory),"Runtime model directory differs from scanner directory");
        require(writeFileAll(modelDirectory,"not a directory"),"Cannot create obstruction fixture");
        require(request("/model-catalog.json").find("HTTP/1.1 500 ")==0,"File in place of model folder must report an error");
        std::filesystem::remove(modelDirectory);
        require(std::filesystem::create_directory(modelDirectory),"Cannot create dedicated model fixture folder");
        {
            FixtureReadDeny denied(modelDirectory);
            const auto response = request("/model-catalog.json");
            require(response.find("HTTP/1.1 500 ")==0 && response.find("\"code\":5")!=std::string::npos,
                "Access denied must report Windows error 5 instead of an empty catalog");
            require(response.find(model_files::escapeJson(model_files::utf8(modelDirectory.wstring())))!=std::string::npos,
                "Unreadable catalog must identify the exact Unicode directory");
            denied.restore();
        }
        for(const auto name:{L"任意模型.onnx",L"其他模型.TRT",L"配对.PARAM",L"配对.BIN",L"缺bin.param"})
            std::ofstream(modelDirectory/name).put('x');
        for(int engine=1;engine<=3;++engine) {
            const auto response=request("/models.json?engine="+std::to_string(engine));
            require(response.find("HTTP/1.1 200 ")==0,"Model list HTTP status failed");
            require(response.find("application/json; charset=utf-8")!=std::string::npos,"Model response content type failed");
            require(response.substr(response.find("\r\n\r\n")+4)==withDirectory(model_files::json(modelDirectory,engine),modelDirectory),"Model HTTP UTF-8 body differs");
        }
        auto catalogResponse=request("/model-catalog.json");
        require(catalogResponse.find("HTTP/1.1 200 ")==0 &&
            catalogResponse.substr(catalogResponse.find("\r\n\r\n")+4)==withDirectory(model_files::catalogJson(modelDirectory),modelDirectory), "Catalog HTTP status/UTF-8 body differs");
        require(catalogResponse.find(model_files::utf8(L"缺bin"))==std::string::npos,"Incomplete NCNN model leaked into catalog");
        require(catalogResponse.find(model_files::utf8(L"根目录模型"))==std::string::npos,"EXE root models leaked into model data catalog");
        require(catalogResponse.find("{\"name\":\""+model_files::utf8(L"任意模型")+"\",\"engines\":[2]}")!=std::string::npos,"Root NCNN files must not pair with ONNX in model data folder");
        for(const auto name:{L"同时存在.param",L"同时存在.bin",L"同时存在.onnx",L"同时存在.trt"}) std::ofstream(modelDirectory/name).put('x');
        const auto allFormats="{\"name\":\""+model_files::utf8(L"同时存在")+"\",\"engines\":[1,2,3]}";
        require(request("/model-catalog.json").find(allFormats)!=std::string::npos,"Same model must expose all three engines through HTTP");
        std::filesystem::remove(modelDirectory/L"同时存在.bin");
        const auto withoutNcnn="{\"name\":\""+model_files::utf8(L"同时存在")+"\",\"engines\":[2,3]}";
        require(request("/model-catalog.json").find(withoutNcnn)!=std::string::npos,"Catalog HTTP must reflect deleted model files");
        std::filesystem::create_directory(modelDirectory/L"no640");
        for(const auto name:{L"320.param",L"320.bin",L"320.trt",L"320.onnx",L"416.onnx",L"512.onnx"})
            std::ofstream(modelDirectory/L"no640"/name).put('x');
        const auto cappedGroup=request("/model-catalog.json");
        require(cappedGroup.find("\"name\":\"no640\",\"engines\":[1,2,3]")!=std::string::npos &&
            cappedGroup.find("\"sizes\":{\"1\":[320],\"2\":[320,416,512],\"3\":[320]}")!=std::string::npos,
            "HTTP catalog must enable single-stage and sub-640 groups independently per engine");
        for(const auto path:{"/models.json","/models.json?engine=4","/models.json?engine=3x","/models.json?engine=1&engine=3"})
            require(request(path).find("HTTP/1.1 400 ")==0,"Invalid model query did not return 400");
        require(request("/?ignored=1").find(page)!=std::string::npos,"Existing root page route broken");
        require(request("/index.html").find(page)!=std::string::npos,"Existing index page route broken");
        g_webConfig["推理引擎"]="3";
        require(request("/config.js?ignored=1").find("application/javascript; charset=utf-8")!=std::string::npos,"Existing config script route broken");
        require(request("/api").find("HTTP/1.1 201 ")==0,"Existing API read route broken");
        require(request("/api",ansiToUtf8("推理引擎|3")).find("HTTP/1.1 201 ")==0 && g.engine==3,"Engine 3 POST/config publication broken");
        // Migrate a legacy file and round-trip all profile fields through real HTTP/config IO.
        writeFileAll(g.cfgPath, ansiToUtf8("优先锁定=1\n其次锁定=2\n再次锁定=3\n优先锁定位置=55\n其次锁定位置=25\n再次锁定位置=35\n锁定范围=320\n中心点范围=40\n框内移动=true\n吸附常驻=false\n自瞄主键=2\n"));
        configInit(); configSyncAll(true);
        require(g.rangePressKey == 0, "Legacy config must disable range repeat");
        request("/api",ansiToUtf8("测距按下|1"));
        require(g.rangePressKey == 0, "Range repeat rejects mouse keys");
        request("/api",ansiToUtf8("测距按下|1057"));
        require(g.rangePressKey == 1057, "Range repeat accepts CAT space");
        configInit(); configSyncAll(false);
        require(g.rangePressKey == 1057, "Range repeat survives config reload");
        auto profiles = g.aimProfiles.config().config;
        require(profiles.profiles[0].keys[0]==2 && profiles.profiles[0].prioLockPos==55, "Legacy profile 1 not preserved");
        for(int group=1;group<3;++group) {
            require(profiles.profiles[group].keys==std::array<int,4>{}, "New profiles must start unbound");
            require(profiles.profiles[group].prioLockPos==55 && profiles.profiles[group].lockRange==320 && profiles.profiles[group].moveInBox,
                "New target pages must inherit legacy target defaults");
        }
        const char* names[]={"自瞄主键","自瞄副键","自瞄副键2","自瞄副键3","优先锁定","其次锁定","再次锁定",
            "优先锁定位置","其次锁定位置","再次锁定位置","锁定范围","中心点范围"};
        const int values[]={1,2,5,6,3,2,1,70,40,20,240,30};
        for(int group=1;group<=3;++group) {
            const auto prefix=group==1?std::string():std::string("热键")+std::to_string(group)+"_";
            require(request("/api",ansiToUtf8(prefix+"X轴补偿幅度|"+std::to_string(group*10))).starts_with("HTTP/1.1 201 "), "X compensation POST failed");
            require(request("/api",ansiToUtf8(prefix+"Y轴补偿幅度|"+std::to_string(group*20))).starts_with("HTTP/1.1 201 "), "Y compensation POST failed");
            require(request("/api",ansiToUtf8(prefix+"移动系数|0."+std::to_string(group))).starts_with("HTTP/1.1 201 "), "Coefficient POST failed");
            for(int field=0;field<12;++field)
                require(request("/api",ansiToUtf8(prefix+names[field]+"|"+std::to_string(values[field]+group))).find("HTTP/1.1 201 ")==0,
                    "Profile field POST failed");
            request("/api",ansiToUtf8(prefix+"框内移动|"+(group==2?"false":"true")));
        }
        profiles=g.aimProfiles.config().config;
        for(int group=0;group<3;++group) {
            const auto& p=profiles.profiles[group];const int offset=group+1;
            require(p.speedCompensationX==offset*10 && p.speedCompensationY==offset*20, "Compensation leaked between axes/groups");
            require(std::abs(p.moveFactor-offset*.1f)<.0001f,
                "Movement and prediction settings leaked between profiles");
            require(p.keys==std::array<int,4>{1+offset,2+offset,5+offset,6+offset}, "Four key bindings leaked between groups");
            require(p.prioLock==3+offset && p.secondLock==2+offset && p.thirdLock==1+offset &&
                p.prioLockPos==70+offset && p.secondLockPos==40+offset && p.thirdLockPos==20+offset &&
                p.lockRange==240+offset && p.centerRange==30+offset && p.moveInBox==(group!=1), "Target settings leaked between groups");
        }
        configInit(); configSyncAll(false);
        require(g.aimProfiles.config().config==profiles, "Saved profiles did not survive reload");
        require(request("/config.js").find(ansiToUtf8("\"热键3_优先锁定\":6"))!=std::string::npos, "Browser config omitted group 3");
        request("/api",ansiToUtf8("保存配置|三组测试"));
        request("/api",ansiToUtf8("热键2_优先锁定|9"));
        request("/api",ansiToUtf8("导入配置|三组测试"));
        require(g.aimProfiles.config().config==profiles, "Named configuration import must restore all groups");
        const auto writesBeforeRepeat = fileWrites;
        const auto revisionBeforeRepeat = g.aimProfiles.config().revision;
        for (int i = 0; i < 30; ++i)
            require(request("/api",ansiToUtf8("测距按下|1057")).starts_with("HTTP/1.1 201 "), "Unchanged value rejected");
        require(fileWrites == writesBeforeRepeat && g.aimProfiles.config().revision == revisionBeforeRepeat,
            "Repeated values must not write the file or disturb the active profile");
        std::filesystem::remove(g.cfgPath);
        require(request("/api",ansiToUtf8("测距按下|1057")).starts_with("HTTP/1.1 201 ") && fileExists(g.cfgPath),
            "An unchanged request must restore a missing configuration file");
        require(SetFileAttributesW(g.cfgPath.c_str(),FILE_ATTRIBUTE_READONLY),"Cannot make fixture config read-only");
        require(request("/api",ansiToUtf8("测距按下|1058")).find("HTTP/1.1 500 ")==0,
            "Failed config write must not claim success");
        require(SetFileAttributesW(g.cfgPath.c_str(),FILE_ATTRIBUTE_NORMAL),"Cannot restore fixture config attributes");
        require(request("/api",ansiToUtf8("测距按下|1058")).starts_with("HTTP/1.1 201 ") &&
            readFileAll(g.cfgPath).find(ansiToUtf8("测距按下=1058")) != std::string::npos,
            "Repeating a failed write must retry persistence");
        require(request("/unknown").find("HTTP/1.1 404 ")==0,"Unknown route did not return 404");
        const auto originalConfig=readFileAll(g.cfgPath);
        require(request("/api","","POST /api HTTP/1.1\r\nContent-Length: 20\r\n\r\nx|1").starts_with("HTTP/1.1 400 "),"Truncated body was accepted");
        require(request("/api","","POST /api HTTP/1.1\r\nCoNtEnT-LeNgTh: 999999\r\n\r\n").starts_with("HTTP/1.1 413 "),"Oversized mixed-case content length bypassed limit");
        require(readFileAll(g.cfgPath)==originalConfig,"Invalid request mutated configuration");
        require(request("/api",ansiToUtf8("模型名称|../escape")).starts_with("HTTP/1.1 400 "),"Model path traversal was accepted");
        require(request("/api",ansiToUtf8("移动系数|nan")).starts_with("HTTP/1.1 201 ") && g.settings.snapshot().moveFactor==0,"Nonfinite input reached worker settings");
        writeFileAll(g.cfgPath,std::string("\xef\xbb\xbf")+ansiToUtf8("移动系数=0.42\n移动方式=2\nKp=0.1\nKi=0.01\n"));
        configInit();configSyncAll(true);
        for(const auto& p:g.aimProfiles.config().config.profiles)
            require(p.moveFactor==.42f,
                "Legacy PID / BOM migration lost coefficient or prediction defaults");
        const auto migrated=request("/config.js");
        require(migrated.find("Kp")==std::string::npos && migrated.find("Ki")==std::string::npos &&
            migrated.find(ansiToUtf8("移动方式"))==std::string::npos, "Removed PID controls were republished");
        require(readFileAll(g.cfgPath).find("Kp=")==std::string::npos, "Removed PID values persisted");
        require(request("/api",ansiToUtf8("移动方式|2")).starts_with("HTTP/1.1 400 "), "Old browser re-enabled a removed mode");
        request("/api",ansiToUtf8("热键2_移动系数|-1"));
        require(g.aimProfiles.config().config.profiles[1].moveFactor==0,"Movement bounds failed");
        require(g.aimProfiles.config().config.profiles[0].speedCompensationX==0 &&
            g.aimProfiles.config().config.profiles[0].speedCompensationY==0,"Legacy compensation must default off");
        for(const auto axis:{"X轴补偿幅度","Y轴补偿幅度"}) {
            request("/api",ansiToUtf8(std::string(axis)+"|9999"));
            require(hashGet(axis)=="100","Compensation upper bound failed");
            for(const auto value:{"-1","nan","inf","2.5"}) {
                request("/api",ansiToUtf8(std::string(axis)+"|"+value));
                require(hashGet(axis)=="0","Invalid compensation accepted");
            }
        }
        writeFileAll(g.cfgPath,ansiToUtf8("移动系数=0.42\n预测连续次数=99\n预测提前量=45\nX轴预测提前量=50\nY轴预测提前量=60\n热键2_X轴预测提前量=30\n热键3_Y轴预测提前量=80\n卡尔曼预测=true\n卡尔曼预测时间=50\n热键2_卡尔曼响应强度=80\n热键3_卡尔曼最大提前量=100\n"));
        configInit();configSyncAll(true);
        for(const auto prefix:{"","热键2_","热键3_"})
            for(const auto name:{"预测连续次数","预测提前量","X轴预测提前量","Y轴预测提前量",
                "卡尔曼预测","卡尔曼预测时间","卡尔曼响应强度","卡尔曼平滑强度","卡尔曼最大提前量"}) {
                const std::string key=std::string(prefix)+name;
                require(!g_webConfig.contains(key),"Retired setting republished");
                require(request("/api",ansiToUtf8(key+"|50")).starts_with("HTTP/1.1 400 "),"Retired prediction accepted");
            }
        require(readFileAll(g.cfgPath).find(ansiToUtf8("卡尔曼"))==std::string::npos &&
            readFileAll(g.cfgPath).find(ansiToUtf8("X轴预测提前量="))==std::string::npos &&
            readFileAll(g.cfgPath).find(ansiToUtf8("预测连续次数="))==std::string::npos,"Retired prediction persisted");
        // Named legacy presets migrate each group separately. Explicit axis
        // values (including zero) take precedence over the old shared value.
        const auto legacyPreset = g.runDir/L"配置保存"/L"旧幅度.js";
        writeFileAll(legacyPreset,ansiToUtf8("移动速度补偿=35\nX轴补偿幅度=0\n热键2_移动速度补偿=65\n热键2_Y轴补偿幅度=80\n热键3_移动速度补偿=20\n"));
        require(request("/api",ansiToUtf8("导入配置|旧幅度")).starts_with("HTTP/1.1 201 "),"Legacy preset import failed");
        const auto migratedAxes=g.aimProfiles.config().config;
        require(migratedAxes.profiles[0].speedCompensationX==0 && migratedAxes.profiles[0].speedCompensationY==35 &&
            migratedAxes.profiles[1].speedCompensationX==65 && migratedAxes.profiles[1].speedCompensationY==80 &&
            migratedAxes.profiles[2].speedCompensationX==20 && migratedAxes.profiles[2].speedCompensationY==20,
            "Per-group migration overwrote explicit axes or mixed groups");
        require(readFileAll(g.cfgPath).find(ansiToUtf8("移动速度补偿="))==std::string::npos,"Shared field persisted after migration");
        for(const auto prefix:{"","热键2_","热键3_"})
            require(request("/api",ansiToUtf8(std::string(prefix)+"移动速度补偿|99")).starts_with("HTTP/1.1 400 "),
                "Stale page must not overwrite both axes");
        configInit();configSyncAll(false);
        require(g.aimProfiles.config().config==migratedAxes,"Axis migration did not survive reload");
        const std::string gbkTrailBackslash("\x81\x5c",2);
        require(jsonEscape(gbkTrailBackslash)==gbkTrailBackslash,"GBK trail byte was mistaken for a JSON escape");
        require(request("/api",ansiToUtf8("不存在的参数|1")).starts_with("HTTP/1.1 400 "),"Unknown keys were accepted");
        g.adaptiveCapture=true; g.imgSize=192;
        const auto live=request("/api");
        require(live.find(ansiToUtf8("\"自适应截图\":true"))!=std::string::npos &&
            live.find(ansiToUtf8("\"截图范围\":192"))!=std::string::npos,"Live crop state missing from existing poll response");
        for (auto state : {model_status::State::Idle, model_status::State::Loading,
            model_status::State::Ready, model_status::State::Failed}) {
            g.modelStatus = state;
            const auto expected = model_files::utf8(L"\"模型加载状态\":\"") +
                model_files::utf8(model_status::label(state)) + "\"";
            require(request("/api").find(expected)!=std::string::npos,
                "Model status must reach the browser as valid UTF-8");
        }
        const std::string largePage = page + std::string(512 * 1024, ' ');
        writeFileAll(g.runDir/L"圣人视觉识别系统.html", largePage);
        const auto largeResponse = request("/");
        const auto bodyOffset = largeResponse.find("\r\n\r\n");
        require(bodyOffset != std::string::npos && largeResponse.substr(bodyOffset + 4) == largePage,
            "Large scatter/gather HTTP response lost or duplicated payload bytes");
        std::filesystem::remove(directory/L"圣人视觉识别系统.html");
        require(request("/models.json?engine=3").find("HTTP/1.1 404 ")==0,"Missing panel protection broken");
        require(request("/model-catalog.json").find("HTTP/1.1 404 ")==0,"Catalog missing-panel protection broken");
        std::filesystem::remove_all(cleanupDirectory);WSACleanup();
        std::cout<<"Production HTTP socket routing passed: engines 1/2/3, UTF-8, invalid queries, root/index/config/API, engine 3 POST, 404 and missing panel protection.\n";
    } catch(const std::exception& error) {
        if(!cleanupDirectory.empty()){
            if(!g.cfgPath.empty()) SetFileAttributesW(g.cfgPath.c_str(),FILE_ATTRIBUTE_NORMAL);
            std::error_code ignored;std::filesystem::remove_all(cleanupDirectory,ignored);
        }
        WSACleanup();std::cerr<<error.what()<<'\n';return 1;
    }
}
