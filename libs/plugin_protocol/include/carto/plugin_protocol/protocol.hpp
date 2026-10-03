#pragma once

#include <carto/core/result.hpp>
#include <carto/providers/registry.hpp>

#include <cstdint>
#include <optional>
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

struct ExecutionBudget {
    std::uint32_t max_wall_time_ms = 30'000U;
    std::uint32_t max_cpu_time_ms = 30'000U;
    std::uint32_t max_output_bytes = 4U * 1024U * 1024U;
};

struct PluginManifest {
    std::string plugin_id;
    providers::Version version;
    std::uint32_t api_version = 1U;
    std::vector<std::string> capabilities;
    providers::TrustClass trust = providers::TrustClass::sandboxed_process;
    PermissionSet permissions;
    ExecutionBudget budget;
};

struct Envelope {
    std::string request_id;
    std::string capability;
    std::string method;
    std::string payload_json;
};

// A plugin result is intentionally a neutral data envelope. It has no
// ApplicationAction, ProjectDocument, mesh pointer, or commit authority. The
// host must still perform domain-specific geometry/size validation before any
// future transaction admission.
struct NeutralResult {
    std::string request_id;
    std::string capability;
    std::string payload_json;
    std::optional<std::uint64_t> source_revision;
};

class Protocol {
public:
    static constexpr std::uint32_t kApiVersion = 1U;
    static constexpr std::string_view kProtocolName = "carto.plugin.v1";
    static constexpr std::uint32_t kMaxFrameBytes = 4U * 1024U * 1024U;

    [[nodiscard]] static core::Result<void> validate_manifest(const PluginManifest& manifest);
    [[nodiscard]] static core::Result<void> validate_execution_budget(
        const ExecutionBudget& budget);
    [[nodiscard]] static core::Result<void> validate_admission(
        const PluginManifest& manifest,
        const Envelope& envelope);
    [[nodiscard]] static core::Result<void> validate_neutral_result(
        const PluginManifest& manifest,
        const Envelope& request,
        const NeutralResult& result,
        std::optional<std::uint64_t> expected_source_revision = std::nullopt);
    [[nodiscard]] static core::Result<void> validate_envelope(const Envelope& envelope);
    [[nodiscard]] static core::Result<std::vector<std::uint8_t>> encode(
        const Envelope& envelope);
    [[nodiscard]] static core::Result<Envelope> decode(
        std::span<const std::uint8_t> frame);
};

} // namespace carto::plugin_protocol
