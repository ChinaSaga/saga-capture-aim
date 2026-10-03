#include "ModelFiles.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    std::filesystem::path directory;
    try {
        directory = std::filesystem::temp_directory_path() /
            (L"saga-model-files-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(directory), "Unique temporary directory creation failed");
        const std::wstring gbkName=L"中文目录";
        char gbkBytes[64]{};
        const int gbkCount=WideCharToMultiByte(936,0,gbkName.data(),static_cast<int>(gbkName.size()),gbkBytes,sizeof(gbkBytes),nullptr,nullptr);
        require(gbkCount>0 && model_files::fromGbk(std::string(gbkBytes,gbkCount))==gbkName, "GBK runtime directory decoding mismatch");
        require(model_files::list(directory,3).empty(), "Empty directory must have no models");
        const auto file = [&](const std::wstring& name) { std::ofstream(directory/name).put('x'); };
        file(L"三角洲行动.onnx"); file(L"任意名字.TRT"); file(L"a.trt"); file(L"z.TrT");
        file(L"配对.PARAM"); file(L"配对.BIN"); file(L"缺权重.param"); file(L"孤立.bin");
        file(L"Mixed.Param"); file(L"mixed.Bin"); file(L"notmodel.txt");
        std::filesystem::create_directory(directory/L"目录伪装.trt");
        std::filesystem::create_directory(directory/L"nested");
        std::ofstream(directory/L"nested"/L"不递归.trt").put('x');
        const auto onnx = model_files::list(directory,2);
        require(onnx.size()==1 && onnx[0]==model_files::utf8(L"三角洲行动"), "Chinese ONNX basename or filtering mismatch");
        const auto trt = model_files::list(directory,3);
        require(trt.size()==3 && trt[0]=="a" && trt[1]=="z" && trt[2]==model_files::utf8(L"任意名字"), "TRT extension, Unicode, sorting or nonrecursive filtering mismatch");
        const auto ncnn = model_files::list(directory,1);
        require(ncnn.size()==2 && ncnn[0]=="Mixed" && ncnn[1]==model_files::utf8(L"配对"), "NCNN case-insensitive pairing mismatch");
        int engine=0;
        require(model_files::parseEngine("engine=3",engine) && engine==3, "Engine query 3 rejected");
        require(model_files::parseEngine("other=x&engine=2&unused=y",engine) && engine==2, "Additional query keys failed");
        for (const auto query : {"", "engine=0", "engine=4", "engine=3x", "engine=-1", "engine=", "engine=1&engine=3"})
            require(!model_files::parseEngine(query,engine), "Malformed query accepted");
        require(model_files::escapeJson("quote\"slash\\\n\t") == "quote\\\"slash\\\\\\u000a\\u0009", "JSON escaping mismatch");
        const auto response=model_files::json(directory,3);
        require(response=="{\"engine\":3,\"models\":[\"a\",\"z\",\""+model_files::utf8(L"任意名字")+"\"]}", "JSON UTF-8 result mismatch");
        std::filesystem::remove_all(directory);
        std::cout << "Model discovery passed: Unicode, arbitrary names, empty/missing files, case pairing, sorting, directory exclusion, nonrecursive scan, query validation and JSON escaping.\n";
    } catch(const std::exception& error) {
        // Only this process-created unique temporary directory is removed.
        if (!directory.empty()) { std::error_code ignored; std::filesystem::remove_all(directory,ignored); }
        std::cerr << error.what() << '\n'; return 1;
    }
}
