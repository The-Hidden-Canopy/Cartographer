#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace carto::deform {

// Delta-smooth correction node (ch11 "delta smoothing / cage or field-based
// correction"). At bind time the mesh is Laplacian-smoothed and each vertex
// stores its offset from the smoothed surface in a local tangent frame. At
// evaluation the deformed mesh is smoothed with the same settings and the
// stored offsets are re-expressed in the new frames. Skinning artefacts
// (collapse, candy-wrapping, noisy weights) are smoothed away while authored
// surface detail rides along. The node is translation- and
// rotation-equivariant and reproduces the bind mesh exactly at rest.
//
// This is a deterministic CPU reference: uniform-weight Laplacian, fixed
// iteration count, area-weighted normals, tangent from the lowest-index
// neighbour. It is not a runtime or GPU claim.

struct DeltaSmoothSettings {
    static constexpr std::uint32_t kMaxIterations = 256U;

    // Laplacian smoothing passes, inside [1, kMaxIterations].
    std::uint32_t iterations = 10U;
    // Per-pass relaxation toward the neighbour average, inside (0, 1].
    double step = 0.5;

    [[nodiscard]] core::Result<void> validate() const;
};

struct DeltaSmoothResult {
    std::vector<core::Vec3d> positions;
    // Distance between the corrected and the input deformed position.
    double max_correction = 0.0;
    double mean_correction = 0.0;
};

class DeltaSmoothBinding final {
public:
    static constexpr std::size_t kMaxVertices = 262'144U;

    // Builds the bind-time offsets for a triangle mesh. Refuses non-finite
    // positions, out-of-range or degenerate (repeated-corner) triangles,
    // vertex counts above kMaxVertices, and any surface vertex whose bind
    // frame is degenerate. Vertices referenced by no triangle carry no frame;
    // they pass through unchanged.
    [[nodiscard]] static core::Result<DeltaSmoothBinding> build(
        std::span<const core::Vec3d> bind_positions,
        std::span<const std::uint32_t> triangle_indices,
        const DeltaSmoothSettings& settings);

    // Applies the correction to positions index-aligned with the bind mesh.
    // Refuses a size mismatch, non-finite input, or a deformed frame that has
    // collapsed (for example a fully degenerate deformation).
    [[nodiscard]] core::Result<DeltaSmoothResult> apply(
        std::span<const core::Vec3d> deformed_positions) const;

    [[nodiscard]] std::size_t vertex_count() const noexcept { return offsets_.size(); }
    [[nodiscard]] const DeltaSmoothSettings& settings() const noexcept { return settings_; }
    // Content digest of the settings, topology, and stored offsets so a
    // receipt can bind the exact correction used.
    [[nodiscard]] const assets::Sha256Digest& digest() const noexcept { return digest_; }

private:
    DeltaSmoothSettings settings_;
    std::vector<std::vector<std::uint32_t>> neighbours_;
    std::vector<std::vector<std::uint32_t>> incident_triangles_;
    std::vector<std::uint32_t> triangles_;
    // Offset in (tangent, bitangent, normal) coordinates; nullopt for
    // vertices outside every triangle.
    std::vector<std::optional<core::Vec3d>> offsets_;
    assets::Sha256Digest digest_;
};

} // namespace carto::deform
