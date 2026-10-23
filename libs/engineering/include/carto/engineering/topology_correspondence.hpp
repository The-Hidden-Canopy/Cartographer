#pragma once

#include <carto/engineering/authoring.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::engineering {

struct TopologyIdentityKey {
    std::string source_namespace;
    std::string part_external_id;
    TopologyKind kind = TopologyKind::face;
    std::string topology_external_id;
    [[nodiscard]] bool operator==(const TopologyIdentityKey&) const noexcept = default;
};

// Identity excludes file revision and display tessellation indices. Stable
// source external IDs therefore survive unchanged-file reimports and mesh
// triangle reorderings without pretending that geometry correspondence is
// exact when the provider cannot establish it.
[[nodiscard]] core::Result<assets::Sha256Digest> persistent_topology_id(
    const TopologyIdentityKey& key);

struct DisplayTriangleBinding {
    TopologyIdentityKey topology;
    assets::Sha256Digest persistent_id;
    std::uint64_t display_mesh_asset = 0U;
    core::Revision display_mesh_revision;
    assets::Sha256Digest display_mesh_digest;
    std::uint32_t triangle = 0U;
};

struct TopologyCorrespondenceMap {
    static constexpr std::size_t kMaxBindings = 2'000'000U;

    std::string source_namespace;
    std::string source_revision;
    assets::Sha256Digest source_digest;
    std::vector<DisplayTriangleBinding> bindings;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> resolve_display_triangle(
        std::string_view expected_source_revision,
        std::uint64_t display_mesh_asset,
        core::Revision display_mesh_revision,
        const assets::Sha256Digest& display_mesh_digest,
        std::uint32_t triangle) const;
};

struct TopologyReimportReport {
    std::vector<assets::Sha256Digest> preserved_identities;
    std::vector<assets::Sha256Digest> added_identities;
    std::vector<assets::Sha256Digest> removed_identities;
    std::vector<assets::Sha256Digest> changed_correspondence_identities;
    std::vector<std::uint64_t> ambiguous_display_mesh_assets;
    bool source_changed = false;
    bool publication_blocked = false;
};

// This comparison is read-only: callers retain the previous authoring
// document and block publication whenever source or topology correspondence
// changed, leaving human reconciliation as a separate action.
[[nodiscard]] core::Result<TopologyReimportReport> compare_topology_reimport(
    const TopologyCorrespondenceMap& previous,
    const TopologyCorrespondenceMap& incoming);

} // namespace carto::engineering
