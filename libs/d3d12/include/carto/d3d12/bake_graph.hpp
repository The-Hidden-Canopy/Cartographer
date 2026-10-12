#pragma once

#include <carto/d3d12/projection_worker.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/production/bake_ray_worker.hpp>

#include <optional>

namespace carto::d3d12 {

struct D3D12AmbientOcclusionBakeReport {
    production::AmbientOcclusionExecutionReport bake;
    std::optional<D3D12ProjectionReport> projection;
    std::optional<assets::Sha256Digest> cpu_reference_artifact_digest;

    [[nodiscard]] bool succeeded() const noexcept {
        if (!bake.succeeded() || !bake.artifact.has_value() || !projection.has_value() ||
            !cpu_reference_artifact_digest.has_value()) {
            return false;
        }
        const production::AmbientOcclusionMapArtifact& artifact = *bake.artifact;
        const D3D12ProjectionReport& worker = *projection;
        return !artifact.settings_digest.is_zero() &&
            artifact.artifact_digest == *cpu_reference_artifact_digest &&
            worker.worker_version == kD3D12ProjectionWorkerVersion &&
            worker.source_digest == artifact.recipe.binding.source.content_digest &&
            worker.target_digest == artifact.recipe.binding.target.content_digest &&
            worker.settings_digest == artifact.settings_digest &&
            worker.output_digest_matches() &&
            worker.evidence_digest_matches() &&
            !worker.request_digest.is_zero() && !worker.output_digest.is_zero() &&
            !worker.shader_source_digest.is_zero() && !worker.shader_binary_digest.is_zero() &&
            !worker.compiler_invocation_digest.empty() && !worker.adapter_name.empty() &&
            !worker.stable_adapter_id.empty() && !worker.driver.empty();
    }
};

struct D3D12NormalMapBakeReport {
    production::BakeExecutionReport bake;
    std::optional<D3D12ProjectionReport> projection;
    std::optional<assets::Sha256Digest> cpu_reference_artifact_digest;
    std::optional<production::BakeRayQueryCompileResult> ray_worker_contract;

    [[nodiscard]] bool succeeded() const noexcept {
        if (!bake.succeeded() || !bake.artifact.has_value() || !projection.has_value() ||
            !cpu_reference_artifact_digest.has_value() || !ray_worker_contract.has_value() ||
            !ray_worker_contract->succeeded() ||
            !ray_worker_contract->bake.artifact.has_value()) {
            return false;
        }
        const production::NormalMapArtifact& artifact = *bake.artifact;
        const D3D12ProjectionReport& worker = *projection;
        return artifact.normal_space == production::BakeNormalSpace::tangent &&
            artifact.artifact_digest == *cpu_reference_artifact_digest &&
            worker.worker_version == kD3D12ProjectionWorkerVersion &&
            worker.source_digest == artifact.recipe.source.content_digest &&
            worker.target_digest == artifact.recipe.target.content_digest &&
            worker.settings_digest == artifact.settings_digest &&
            worker.output_digest_matches() &&
            worker.evidence_digest_matches() &&
            !worker.request_digest.is_zero() && !worker.output_digest.is_zero() &&
            !worker.shader_source_digest.is_zero() && !worker.shader_binary_digest.is_zero() &&
            !worker.compiler_invocation_digest.empty() && !worker.adapter_name.empty() &&
            !worker.stable_adapter_id.empty() && !worker.driver.empty() &&
            ray_worker_contract->bake.artifact->artifact_digest == artifact.artifact_digest &&
            ray_worker_contract->cpu_reference_artifact_digest == artifact.artifact_digest;
    }
};

[[nodiscard]] core::Result<std::string> d3d12_bake_platform_profile(
    const D3D12ProjectionReport& worker);

// Runs the projection on D3D12, builds the normal-map artifact from those GPU
// hits, and publishes it only when its canonical digest matches the CPU reference.
[[nodiscard]] D3D12NormalMapBakeReport compile_tangent_normal_map_d3d12(
    D3D12Device& device,
    const production::BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);

// Runs canonical AO rays on D3D12, reconstructs the RGBA8 artifact through the
// CPU reference encoder, and accepts it only when the artifact digest matches
// the deterministic CPU reference.
[[nodiscard]] D3D12AmbientOcclusionBakeReport compile_ambient_occlusion_d3d12(
    D3D12Device& device,
    const production::AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target);

// Publishes a D3D12 result only after rechecking its worker provenance, current
// mesh bindings, and deterministic CPU artifact parity through the same
// production receipt and content-addressed storage boundary as CPU bakes.
[[nodiscard]] core::Result<void> publish_tangent_normal_map_d3d12(
    production::ProductionArtifactStore& store,
    production::ProductionArtifactReceipt receipt,
    const D3D12NormalMapBakeReport& report,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const production::AdmissionPolicy& policy,
    std::string platform_profile = "carto.d3d12.reference-verified.v1");

[[nodiscard]] core::Result<void> publish_ambient_occlusion_map_d3d12(
    production::ProductionArtifactStore& store,
    production::ProductionArtifactReceipt receipt,
    const D3D12AmbientOcclusionBakeReport& report,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const production::AdmissionPolicy& policy,
    std::string platform_profile = "carto.d3d12.reference-verified.v1");

} // namespace carto::d3d12
