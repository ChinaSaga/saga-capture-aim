// CPU-only fixture for production routing. It never starts the application,
// driver setup, inference, mouse transport or permanent HTTP listeners.
#include "../src/Net.cpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#pragma comment(lib, "ws2_32.lib")

AppState g;
long long g_lastFrameTime=0;
long long nowMs() { return GetTickCount64(); }
void makcuState(int, bool& state) { state=false; }
namespace runtime_log { void write(const char*, ...) {} }
static std::string encode(const std::wstring& text, unsigned codepage) {
    const int count=WideCharToMultiByte(codepage,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,'\0');
    WideCharToMultiByte(codepage,0,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr);
    return result;
}
std::string ansiToUtf8(const std::string& text) { return model_files::utf8(model_files::fromGbk(text)); }
std::string utf8ToAnsi(const std::string& text) {
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
    std::wstring result(count,L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);
    return encode(result,936);
}
bool fileExists(const std::string& name) { return std::filesystem::is_regular_file(model_files::fromGbk(name)); }
std::string readFileAll(const std::string& name) {
    std::ifstream stream(std::filesystem::path(model_files::fromGbk(name)),std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),{});
}
bool writeFileAll(const std::string& name,const std::string& content) {
    std::ofstream stream(std::filesystem::path(model_files::fromGbk(name)),std::ios::binary);
    stream.write(content.data(),static_cast<std::streamsize>(content.size()));return bool(stream);
}
// Production httpServerThread is deliberately unused and discarded with /Gy
// /OPT:REF. These stubs also ensure accidental fixture calls cannot start it.
WebListeners startWebListeners(unsigned short) { throw std::runtime_error("Forbidden permanent listener"); }
void closeWebListeners(WebListeners&) {}
void webAccessMonitor(WebListeners) { throw std::runtime_error("Forbidden app monitor"); }
void webAccessFailed(const std::wstring&) {}

static void require(bool ok,const char* error) { if(!ok)throw std::runtime_error(error); }
static std::string request(const std::string& path,const std::string& body="") {
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
        const auto message=std::string(method)+" "+path+" HTTP/1.1\r\nHost: localhost\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
        handleRequest(server,message);
        closesocket(server);server=INVALID_SOCKET;
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
int main() {
    std::filesystem::path directory;
    WSADATA winsock{};
    try {
        require(WSAStartup(MAKEWORD(2,2),&winsock)==0,"Winsock startup failed");
        directory=std::filesystem::temp_directory_path()/(L"saga-model-http-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(directory),"Unique fixture directory failed");
        g.runDir=encode(directory.wstring(),936);g.cfgPath=g.runDir+"\\fixture.js";
        const std::string page="<!doctype html><title>fixture</title>";
        writeFileAll(g.runDir+"\\圣人视觉识别系统.html",page);
        // Models in the EXE root must neither appear nor enable an engine.
        for(const auto name:{L"根目录模型.onnx",L"根目录模型.trt",L"任意模型.param",L"任意模型.bin"})
            std::ofstream(directory/name).put('x');
        require(request("/model-catalog.json").find("{\"models\":[]}")!=std::string::npos,"Missing model data folder must not fall back to EXE root");
        const auto modelDirectory = model_paths::directory(directory);
        require(model_files::fromGbk(g.modelDirectory())==modelDirectory.wstring(),"Runtime model directory differs from scanner directory");
        require(std::filesystem::create_directory(modelDirectory),"Cannot create dedicated model fixture folder");
        for(const auto name:{L"任意模型.onnx",L"其他模型.TRT",L"配对.PARAM",L"配对.BIN",L"缺bin.param"})
            std::ofstream(modelDirectory/name).put('x');
        for(int engine=1;engine<=3;++engine) {
            const auto response=request("/models.json?engine="+std::to_string(engine));
            require(response.find("HTTP/1.1 200 ")==0,"Model list HTTP status failed");
            require(response.find("application/json; charset=utf-8")!=std::string::npos,"Model response content type failed");
            require(response.substr(response.find("\r\n\r\n")+4)==model_files::json(modelDirectory,engine),"Model HTTP UTF-8 body differs");
        }
        auto catalogResponse=request("/model-catalog.json");
        require(catalogResponse.find("HTTP/1.1 200 ")==0 &&
            catalogResponse.substr(catalogResponse.find("\r\n\r\n")+4)==model_files::catalogJson(modelDirectory), "Catalog HTTP status/UTF-8 body differs");
        require(catalogResponse.find(model_files::utf8(L"缺bin"))==std::string::npos,"Incomplete NCNN model leaked into catalog");
        require(catalogResponse.find(model_files::utf8(L"根目录模型"))==std::string::npos,"EXE root models leaked into model data catalog");
        require(catalogResponse.find("{\"name\":\""+model_files::utf8(L"任意模型")+"\",\"engines\":[2]}")!=std::string::npos,"Root NCNN files must not pair with ONNX in model data folder");
        for(const auto name:{L"同时存在.param",L"同时存在.bin",L"同时存在.onnx",L"同时存在.trt"}) std::ofstream(modelDirectory/name).put('x');
        const auto allFormats="{\"name\":\""+model_files::utf8(L"同时存在")+"\",\"engines\":[1,2,3]}";
        require(request("/model-catalog.json").find(allFormats)!=std::string::npos,"Same model must expose all three engines through HTTP");
        std::filesystem::remove(modelDirectory/L"同时存在.bin");
        const auto withoutNcnn="{\"name\":\""+model_files::utf8(L"同时存在")+"\",\"engines\":[2,3]}";
        require(request("/model-catalog.json").find(withoutNcnn)!=std::string::npos,"Catalog HTTP must reflect deleted model files");
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
            for(int field=0;field<12;++field)
                require(request("/api",ansiToUtf8(prefix+names[field]+"|"+std::to_string(values[field]+group))).find("HTTP/1.1 201 ")==0,
                    "Profile field POST failed");
            request("/api",ansiToUtf8(prefix+"框内移动|"+(group==2?"false":"true")));
        }
        profiles=g.aimProfiles.config().config;
        for(int group=0;group<3;++group) {
            const auto& p=profiles.profiles[group];const int offset=group+1;
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
        require(request("/unknown").find("HTTP/1.1 404 ")==0,"Unknown route did not return 404");
        std::filesystem::remove(directory/L"圣人视觉识别系统.html");
        require(request("/models.json?engine=3").find("HTTP/1.1 404 ")==0,"Missing panel protection broken");
        require(request("/model-catalog.json").find("HTTP/1.1 404 ")==0,"Catalog missing-panel protection broken");
        std::filesystem::remove_all(directory);WSACleanup();
        std::cout<<"Production HTTP socket routing passed: engines 1/2/3, UTF-8, invalid queries, root/index/config/API, engine 3 POST, 404 and missing panel protection.\n";
    } catch(const std::exception& error) {
        if(!directory.empty()){std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
        WSACleanup();std::cerr<<error.what()<<'\n';return 1;
    }
}
