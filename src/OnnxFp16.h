#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Native ONNX protobuf transformation for the raw detection exports accepted by
// this application. TensorRT 11 derives precision from the graph, not a builder
// flag. Preserve unknown fields and metadata, change floating tensors to HALF,
// and insert the FLOAT casts required by ONNX Resize's ROI/scales contract.
namespace onnx_fp16 {
inline uint16_t half(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000, magnitude = bits & 0x7fffffff;
    if (magnitude >= 0x7f800000) {
        if (magnitude == 0x7f800000) return uint16_t(sign | 0x7c00);
        return uint16_t(sign | 0x7c00 | (0x200 | ((magnitude >> 13) & 0x3ff)));
    }
    // Clamp finite weights to the finite HALF range instead of introducing Inf.
    if (magnitude > 0x477fe000) return uint16_t(sign | 0x7bff);
    const uint32_t exponent = magnitude >> 23, mantissa = magnitude & 0x7fffff;
    if (exponent < 102) return uint16_t(sign);
    if (exponent < 113) {
        const uint32_t shift = 126 - exponent, significand = mantissa | 0x800000;
        const uint32_t rounded = (significand + ((1u << (shift - 1)) - 1) + ((significand >> shift) & 1)) >> shift;
        return uint16_t(sign | rounded);
    }
    return uint16_t(sign | (((exponent - 112) << 10) + ((mantissa + 0xfff + ((mantissa >> 13) & 1)) >> 13)));
}
struct Field {
    unsigned number{}, wire{};
    uint64_t value{};
    std::string_view data, original;
};
inline uint64_t readInteger(std::string_view bytes, size_t& offset) {
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (offset == bytes.size()) throw std::runtime_error("Truncated ONNX protobuf integer");
        const unsigned char byte = bytes[offset++];
        if (shift == 63 && (byte & 0xfe)) throw std::runtime_error("Oversized ONNX protobuf integer");
        result |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return result;
    }
    throw std::runtime_error("Invalid ONNX protobuf integer");
}
inline std::vector<Field> fields(std::string_view bytes) {
    std::vector<Field> result;
    for (size_t offset = 0; offset < bytes.size();) {
        const size_t start = offset;
        const uint64_t tag = readInteger(bytes, offset);
        if (!(tag >> 3) || tag >> 3 > 0x1fffffff) throw std::runtime_error("Invalid ONNX protobuf field");
        Field item; item.number = unsigned(tag >> 3); item.wire = unsigned(tag & 7);
        size_t length = 0;
        if (item.wire == 0) item.value = readInteger(bytes, offset);
        else if (item.wire == 1) length = 8;
        else if (item.wire == 5) length = 4;
        else if (item.wire == 2) {
            const auto count = readInteger(bytes, offset);
            if (count > bytes.size() - offset) throw std::runtime_error("Truncated ONNX protobuf field");
            length = size_t(count);
        } else throw std::runtime_error("Unsupported ONNX protobuf wire type");
        if (length > bytes.size() - offset) throw std::runtime_error("Truncated ONNX protobuf field");
        item.data = bytes.substr(offset, length); offset += length;
        item.original = bytes.substr(start, offset - start); result.push_back(item);
    }
    return result;
}
inline void integer(std::string& bytes, uint64_t value) {
    while (value >= 128) { bytes += char((value & 127) | 128); value >>= 7; }
    bytes += char(value);
}
inline void number(std::string& bytes, unsigned field, uint64_t value) {
    integer(bytes, uint64_t(field) << 3); integer(bytes, value);
}
inline void message(std::string& bytes, unsigned field, std::string_view value) {
    integer(bytes, (uint64_t(field) << 3) | 2); integer(bytes, value.size()); bytes.append(value);
}
inline std::string halfData(std::string_view raw) {
    if (raw.size() % 4) throw std::runtime_error("FP32 tensor data is not a multiple of four bytes");
    std::string output; output.reserve(raw.size() / 2);
    for (size_t i = 0; i < raw.size(); i += 4) {
        float value; std::memcpy(&value, raw.data() + i, 4);
        const uint16_t converted = half(value);
        output += char(converted & 255); output += char(converted >> 8);
    }
    return output;
}
inline std::string tensor(std::string_view source) {
    const auto items = fields(source);
    bool floating = false, external = false, hasRaw = false;
    std::string floats; std::string_view raw;
    uint64_t elements = 1;
    for (const auto& item : items) {
        if (item.number == 2 && item.wire == 0) floating = item.value == 1;
        if (item.number == 13 || (item.number == 14 && item.value == 1)) external = true;
        if (item.number == 9 && item.wire == 2) { if (hasRaw) throw std::runtime_error("Duplicate ONNX raw tensor data"); hasRaw = true; raw = item.data; }
        if (item.number == 4 && (item.wire == 2 || item.wire == 5)) floats.append(item.data);
        if (item.number == 1) {
            std::vector<uint64_t> dimensions;
            if (item.wire == 0) dimensions.push_back(item.value);
            else if (item.wire == 2) for (size_t i = 0; i < item.data.size();) dimensions.push_back(readInteger(item.data, i));
            else throw std::runtime_error("Invalid ONNX tensor dimensions");
            for (const uint64_t dimension : dimensions) {
                if (dimension > 536870912 || (elements && dimension > 536870912 / elements)) throw std::runtime_error("Oversized ONNX tensor");
                elements *= dimension;
            }
        }
    }
    if (!floating) return std::string(source);
    if (external) throw std::runtime_error("FP16 conversion requires embedded ONNX weights. Export the model with external_data=false.");
    if (hasRaw && !floats.empty()) throw std::runtime_error("ONNX tensor has both raw and float data");
    const auto values = hasRaw ? raw : std::string_view(floats);
    if (elements * 4 != values.size()) throw std::runtime_error("FP32 tensor dimensions and data length disagree");
    std::string result;
    for (const auto& item : items) if (item.number != 2 && item.number != 4 && item.number != 9) result.append(item.original);
    number(result, 2, 10); message(result, 9, halfData(values)); return result;
}
inline std::string sparse(std::string_view source) {
    std::string result;
    for (const auto& item : fields(source)) {
        if ((item.number == 1 || item.number == 2) && item.wire == 2) message(result, item.number, tensor(item.data));
        else result.append(item.original);
    }
    return result;
}
inline std::string tensorType(std::string_view source) {
    std::string result;
    for (const auto& item : fields(source)) {
        if (item.number == 1 && item.wire == 0 && item.value == 1) number(result, 1, 10);
        else result.append(item.original);
    }
    return result;
}
inline std::string valueInfo(std::string_view source) {
    std::string result;
    for (const auto& item : fields(source)) {
        if (item.number != 2 || item.wire != 2) { result.append(item.original); continue; }
        std::string type;
        for (const auto& child : fields(item.data)) {
            if (child.number == 1 && child.wire == 2) message(type, 1, tensorType(child.data));
            else type.append(child.original);
        }
        message(result, 2, type);
    }
    return result;
}
struct Transform {
    std::set<std::string> names;
    unsigned serial = 0;
    unsigned depth = 0;
    std::string unique() {
        for (;;) { auto name = "__saga_fp16_" + std::to_string(++serial); if (names.insert(name).second) return name; }
    }
    std::string attribute(std::string_view source, bool cast, bool constant) {
        const auto items = fields(source);
        std::string_view name;
        for (const auto& item : items) if (item.number == 1 && item.wire == 2) name = item.data;
        if (constant && (name == "value_float" || name == "value_floats")) {
            std::string raw;
            for (const auto& item : items) if ((name == "value_float" && item.number == 2 && item.wire == 5) ||
                (name == "value_floats" && item.number == 7 && (item.wire == 2 || item.wire == 5))) raw.append(item.data);
            if (raw.size() % 4 || (name == "value_float" && raw.size() != 4)) throw std::runtime_error("Invalid floating Constant attribute");
            std::string value; number(value, 2, 10);
            if (name == "value_floats") number(value, 1, raw.size() / 4);
            message(value, 9, halfData(raw));
            std::string result; message(result, 1, "value"); number(result, 20, 4); message(result, 5, value); return result;
        }
        std::string result;
        for (const auto& item : items) {
            if (cast && name == "to" && item.number == 3 && item.wire == 0 && item.value == 1) number(result, 3, 10);
            else if ((item.number == 5 || item.number == 10) && item.wire == 2) message(result, item.number, tensor(item.data));
            else if ((item.number == 6 || item.number == 11) && item.wire == 2) message(result, item.number, graph(item.data));
            else if ((item.number == 22 || item.number == 23) && item.wire == 2) message(result, item.number, sparse(item.data));
            else result.append(item.original);
        }
        return result;
    }
    std::string graph(std::string_view source) {
        if (++depth > 32) throw std::runtime_error("ONNX subgraph nesting exceeds 32 levels");
        const auto items = fields(source);
        // Reserve original tensor/node names before introducing cast nodes.
        for (const auto& item : items) {
            if (item.number == 1 && item.wire == 2) for (const auto& child : fields(item.data)) {
                if ((child.number == 1 || child.number == 2 || child.number == 3) && child.wire == 2) names.emplace(child.data);
            }
            else if ((item.number == 11 || item.number == 12 || item.number == 13) && item.wire == 2) {
                for (const auto& child : fields(item.data)) if (child.number == 1 && child.wire == 2) names.emplace(child.data);
            } else if (item.number == 5 && item.wire == 2) {
                for (const auto& child : fields(item.data)) if (child.number == 8 && child.wire == 2) names.emplace(child.data);
            }
        }
        std::string result;
        for (const auto& item : items) {
            if (item.number == 1 && item.wire == 2) {
                const auto children = fields(item.data);
                std::string_view operation, domain;
                for (const auto& child : children) {
                    if (child.number == 4 && child.wire == 2) operation = child.data;
                    if (child.number == 7 && child.wire == 2) domain = child.data;
                }
                if (!domain.empty() && domain != "ai.onnx") throw std::runtime_error("FP16 conversion requires standard ONNX operators, without custom domains");
                if (operation == "Range") throw std::runtime_error("FP16 conversion of Range is unsupported; export a fixed-size YOLO model");
                std::string node; unsigned input = 0;
                for (const auto& child : children) {
                    if (child.number == 1 && child.wire == 2) {
                        if (operation == "Resize" && (input == 1 || input == 2) && !child.data.empty()) {
                            const auto output = unique();
                            std::string cast; message(cast, 1, child.data); message(cast, 2, output); message(cast, 3, unique()); message(cast, 4, "Cast");
                            std::string to; message(to, 1, "to"); number(to, 3, 1); number(to, 20, 2); message(cast, 5, to);
                            message(result, 1, cast); message(node, 1, output);
                        } else node.append(child.original);
                        ++input;
                    } else if (child.number == 5 && child.wire == 2) message(node, 5, attribute(child.data, operation == "Cast", operation == "Constant"));
                    else node.append(child.original);
                }
                message(result, 1, node);
            } else if (item.number == 5 && item.wire == 2) message(result, 5, tensor(item.data));
            else if ((item.number == 11 || item.number == 12) && item.wire == 2) message(result, item.number, valueInfo(item.data));
            else if (item.number == 15 && item.wire == 2) message(result, 15, sparse(item.data));
            // Internal annotations can become stale around the required casts;
            // public input/output types stay explicit, the parser infers others.
            else if (item.number != 13) result.append(item.original);
        }
        --depth; return result;
    }
};
inline std::vector<char> convert(const std::vector<char>& model) {
    const auto items = fields(std::string_view(model.data(), model.size()));
    Transform transform; std::string result; unsigned graphs = 0;
    for (const auto& item : items) {
        if (item.number == 25) throw std::runtime_error("FP16 conversion requires inlined ONNX functions; re-export without local FunctionProto definitions");
        if (item.number == 7 && item.wire == 2) { message(result, 7, transform.graph(item.data)); ++graphs; }
        else result.append(item.original);
    }
    if (graphs != 1) throw std::runtime_error("ONNX model must contain exactly one graph");
    return {result.begin(), result.end()};
}
}
