#pragma once

#include <carto/core/result.hpp>
#include <carto/providers/registry.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::plugin_protocol {

struct PermissionSet {
    bool network = false;
    bool project_write_new_assets = false;
    std::vector<std::string> filesystem_inputs;
};

struct PluginManifest {
    std::string plugin_id;
    providers::Version version;
    std::uint32_t api_version = 1U;
    std::vector<std::string> capabilities;
    providers::TrustClass trust = providers::TrustClass::sandboxed_process;
    PermissionSet permissions;
};

struct Envelope {
    std::string request_id;
    std::string capability;
    std::string method;
    std::string payload_json;
};

class Protocol {
public:
    static constexpr std::uint32_t kApiVersion = 1U;
    static constexpr std::string_view kProtocolName = "carto.plugin.v1";
    static constexpr std::uint32_t kMaxFrameBytes = 4U * 1024U * 1024U;

    [[nodiscard]] static core::Result<void> validate_manifest(const PluginManifest& manifest);
    [[nodiscard]] static core::Result<void> validate_envelope(const Envelope& envelope);
    [[nodiscard]] static core::Result<std::vector<std::uint8_t>> encode(
        const Envelope& envelope);
    [[nodiscard]] static core::Result<Envelope> decode(
        std::span<const std::uint8_t> frame);
};

} // namespace carto::plugin_protocol
