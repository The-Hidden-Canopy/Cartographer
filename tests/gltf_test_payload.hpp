#pragma once
#include <bit>
#include <carto/core/json.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/mesh_attributes/provenance.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/transaction.hpp>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
using namespace carto;
void check(bool v, const char *s) {
    if (!v)
        throw std::runtime_error(s);
}
template <class T> T take(core::Result<T> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
    return std::move(r.value());
}
void okay(core::Result<void> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
}
std::string read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    check(bool(f), "fixture file readable");
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string_view member(std::string_view text, std::string_view key) {
    auto parsed = take(core::json::parse_object(text));
    auto m = core::json::find_member(parsed, key);
    if (!m) std::cerr << "Missing JSON member: " << key << '\n';
    check(m != nullptr, "JSON member present");
    return m->raw_value;
}
std::uint64_t number(std::string_view s) { return take(core::json::parse_uint(s)); }
std::vector<std::string_view> array(std::string_view s) {
    check(s.front() == '[' && s.back() == ']', "JSON array shape");
    std::vector<std::string_view> result;
    std::size_t start = 1;
    int depth = 0;
    bool quoted = false, escaped = false;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
            --depth;
        else if (c == ',' && depth == 0) {
            result.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (s.substr(start, s.size() - 1 - start).find_first_not_of(" \r\n\t") !=
        std::string_view::npos)
        result.push_back(s.substr(start, s.size() - 1 - start));
    return result;
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
    check(p + 4 <= b.size(), "binary u32 bound");
    return b[p] | (std::uint32_t(b[p + 1]) << 8) | (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}
std::vector<std::uint8_t> unbase64(std::string_view s) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    unsigned count = 0;
    for (char c : s) {
        if (c == '=')
            break;
        auto n = alphabet.find(c);
        check(n != alphabet.npos, "embedded buffer base64 alphabet");
        bits = (bits << 6) | static_cast<unsigned>(n);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>(bits >> count));
        }
    }
    return out;
}
struct Payload {
    std::string json;
    std::vector<std::uint8_t> bytes;
    explicit Payload(const std::filesystem::path &p) {
        auto file = read(p);
        if (p.extension() == ".glb") {
            auto raw = std::span(reinterpret_cast<const std::uint8_t *>(file.data()), file.size());
            check(u32(raw, 0) == 0x46546c67 && u32(raw, 4) == 2 && u32(raw, 8) == raw.size(),
                  "GLB header and length");
            auto length = u32(raw, 12);
            check(u32(raw, 16) == 0x4e4f534a && 20ULL + length + 8 <= raw.size(), "GLB JSON chunk");
            json = file.substr(20, length);
            auto offset = 20 + length;
            auto bin = u32(raw, offset);
            check(u32(raw, offset + 4) == 0x004e4942 && offset + 8ULL + bin == raw.size(),
                  "GLB BIN chunk");
            bytes.assign(raw.begin() + offset + 8, raw.end());
        } else {
            json = std::move(file);
            auto uri =
                take(core::json::decode_string(member(array(member(json, "buffers"))[0], "uri")));
            constexpr std::string_view prefix = "data:application/octet-stream;base64,";
            check(uri.starts_with(prefix), "single embedded glTF buffer");
            bytes = unbase64(std::string_view(uri).substr(prefix.size()));
        }
        check(bool(core::json::parse_object(json)), "export JSON grammar");
        check(number(member(array(member(json, "buffers"))[0], "byteLength")) <= bytes.size(),
              "buffer length bounded");
    }
    std::vector<double> accessor(std::size_t id) const {
        auto a = array(member(json, "accessors")).at(id);
        auto v = array(member(json, "bufferViews")).at(number(member(a, "bufferView")));
        auto offset = number(member(v, "byteOffset"));
        auto count = number(member(a, "count"));
        auto component = number(member(a, "componentType"));
        auto type = take(core::json::decode_string(member(a, "type")));
        unsigned channels = type == "SCALAR" ? 1
                            : type == "VEC2" ? 2
                            : type == "VEC3" ? 3
                            : type == "VEC4" ? 4
                                             : 0;
        check(channels != 0, "accessor type");
        unsigned size = component == 5123 ? 2 : 4;
        check(component == 5123 || component == 5125 || component == 5126, "accessor component");
        check(count * channels * size <= number(member(v, "byteLength")) &&
                  offset + count * channels * size <= bytes.size(),
              "accessor views bounded");
        std::vector<double> out;
        for (std::size_t i = 0; i < count * channels; ++i) {
            auto pos = offset + i * size;
            if (component == 5126)
                out.push_back(std::bit_cast<float>(u32(bytes, pos)));
            else if (size == 4)
                out.push_back(u32(bytes, pos));
            else
                out.push_back(bytes[pos] | (unsigned(bytes[pos + 1]) << 8));
        }
        return out;
    }
    std::string_view primitive() const {
        return array(member(array(member(json, "meshes"))[0], "primitives"))[0];
    }
    std::vector<double> attribute(std::string_view name) const {
        return accessor(number(member(member(primitive(), "attributes"), name)));
    }
    std::vector<double> indices() const { return accessor(number(member(primitive(), "indices"))); }
};
} // namespace
