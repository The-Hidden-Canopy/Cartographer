#pragma once

#include <carto/condition/authoring.hpp>
#include <carto/spatial_field/scalar_field.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace carto::condition {

struct VegetationFieldReceipt {
    std::string identity;
    spatial_field::FieldSemantic semantic = spatial_field::FieldSemantic::generic;
    core::Revision source_revision;
    assets::Sha256Digest digest;
};

struct VegetationInstancePreview {
    std::uint64_t stable_id = 0U;
    Id recipe_id = 0U;
    Id species_asset_id = 0U;
    std::uint32_t grid_u = 0U;
    std::uint32_t grid_v = 0U;
    core::Vec3d position;
    double yaw_radians = 0.0;
    double uniform_scale = 1.0;
    double suitability = 0.0;
};

struct VegetationPreviewResult {
    static constexpr std::size_t kMaxCandidates = 65'536U;

    Id recipe_id = 0U;
    core::Revision project_revision;
    assets::Sha256Digest authoring_digest;
    assets::Sha256Digest preview_digest;
    std::size_t candidate_count = 0U;
    std::vector<VegetationFieldReceipt> fields;
    std::vector<VegetationInstancePreview> instances;
};

// Builds an ephemeral deterministic forest-placement preview from exact,
// revision-and-digest-bound project fields. It does not mutate the project or
// turn generated placements into scene authority.
[[nodiscard]] core::Result<VegetationPreviewResult> preview_vegetation(
    const AuthoringDocument& authoring,
    const std::map<std::string, spatial_field::ScalarField2D>& fields,
    Id recipe_id);

} // namespace carto::condition
