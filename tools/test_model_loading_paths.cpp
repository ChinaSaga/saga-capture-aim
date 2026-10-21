// Real model loaders, CPU only. No application, capture, driver or mouse IO.
#include "Inference.h"
#include "FileIO.h"
#include "ModelFiles.h"
#include <iostream>
#include <array>

static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
int wmain(int argc, wchar_t** argv) {
    std::filesystem::path cleanup;
    try {
        require(argc == 2, "Supply the Release directory with test models");
        const auto release = file_paths::io(argv[1]);
        const auto exeDirectory = file_paths::executableDirectory();
        const auto previousDirectory = std::filesystem::current_path();
        std::filesystem::current_path(std::filesystem::temp_directory_path());
        require(file_paths::executableDirectory()==exeDirectory, "EXE directory must not depend on the working directory");
        std::filesystem::current_path(previousDirectory);
        cleanup = file_paths::io(std::filesystem::temp_directory_path() /
            (L"saga-native-paths-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        require(std::filesystem::create_directory(cleanup), "Unique fixture creation failed");
        const auto directory = cleanup / L"桌面 软件 \U0001F600" / std::wstring(100,L'a') / std::wstring(100,L'b');
        require(std::filesystem::create_directories(directory), "Long Unicode directory creation failed");
        require(directory.native().size() > MAX_PATH, "Fixture must exceed MAX_PATH");
        const auto base = directory / L"中文模型";
        for (const auto extension : {L".param", L".bin", L".onnx"}) {
            const auto source = release / L"模型数据" / (std::wstring(L"三角洲416") + extension);
            std::filesystem::copy_file(source, base.wstring() + extension);
        }
        const auto config = directory / L"配置保存.js";
        require(file_io::write(config,"unicode path roundtrip"), "Unicode file write failed");
        require(file_io::exists(config) && file_io::read(config)=="unicode path roundtrip", "Unicode file read failed");
        const auto ini = directory / L"配置保存.ini";
        require(file_io::write(ini,"; legacy GBK\r\n[默认]\r\n设备=旧设备\r\n[CAT]\r\nPort=8888\r\n"),"Legacy INI fixture failed");
        char legacy[128]{};
        file_paths::profileString("默认","设备","",legacy,sizeof(legacy),ini.c_str());
        require(std::string(legacy)=="旧设备", "Legacy GBK INI read depends on the system ACP");
        require(file_paths::writeProfile("默认","设备","中文设备",ini.c_str()), "Unicode INI write failed");
        char value[128]{};
        file_paths::profileString("默认","设备","",value,sizeof(value),ini.c_str());
        require(std::string(value)=="中文设备", "Unicode INI roundtrip failed");
        require(file_io::read(ini).starts_with("\xff\xfe") && file_paths::profileInt("CAT","Port",0,ini.c_str())==8888,
            "INI migration must preserve other sections and use a Unicode BOM");
        const auto freshIni = directory / L"新建.ini";
        require(file_paths::writeProfile("默认","设备","新设备",freshIni.c_str()), "New Unicode INI write failed");
        require(SetFileAttributesW(freshIni.c_str(),FILE_ATTRIBUTE_READONLY),"Read-only INI fixture failed");
        require(!file_paths::writeProfile("默认","设备","不应保存",freshIni.c_str()),"Read-only INI must not be overwritten");
        require(SetFileAttributesW(freshIni.c_str(),FILE_ATTRIBUTE_NORMAL),"Restore INI attributes failed");
        const auto malformed = directory / L"损坏.ini";
        require(file_io::write(malformed,std::string("\x81",1)),"Malformed INI fixture failed");
        file_paths::profileString("默认","设备","fallback",value,sizeof(value),malformed.c_str());
        require(std::string(value)=="fallback" && !file_paths::writeProfile("默认","设备","x",malformed.c_str()) &&
            file_io::read(malformed)==std::string("\x81",1),"Malformed INI must use defaults and preserve original bytes");
        require(model_files::catalog(directory).size()==1,"Unicode/long-path model discovery failed");
        require(saga::ncnn_create((base.wstring()+L".param").c_str(),(base.wstring()+L".bin").c_str(),0,0)==0,
            std::string("NCNN Unicode model load failed: ")+saga::ncnn_last_error());
        std::vector<unsigned char> pixels(320*320*3,114);
        DetectObject detections[99]{};
        require(ncnnDetectBgr(pixels.data(),320,320,320*3,.5f,.5f,detections)>=0,
            std::string("NCNN real CPU inference failed: ")+saga::ncnn_last_error());
        saga::ncnn_destroy();
        require(saga::onnx_create((base.wstring()+L".onnx").c_str(),L"",false)==0,
            std::string("ONNX Unicode model load failed: ")+saga::onnx_last_error());
        require(onnxDetectBgr(pixels.data(),320,320,320*3,.5f,.5f,detections)>=0,
            std::string("ONNX real CPU inference failed: ")+saga::onnx_last_error());
        saga::onnx_destroy();
        const auto hand = directory / L"人手模型.bin";
        std::filesystem::copy_file(release/L"人手数据"/L"mouse.bin",hand);
        require(saga::mc_create(hand.c_str())!=0,"Human model Unicode load failed");
        saga::mc_destroy();
        // Empty data is rejected before NVIDIA runtime loading; this proves the
        // TRT loader opened the Unicode path instead of reporting 'cannot open'.
        const auto trt = directory / L"中文模型.trt";
        require(file_io::write(trt,""),"TRT fixture write failed");
        require(saga::trt_create(trt.c_str())<0 && std::string(saga::trt_last_error()).find("Invalid TensorRT engine file size")!=std::string::npos,
            "TRT Unicode path did not reach content validation");
        require(!GetModuleHandleW(L"nvinfer_11.dll"),"Path fixture must not initialize NVIDIA runtime");
        std::filesystem::remove_all(cleanup);
        std::cout << "Unicode desktop/emoji/long paths passed: config IO, INI, discovery, real NCNN/ONNX/human models, TRT file access.\n";
    } catch (const std::exception& error) {
        if (!cleanup.empty()) { std::error_code ignored; std::filesystem::remove_all(cleanup,ignored); }
        std::cerr << error.what() << '\n'; return 1;
    }
}
