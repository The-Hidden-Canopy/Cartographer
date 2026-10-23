#pragma once

#include <carto/core/result.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::providers {

enum class ProviderKind {
    geometry_import,
    geometry_export,
    render_backend,
    material_compiler,
    cad_solver,
    bim,
};

enum class TrustClass {
    trusted_in_process,
    sandboxed_process,
    data_only,
    script,
    remote_service,
};

enum class ProviderState {
    discovered,
    inspected,
    registered,
    verified,
    ready,
    loaded,
    unsupported,
    version_mismatch,
    capability_missing,
    dependency_missing,
    untrusted,
    load_failed,
    unavailable,
};

struct Version {
    std::uint32_t major = 0U;
    std::uint32_t minor = 0U;
    std::uint32_t patch = 0U;

    [[nodiscard]] constexpr auto operator<=>(const Version&) const noexcept = default;
};

struct ProviderDescriptor {
    std::string id;
    ProviderKind kind = ProviderKind::geometry_import;
    std::vector<std::string> capabilities;
    Version version;
    TrustClass trust = TrustClass::data_only;
    ProviderState state = ProviderState::discovered;
};

class Registry {
public:
    [[nodiscard]] core::Result<void> discover(ProviderDescriptor descriptor);
    [[nodiscard]] core::Result<void> transition(
        std::string_view provider_id,
        ProviderState target);
    [[nodiscard]] core::Result<ProviderDescriptor> resolve(
        std::string_view capability) const;
    [[nodiscard]] const std::map<std::string, ProviderDescriptor>& providers() const noexcept {
        return providers_;
    }

private:
    [[nodiscard]] static core::Result<void> validate_descriptor(
        const ProviderDescriptor& descriptor);
    [[nodiscard]] static bool can_transition(ProviderState from, ProviderState to) noexcept;

    std::map<std::string, ProviderDescriptor> providers_;
};

} // namespace carto::providers
