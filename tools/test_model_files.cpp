#include "ModelFiles.h"
#include "ModelPaths.h"
#include "AdaptiveCapture.h"
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
        require(std::filesystem::path(model_files::fromGbk(model_paths::gbkDirectory(std::string(gbkBytes,gbkCount)))) ==
            model_paths::directory(std::filesystem::path(gbkName)), "GBK application and Unicode converter model directories differ");
        require(model_paths::defaultTrtOutput(directory, L"D:/external/任意模型.onnx") ==
            model_paths::directory(directory)/L"任意模型.trt", "Converter must save model beside the EXE in the model data folder");
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
        // Every combination of the four file types, including incomplete NCNN
        // pairs. Different models must never enable each other's buttons.
        for (unsigned mask = 0; mask < 16; ++mask) {
            const auto name = L"combination-" + std::to_wstring(mask);
            const wchar_t* suffixes[] = {L".param", L".bin", L".onnx", L".trt"};
            for (unsigned bit = 0; bit < 4; ++bit) if (mask & (1u << bit)) file(name + suffixes[bit]);
        }
        file(L"CaseUnified.PARAM"); file(L"caseunified.bin"); file(L"CASEUNIFIED.ONNX"); file(L"Caseunified.TrT");
        const auto models = model_files::catalog(directory);
        for (unsigned mask = 0; mask < 16; ++mask) {
            const auto name = "combination-" + std::to_string(mask);
            const auto found = std::find_if(models.begin(), models.end(), [&](const auto& model) { return model.name == name; });
            const bool ncnn = (mask & 3) == 3, onnx = (mask & 4) != 0, trt = (mask & 8) != 0;
            require((found != models.end()) == (ncnn || onnx || trt), "Catalog includes an incomplete model or omits a valid model");
            if (found != models.end()) require(!found->engineNames[0].empty() == ncnn &&
                !found->engineNames[1].empty() == onnx && !found->engineNames[2].empty() == trt, "Catalog engine availability differs from files");
        }
        const auto unified = std::find_if(models.begin(), models.end(), [](const auto& model) { return model.name == "CaseUnified"; });
        require(unified != models.end() && unified->engineNames == std::array<std::string,3>{"CaseUnified","CASEUNIFIED","Caseunified"}, "Cross-format case-insensitive merge failed");
        require(model_files::catalogJson(directory).find("{\"name\":\"CaseUnified\",\"engines\":[1,2,3]}") != std::string::npos, "Catalog JSON engine flags mismatch");
        std::filesystem::remove(directory/L"caseunified.bin"); std::filesystem::remove(directory/L"Caseunified.TrT");
        require(model_files::catalogJson(directory).find("{\"name\":\"CASEUNIFIED\",\"engines\":[2]}") != std::string::npos, "Deleted NCNN weight/TRT file did not disable engines");
        std::filesystem::create_directory(directory/L"模型组");
        for (const auto name : {L"320.onnx", L"416.onnx", L"640.onnx", L"640.trt", L"320.bin", L"416.param", L"640.param", L"640.bin"})
            std::ofstream(directory/L"模型组"/name).put('x');
        file(L"模型组.onnx");
        auto groups = model_files::catalog(directory);
        const auto groupName = model_files::utf8(L"模型组");
        require(std::count_if(groups.begin(), groups.end(), [&](const auto& m) { return m.name == groupName; }) == 1, "Duplicate folder/legacy name");
        const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& m) { return m.name == groupName; });
        require(group != groups.end() && group->group && group->sizes[0] == std::vector<int>{640} &&
            group->sizes[1] == std::vector<int>({320,416,640}) && group->sizes[2] == std::vector<int>{640}, "Group sizing or NCNN pairing failed");
        require(model_files::isGroup(directory,L"模型组") && !model_files::isGroup(directory,L"../escape"), "Group path validation failed");
        std::filesystem::remove(directory/L"模型组"/L"640.onnx");
        groups = model_files::catalog(directory);
        const auto incomplete = std::find_if(groups.begin(), groups.end(), [&](const auto& m) { return m.name == groupName; });
        require(incomplete->engineNames[1] == groupName && incomplete->sizes[1] == std::vector<int>({320,416}),
            "A group without 640 must remain available");
        std::filesystem::remove(directory/L"模型组"/L"416.onnx");
        require(model_files::groupSizes(directory/L"模型组",2)==std::vector<int>{320}, "Single-size group must be usable");
        require(model_files::catalogJson(directory).find("\"2\":[320]")!=std::string::npos, "Single-size group missing from catalog");
        using namespace adaptive_capture;
        require(desiredSize(false,true,0,0)==640 && desiredSize(true,false,0,0)==640, "Idle/lost target must search at 640");
        require(desiredSize(true,true,79.99,0)==160 && desiredSize(true,true,80,0)==192 &&
            desiredSize(true,true,159.99,0)==320 && desiredSize(true,true,160,0)==352 &&
            desiredSize(true,true,199.99,0)==416 && desiredSize(true,true,200,0)==416 &&
            desiredSize(true,true,208,0)==448 && desiredSize(true,true,1e9,0)==640, "Strict size thresholds failed");
        require(desiredSize(true,true,120,120)==352 && desiredSize(true,true,-120,-120)==352, "Euclidean distance must use both axes");
        require(availableSize(320,{416,640})==416 && availableSize(320,{640})==640 &&
            availableSize(416,{320,640})==640 && availableSize(640,{320,416})==416 &&
            availableSize(640,{320})==320 && availableSize(320,{})==0, "Larger-size fallback or maximum cap failed");
        std::filesystem::create_directory(directory/L"全档位");
        std::vector<int> allSizes;
        for (int size=160; size<=640; size+=32) {
            allSizes.push_back(size);
            for (const auto ext : {L".param",L".bin",L".onnx",L".trt"})
                std::ofstream(directory/L"全档位"/(std::to_wstring(size)+ext)).put('x');
            require(desiredSize(true,true,size/2.0-.01,0)==size, "Stage not reachable just below its distance limit");
            require(desiredSize(true,true,size/2.0,0)==(std::min)(640,size+32), "Boundary must advance by 32");
        }
        for (const auto name : {L"128.onnx",L"176.onnx",L"672.onnx",L"abc.onnx",L"0160.onnx"})
            std::ofstream(directory/L"全档位"/name).put('x');
        for (int engine : {1,2,3}) require(model_files::groupSizes(directory/L"全档位",engine)==allSizes, "Must discover all and only 16 valid stages");
        int previous=160;
        for(int distance=0; distance<=1000; ++distance) {
            const int size=desiredSize(true,true,distance,0);
            require(validSize(size) && size>=previous, "Size must increase monotonically with distance");
            previous=size;
        }
        std::filesystem::remove_all(directory);
        std::cout << "Model discovery passed: Unicode, arbitrary names, empty/missing files, case pairing, sorting, directory exclusion, nonrecursive scan, query validation and JSON escaping.\n";
    } catch(const std::exception& error) {
        // Only this process-created unique temporary directory is removed.
        if (!directory.empty()) { std::error_code ignored; std::filesystem::remove_all(directory,ignored); }
        std::cerr << error.what() << '\n'; return 1;
    }
}
