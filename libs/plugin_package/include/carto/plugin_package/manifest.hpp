#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/plugin_protocol/protocol.hpp>
#include <carto/providers/registry.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace carto::plugin_package {

enum class TrustClass {
    sandboxed_process,
    trusted_native,
    data_only,
};

struct PermissionSet {
    bool network = false;
    bool project_write_new_assets = false;
    std::vector<std::string> filesystem_inputs;
};

struct PlatformEntry {
    std::string entrypoint;
    assets::Sha256Digest sha256;
};

struct VantaSection {
    bool optional = false;
    std::uint32_t api = 0U;
    std::map<std::string, PlatformEntry> platforms;
};

struct PluginPackageManifest {
    std::string schema;
    std::string plugin_id;
    std::string display_name;
    std::string publisher;
    std::string version;
    std::uint32_t cartographer_api = 0U;
    std::string min_cartographer_version;
    TrustClass trust_class = TrustClass::sandboxed_process;
    std::vector<std::string> capabilities;
    PermissionSet permissions;
    std::map<std::string, PlatformEntry> platforms;
    std::optional<VantaSection> vanta;
};

[[nodiscard]] core::Result<PluginPackageManifest> parse_manifest(std::string_view json);
[[nodiscard]] core::Result<plugin_protocol::PluginManifest> to_plugin_manifest(
    const PluginPackageManifest& manifest);
[[nodiscard]] providers::TrustClass map_trust_class(TrustClass trust) noexcept;

} // namespace carto::plugin_package
