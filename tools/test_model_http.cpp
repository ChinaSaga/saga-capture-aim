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
        for(const auto name:{L"任意模型.onnx",L"其他模型.TRT",L"配对.PARAM",L"配对.BIN",L"缺bin.param"})
            std::ofstream(directory/name).put('x');
        for(int engine=1;engine<=3;++engine) {
            const auto response=request("/models.json?engine="+std::to_string(engine));
            require(response.find("HTTP/1.1 200 ")==0,"Model list HTTP status failed");
            require(response.find("application/json; charset=utf-8")!=std::string::npos,"Model response content type failed");
            require(response.substr(response.find("\r\n\r\n")+4)==model_files::json(directory,engine),"Model HTTP UTF-8 body differs");
        }
        for(const auto path:{"/models.json","/models.json?engine=4","/models.json?engine=3x","/models.json?engine=1&engine=3"})
            require(request(path).find("HTTP/1.1 400 ")==0,"Invalid model query did not return 400");
        require(request("/?ignored=1").find(page)!=std::string::npos,"Existing root page route broken");
        require(request("/index.html").find(page)!=std::string::npos,"Existing index page route broken");
        g_webConfig["推理引擎"]="3";
        require(request("/config.js?ignored=1").find("application/javascript; charset=utf-8")!=std::string::npos,"Existing config script route broken");
        require(request("/api").find("HTTP/1.1 201 ")==0,"Existing API read route broken");
        require(request("/api",ansiToUtf8("推理引擎|3")).find("HTTP/1.1 201 ")==0 && g.engine==3,"Engine 3 POST/config publication broken");
        require(request("/unknown").find("HTTP/1.1 404 ")==0,"Unknown route did not return 404");
        std::filesystem::remove(directory/L"圣人视觉识别系统.html");
        require(request("/models.json?engine=3").find("HTTP/1.1 404 ")==0,"Missing panel protection broken");
        std::filesystem::remove_all(directory);WSACleanup();
        std::cout<<"Production HTTP socket routing passed: engines 1/2/3, UTF-8, invalid queries, root/index/config/API, engine 3 POST, 404 and missing panel protection.\n";
    } catch(const std::exception& error) {
        if(!directory.empty()){std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
        WSACleanup();std::cerr<<error.what()<<'\n';return 1;
    }
}
