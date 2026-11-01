#pragma once

#include <carto/geometry/sweep.hpp>
#include <carto/native_source/project_mesh_source.hpp>

namespace carto::native_source {

// A source-model measure and independently regenerated selected mesh from one
// project snapshot. Equality of generated and selected mesh content does not
// prove that the selected asset historically originated from the sweep.
struct ProjectStraightSweep {
    ProjectMeshSource selected_mesh;
    geometry::StraightSweepMeasure source_measure;
    std::string sweep_source_bytes;
    std::string centerline_source_bytes;
    assets::Sha256Digest sweep_source_digest;
    assets::Sha256Digest centerline_source_digest;
    assets::Sha256Digest generated_mesh_digest;
    // Content-derived operation identity; it is not a persisted operation log.
    assets::Sha256Digest operation_identity;
    [[nodiscard]] constexpr bool historical_origin_verified() const noexcept { return false; }
};

// Initial scope: a selected root mesh object that is also the source's owner
// frame, and a filled/capped/constant-scale, single axis-aligned sweep. The
// caller must prevent concurrent project mutation during the snapshot copy.
// Units are the project convention (meters); area is m^2 and volume m^3.
[[nodiscard]] core::Result<ProjectStraightSweep> derive_project_straight_sweep(
    const project::ProjectDocument& project, scene::ObjectId selected_object,
    std::string_view sweep_source_identity);

} // namespace carto::native_source
