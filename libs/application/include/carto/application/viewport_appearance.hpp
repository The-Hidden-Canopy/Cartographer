#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/render/material.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace carto::project {
class ProjectDocument;
}

namespace carto::application {

// Why a face shows (or does not show) authored appearance in the live
// viewport. Only `authored` carries a color; every other state is drawn as
// an explicit "no authored material" marking, never as a substitute color.
enum class FaceAppearanceState : std::uint8_t {
    authored,
    no_region_layer,
    ambiguous_region_layers,
    unassigned_region,
    unresolved_material,
};

struct FaceAppearance {
    FaceAppearanceState state = FaceAppearanceState::no_region_layer;
    render::MaterialId material = 0U;
    // Linear base-color factor of the resolved material (variant applied).
    core::Vec4d base_color{};
    // True when the material also references a base-color texture that the
    // live viewport does not sample; the factor alone is shown.
    bool base_color_texture_not_sampled = false;
};

struct MeshAppearance {
    std::uint64_t mesh_asset = 0U;
    core::Revision mesh_revision;
    std::string region_layer;
    // Per authored face. Faces absent here are reported by `state`.
    std::map<geometry::FaceId, FaceAppearance> faces;
    // Mesh-wide state when the mesh cannot be mapped at all.
    FaceAppearanceState state = FaceAppearanceState::authored;
    std::string detail;
};

// Resolves the same face -> material-region -> catalog assignment -> material
// chain the native scene renderer uses (layers `condition.material_region` or
// `cartographer.material_slot`). It never invents a material: faces without a
// complete mapping keep a non-`authored` state and no color.
[[nodiscard]] MeshAppearance resolve_mesh_appearance(
    const project::ProjectDocument& document, std::uint64_t mesh_asset);

[[nodiscard]] std::map<std::uint64_t, MeshAppearance> resolve_viewport_appearance(
    const project::ProjectDocument& document);

[[nodiscard]] const char* to_string(FaceAppearanceState state) noexcept;

} // namespace carto::application
