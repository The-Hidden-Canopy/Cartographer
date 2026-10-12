#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace carto::project {
class ProjectDocument;
}

namespace carto::production {

enum class PreparationScopeKind : std::uint8_t {
    asset,
    selection,
    assembly,
    scene,
};

// Stable IDs are captured when the request is submitted. No resolver reads a
// later mutable UI selection. Vectors must be sorted and unique so the request
// has one canonical meaning.
struct PreparationScopeRequest {
    PreparationScopeKind kind = PreparationScopeKind::scene;
    core::Revision expected_source_revision;
    std::vector<std::uint64_t> asset_ids;
    std::vector<std::uint64_t> node_ids;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ResolvedPreparationScope {
    PreparationScopeKind kind = PreparationScopeKind::scene;
    core::Revision source_revision;
    std::vector<std::uint64_t> asset_ids;
    std::vector<std::uint64_t> node_ids;

    [[nodiscard]] core::Result<void> validate(
        const project::ProjectDocument& document) const;
};

// Asset: one explicit mesh asset. Selection: explicit nodes/assets plus the
// ancestor and referenced-asset closure required to preserve transforms.
// Assembly: one root plus descendants, ancestors, and referenced assets.
// Scene: every node and authored mesh asset.
[[nodiscard]] core::Result<ResolvedPreparationScope> resolve_preparation_scope(
    const project::ProjectDocument& document,
    const PreparationScopeRequest& request);

// Backward-compatible convenience for single-mesh file formats. An explicit
// ID always wins. Omission is accepted only when the project has exactly one
// mesh; ambiguous projects fail instead of exporting the first map entry.
[[nodiscard]] core::Result<std::uint64_t> resolve_single_mesh_export(
    const project::ProjectDocument& document,
    std::optional<std::uint64_t> explicit_mesh_asset);

} // namespace carto::production
