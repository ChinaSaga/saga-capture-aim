#include "OnnxFp16.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void structureTests() {
    using namespace onnx_fp16;
    // Test both encodings of FLOAT weights and reject dimension/data mismatch.
    float values[] = {1.f, -2.f};
    const std::string_view raw(reinterpret_cast<const char*>(values), sizeof(values));
    for (unsigned field : {4u, 9u}) {
        std::string input; number(input, 1, 2); number(input, 2, 1); message(input, 8, "weights"); message(input, field, raw);
        bool type = false, data = false;
        const std::string convertedTensor = tensor(input);
        for (const auto& item : fields(convertedTensor)) {
            if (item.number == 2) type = item.value == 10;
            if (item.number == 9) data = item.data == std::string("\x00\x3c\x00\xc0",4);
        }
        require(type && data, "Tensor data/type conversion differs");
        number(input, 1, 3);
        bool rejected=false; try { tensor(input); } catch(...) { rejected=true; }
        require(rejected,"Mismatched tensor dimensions must be rejected");
    }
    // Resize parameters require FLOAT; preserve integer sizes and avoid names
    // already present in the original graph when inserting those casts.
    std::string resize; message(resize, 1, "image"); message(resize, 1, ""); message(resize, 1, "scales");
    message(resize, 1, "sizes"); message(resize, 2, "__saga_fp16_1"); message(resize, 4, "Resize");
    std::string graph; message(graph,1,resize);
    Transform transform;
    const std::string converted = transform.graph(graph);
    auto nodes = fields(converted);
    require(nodes.size()==2,"Resize requires exactly one scales cast");
    auto cast = fields(nodes[0].data), node = fields(nodes[1].data);
    std::vector<std::string> inputs;
    std::string castOutput;
    bool floatCast = false;
    for (const auto& item : cast) {
        if (item.number == 2) castOutput = item.data;
        if (item.number == 5) for (const auto& attr : fields(item.data)) if (attr.number == 3) floatCast = attr.value == 1;
    }
    for (const auto& item : node) if (item.number == 1) inputs.emplace_back(item.data);
    require(floatCast && castOutput!="__saga_fp16_1" && inputs==std::vector<std::string>{"image","",castOutput,"sizes"},"Resize parameter cast/name collision");
}
static float unpack(uint16_t value) {
    const uint32_t sign = uint32_t(value & 0x8000) << 16;
    int exponent = (value >> 10) & 31; uint32_t mantissa = value & 1023;
    if (!exponent && mantissa) {
        exponent = -14;
        while (!(mantissa & 1024)) { mantissa <<= 1; --exponent; }
        return std::bit_cast<float>(sign | (uint32_t(exponent + 127) << 23) | ((mantissa & 1023) << 13));
    }
    return std::bit_cast<float>(sign | (exponent == 31 ? 0x7f800000 : exponent ? uint32_t(exponent + 112) << 23 : 0) | (mantissa << 13));
}
int wmain(int argc, wchar_t** argv) {
    try {
        structureTests();
        for (unsigned bits = 0; bits <= 65535; ++bits) {
            const auto value = uint16_t(bits);
            if ((value & 0x7c00) == 0x7c00 && (value & 1023)) continue;
            require(onnx_fp16::half(unpack(value)) == value, "HALF roundtrip differs");
        }
        require(onnx_fp16::half(1.00048828125f) == 0x3c00, "Round-to-even lower tie");
        require(onnx_fp16::half(1.00146484375f) == 0x3c02, "Round-to-even upper tie");
        require(onnx_fp16::half(1e30f) == 0x7bff, "Finite overflow must clamp");
        require(onnx_fp16::half(std::ldexp(1.f,-25)) == 0, "Subnormal tie must round to even");
        for (const std::string broken : {std::string("\x80",1),std::string("\x3a\x7f",2),std::string("\x00",1)}) {
            bool rejected=false; try { onnx_fp16::convert({broken.begin(),broken.end()}); } catch(...) { rejected=true; }
            require(rejected,"Malformed protobuf must be rejected");
        }
        if (argc == 3) {
            const auto input = std::filesystem::absolute(argv[1]), output = std::filesystem::absolute(argv[2]);
            require(input != output && !std::filesystem::exists(output), "Choose a fresh separate fixture output");
            std::ifstream file(input,std::ios::binary);
            require(bool(file), "Cannot read input fixture");
            const std::vector<char> original((std::istreambuf_iterator<char>(file)), {});
            const auto converted=onnx_fp16::convert(original);
            std::ofstream target(output,std::ios::binary); target.write(converted.data(),converted.size());
            require(bool(target), "Cannot write FP16 fixture");
            std::cout << "Native FP16 ONNX fixture bytes=" << converted.size() << '\n';
        } else require(argc==1,"Usage: test_onnx_fp16 [SOURCE.onnx FRESH_FP16.onnx]");
        std::cout << "PASS HALF roundtrip/rounding, tensor conversion, Resize casts and malformed protobuf\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
