#include "internal.hpp"

#include <carto/plugin_package/manifest.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::plugin_package {

namespace {

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                static constexpr char hex[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(hex[byte >> 4U]);
                output.push_back(hex[byte & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_string_array(
    std::string& output,
    std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    output.push_back('[');
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) output.push_back(',');
        append_json_string(output, values[index]);
    }
    output.push_back(']');
}

void append_platform_map(
    std::string& output,
    const std::map<std::string, PlatformEntry>& platforms) {
    output.push_back('{');
    std::size_t index = 0U;
    for (const auto& [platform, entry] : platforms) {
        if (index++ != 0U) output.push_back(',');
        append_json_string(output, platform);
        output += ":{";
        output += "\"entrypoint\":";
        append_json_string(output, entry.entrypoint);
        output += ",\"sha256\":";
        append_json_string(output, entry.sha256.hex());
        output.push_back('}');
    }
    output.push_back('}');
}

} // namespace

std::string canonical_manifest_json(const PluginPackageManifest& manifest) {
    std::string output;
    output.reserve(1024U);
    output += "{\"schema\":";
    append_json_string(output, manifest.schema);
    output += ",\"plugin_id\":";
    append_json_string(output, manifest.plugin_id);
    output += ",\"display_name\":";
    append_json_string(output, manifest.display_name);
    output += ",\"publisher\":";
    append_json_string(output, manifest.publisher);
    output += ",\"version\":";
    append_json_string(output, manifest.version);
    output += ",\"cartographer_api\":" + std::to_string(manifest.cartographer_api);
    output += ",\"min_cartographer_version\":";
    append_json_string(output, manifest.min_cartographer_version);
    output += ",\"trust_class\":";
    append_json_string(output, [&]() -> std::string_view {
        switch (manifest.trust_class) {
        case TrustClass::sandboxed_process: return "sandboxed_process";
        case TrustClass::trusted_native: return "trusted_native";
        case TrustClass::data_only: return "data_only";
        }
        return "data_only";
    }());
    output += ",\"capabilities\":";
    append_string_array(output, manifest.capabilities);
    output += ",\"permissions\":{\"network\":";
    output += manifest.permissions.network ? "true" : "false";
    output += ",\"project_write_new_assets\":";
    output += manifest.permissions.project_write_new_assets ? "true" : "false";
    output += ",\"filesystem_inputs\":";
    append_string_array(output, manifest.permissions.filesystem_inputs);
    output += "},\"platforms\":";
    append_platform_map(output, manifest.platforms);
    if (manifest.vanta.has_value()) {
        output += ",\"vanta\":{\"optional\":";
        output += manifest.vanta->optional ? "true" : "false";
        output += ",\"api\":" + std::to_string(manifest.vanta->api);
        output += ",\"platforms\":";
        append_platform_map(output, manifest.vanta->platforms);
        output.push_back('}');
    }
    output.push_back('}');
    return output;
}

assets::Sha256Digest package_content_digest(
    const std::map<std::string, std::vector<std::uint8_t>>& entries) {
    // The signature file is excluded to avoid a circular digest. Length-prefixing
    // names and payloads makes concatenation unambiguous and deterministic.
    std::vector<std::uint8_t> canonical;
    for (const auto& [path, bytes] : entries) {
        if (path == "signature.json") continue;
        const auto append_u64 = [&canonical](std::uint64_t value) {
            for (unsigned index = 0U; index < 8U; ++index) {
                canonical.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
            }
        };
        append_u64(path.size());
        canonical.insert(canonical.end(), path.begin(), path.end());
        append_u64(bytes.size());
        canonical.insert(canonical.end(), bytes.begin(), bytes.end());
    }
    return assets::sha256(canonical);
}

std::vector<std::uint8_t> decode_hex(std::string_view value) {
    if ((value.size() % 2U) != 0U) return {};
    const auto digit = [](char byte) -> int {
        if (byte >= '0' && byte <= '9') return byte - '0';
        if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
        if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> result(value.size() / 2U);
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const int high = digit(value[index * 2U]);
        const int low = digit(value[index * 2U + 1U]);
        if (high < 0 || low < 0) return {};
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

std::string encode_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0fU]);
    }
    return result;
}

} // namespace carto::plugin_package
