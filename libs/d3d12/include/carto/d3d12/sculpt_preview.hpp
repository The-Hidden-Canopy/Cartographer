#pragma once

#include <carto/d3d12/device.hpp>
#include <carto/production/sculpt_stack.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::d3d12 {

inline constexpr std::string_view kD3D12SculptPreviewAlgorithm =
    "carto.sculpt.d3d12.float32-preview.v2";
inline constexpr std::string_view kD3D12SculptPreviewEvidenceClass =
    "carto.d3d12.transient-preview.cpu-checked";
inline constexpr double kD3D12SculptPreviewAbsoluteTolerance = 2.0e-5;
inline constexpr double kD3D12SculptPreviewRelativeTolerance = 2.0e-6;

struct SculptPreviewPosition {
    geometry::VertexId vertex;
    core::Vec3d position;

    [[nodiscard]] bool operator==(const SculptPreviewPosition& other) const noexcept {
        return vertex == other.vertex && position.x == other.position.x &&
            position.y == other.position.y && position.z == other.position.z;
    }
};

// A transient, non-authoritative GPU product. It has no project-staging or
// publication API; commits continue through compile_sculpt_stack and the
// existing project transaction boundary.
struct D3D12SculptPreviewArtifact {
    std::string project_namespace;
    std::uint64_t mesh_asset = 0U;
    production::BakeMeshBinding source;
    std::string stroke_identity;
    std::string algorithm{ kD3D12SculptPreviewAlgorithm };
    std::string evidence_class{ kD3D12SculptPreviewEvidenceClass };
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest kernel_input_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t byte_size = 0U;
    double maximum_displacement = 0.0;
    double maximum_reference_error = 0.0;
    double maximum_reference_error_tolerance = 0.0;
    bool non_authoritative = true;
    std::vector<SculptPreviewPosition> positions;

    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<void> validate(
        const production::SculptStack& stack,
        const geometry::EditableMesh& base_mesh) const;
};

// The first accelerated preview slice supports exactly one active add,
// subtract, or inflate stroke using surface normals, masks and surface
// anchors. Unsupported combinations fail closed. The CPU reference is run to
// measure parity; neither path mutates base_mesh or creates committed truth.
[[nodiscard]] core::Result<D3D12SculptPreviewArtifact> run_d3d12_sculpt_preview(
    D3D12Device& device,
    const production::SculptStack& stack,
    const geometry::EditableMesh& base_mesh);

} // namespace carto::d3d12
