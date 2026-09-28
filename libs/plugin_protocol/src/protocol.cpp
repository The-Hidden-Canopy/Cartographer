#include <carto/plugin_protocol/protocol.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace carto::plugin_protocol {

namespace {

constexpr std::size_t kMaxTokenBytes = 128U;
constexpr std::size_t kMaxCapabilities = 64U;
constexpr std::size_t kMaxFilesystemInputs = 32U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_token(std::string_view value) {
    return !value.empty() && value.size() <= kMaxTokenBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

bool safe_request_id(std::string_view value) {
    return safe_token(value) && value.size() >= 8U;
}

bool valid_json_object(std::string_view text) {
    if (text.empty() || text.front() != '{') return false;
    std::size_t object_depth = 0U;
    std::size_t array_depth = 0U;
    bool in_string = false;
    bool escaped = false;
    bool root_closed = false;
    for (const char byte : text) {
        if (in_string) {
            if (escaped) {
                if (byte != '\\' && byte != '"' && byte != '/' && byte != 'b' &&
                    byte != 'f' && byte != 'n' && byte != 'r' && byte != 't' && byte != 'u') {
                    return false;
                }
                escaped = false;
            } else if (byte == '\\') {
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            } else if (static_cast<unsigned char>(byte) < 0x20U) {
                return false;
            }
            continue;
        }
        if (root_closed) {
            if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t') return false;
            continue;
        }
        if (byte == '"') {
            in_string = true;
        } else if (byte == '{') {
            ++object_depth;
        } else if (byte == '}') {
            if (object_depth == 0U) return false;
            --object_depth;
            if (object_depth == 0U && array_depth == 0U) root_closed = true;
        } else if (byte == '[') {
            ++array_depth;
        } else if (byte == ']') {
            if (array_depth == 0U) return false;
            --array_depth;
        }
    }
    return root_closed && !in_string && !escaped && object_depth == 0U && array_depth == 0U;
}

bool valid_json_payload(std::string_view value) {
    return value.size() <= Protocol::kMaxFrameBytes && valid_json_object(value);
}

std::string escape_json(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    for (const char byte : value) {
        if (byte == '\\' || byte == '"') result.push_back('\\');
        result.push_back(byte);
    }
    return result;
}

std::string make_json(const Envelope& envelope) {
    return "{\"protocol\":\"" + std::string(Protocol::kProtocolName) +
        "\",\"request_id\":\"" + escape_json(envelope.request_id) +
        "\",\"capability\":\"" + escape_json(envelope.capability) +
        "\",\"method\":\"" + escape_json(envelope.method) +
        "\",\"payload\":" + envelope.payload_json + "}";
}

core::Result<std::string> json_string(std::string_view text, std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":\"";
    const std::size_t start = text.find(marker);
    if (start == std::string_view::npos) {
        return core::Result<std::string>::failure(validation("plugin envelope field is missing"));
    }
    const std::size_t value_start = start + marker.size();
    std::size_t cursor = value_start;
    std::string value;
    while (cursor < text.size()) {
        const char byte = text[cursor++];
        if (byte == '"') return core::Result<std::string>::success(std::move(value));
        if (byte == '\\' || static_cast<unsigned char>(byte) < 0x20U) {
            return core::Result<std::string>::failure(validation("plugin envelope string is malformed"));
        }
        value.push_back(byte);
    }
    return core::Result<std::string>::failure(validation("plugin envelope string is unterminated"));
}

core::Result<std::string> json_payload(std::string_view text) {
    const std::string marker = "\"payload\":";
    const std::size_t start = text.find(marker);
    if (start == std::string_view::npos) {
        return core::Result<std::string>::failure(validation("plugin envelope payload is missing"));
    }
    const std::size_t value_start = start + marker.size();
    if (value_start >= text.size() || text[value_start] != '{' || text.back() != '}') {
        return core::Result<std::string>::failure(validation("plugin envelope payload is not an object"));
    }
    return core::Result<std::string>::success(
        std::string(text.substr(value_start, text.size() - value_start - 1U)));
}

} // namespace

core::Result<void> Protocol::validate_manifest(const PluginManifest& manifest) {
    if (!safe_token(manifest.plugin_id) || manifest.api_version != kApiVersion) {
        return core::Result<void>::failure(invalid("plugin manifest id or API version is invalid"));
    }
    if (manifest.trust == providers::TrustClass::remote_service || manifest.permissions.network) {
        return core::Result<void>::failure(validation(
            "plugin manifests cannot request implicit remote or network authority"));
    }
    if (manifest.capabilities.empty() || manifest.capabilities.size() > kMaxCapabilities) {
        return core::Result<void>::failure(invalid("plugin manifest capabilities are invalid"));
    }
    for (const auto& capability : manifest.capabilities) {
        if (!safe_token(capability)) {
            return core::Result<void>::failure(invalid("plugin manifest capability is invalid"));
        }
    }
    if (manifest.permissions.filesystem_inputs.size() > kMaxFilesystemInputs) {
        return core::Result<void>::failure(invalid("plugin filesystem permission list is too large"));
    }
    for (const auto& path : manifest.permissions.filesystem_inputs) {
        if (!safe_token(path) || path.find("..") != std::string::npos) {
            return core::Result<void>::failure(invalid("plugin filesystem permission is invalid"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> Protocol::validate_envelope(const Envelope& envelope) {
    if (!safe_request_id(envelope.request_id) || !safe_token(envelope.capability) ||
        !safe_token(envelope.method) || !valid_json_payload(envelope.payload_json)) {
        return core::Result<void>::failure(invalid("plugin envelope contains an invalid field"));
    }
    if (envelope.payload_json.size() + 256U > kMaxFrameBytes) {
        return core::Result<void>::failure(invalid("plugin envelope exceeds the frame limit"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> Protocol::encode(const Envelope& envelope) {
    if (auto result = validate_envelope(envelope); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    const std::string json = make_json(envelope);
    const std::uint64_t frame_bytes = 4ULL + json.size();
    if (frame_bytes > kMaxFrameBytes || json.size() > 0xffff'ffffULL) {
        return core::Result<std::vector<std::uint8_t>>::failure(invalid("plugin frame is too large"));
    }
    std::vector<std::uint8_t> frame(json.size() + 4U);
    const std::uint32_t size = static_cast<std::uint32_t>(json.size());
    frame[0] = static_cast<std::uint8_t>(size);
    frame[1] = static_cast<std::uint8_t>(size >> 8U);
    frame[2] = static_cast<std::uint8_t>(size >> 16U);
    frame[3] = static_cast<std::uint8_t>(size >> 24U);
    std::memcpy(frame.data() + 4U, json.data(), json.size());
    return core::Result<std::vector<std::uint8_t>>::success(std::move(frame));
}

core::Result<Envelope> Protocol::decode(std::span<const std::uint8_t> frame) {
    if (frame.size() < 4U) {
        return core::Result<Envelope>::failure(validation("plugin frame is truncated"));
    }
    const std::uint32_t size = static_cast<std::uint32_t>(frame[0]) |
        (static_cast<std::uint32_t>(frame[1]) << 8U) |
        (static_cast<std::uint32_t>(frame[2]) << 16U) |
        (static_cast<std::uint32_t>(frame[3]) << 24U);
    if (size > kMaxFrameBytes - 4U || frame.size() != static_cast<std::size_t>(size) + 4U) {
        return core::Result<Envelope>::failure(validation("plugin frame length is invalid"));
    }
    const std::string_view json(reinterpret_cast<const char*>(frame.data() + 4U), size);
    const std::string protocol_marker = "\"protocol\":\"" + std::string(kProtocolName) + "\"";
    if (json.find(protocol_marker) == std::string_view::npos || !valid_json_object(json)) {
        return core::Result<Envelope>::failure(validation("plugin frame protocol is invalid"));
    }
    const auto request_id = json_string(json, "request_id");
    const auto capability = json_string(json, "capability");
    const auto method = json_string(json, "method");
    const auto payload = json_payload(json);
    if (!request_id || !capability || !method || !payload) {
        const auto& diagnostic = !request_id ? request_id.error() : !capability ? capability.error() :
            !method ? method.error() : payload.error();
        return core::Result<Envelope>::failure(diagnostic);
    }
    Envelope envelope{request_id.value(), capability.value(), method.value(), payload.value()};
    if (auto result = validate_envelope(envelope); !result) {
        return core::Result<Envelope>::failure(result.error());
    }
    return core::Result<Envelope>::success(std::move(envelope));
}

} // namespace carto::plugin_protocol
