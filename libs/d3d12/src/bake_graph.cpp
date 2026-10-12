#include <carto/d3d12/bake_graph.hpp>

#include "ambient_occlusion_projection.hpp"
#include "normal_map_projection.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::d3d12 {
namespace {

using SourceTriangleIdentity = std::pair<std::uint64_t, std::uint32_t>;
using AmbientOcclusionRayIdentity =
    std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>;

std::string hex_text(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

template <typename Report>
void add_error(
    Report& report,
    std::string code,
    std::string message) {
    report.artifact.reset();
    production::BakeDiagnostic diagnostic{
        std::move(code), std::move(message), true, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt};
    constexpr std::size_t maximum_diagnostics = 64U;
    if (report.diagnostics.size() < maximum_diagnostics) {
        report.diagnostics.push_back(std::move(diagnostic));
    } else if (report.diagnostics.back().code == "diagnostics_truncated") {
        report.diagnostics.back().is_error = true;
    } else {
        report.diagnostics.back() = production::BakeDiagnostic{
            "diagnostics_truncated",
            "additional bake diagnostics were omitted after the bounded report limit",
            true, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    }
}

} // namespace

core::Result<std::string> d3d12_bake_platform_profile(
    const D3D12ProjectionReport& worker) {
    if (worker.worker_version != kD3D12ProjectionWorkerVersion ||
        worker.stable_adapter_id.empty() || worker.driver.empty() ||
        worker.compiler_invocation_digest.empty() || worker.shader_source_digest.is_zero() ||
        worker.shader_binary_digest.is_zero()) {
        return core::Result<std::string>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 worker lacks stable adapter, driver, compiler, or shader identity"));
    }
    const std::string canonical =
        "CARTO_D3D12_BAKE_PLATFORM 1\nVERSION " +
        std::to_string(worker.worker_version) + "\nADAPTER " +
        hex_text(worker.stable_adapter_id) + "\nDRIVER " + hex_text(worker.driver) +
        "\nCOMPILER " + hex_text(worker.compiler_invocation_digest) +
        "\nSHADER_SOURCE " + worker.shader_source_digest.hex() +
        "\nSHADER_BINARY " + worker.shader_binary_digest.hex() + "\n";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(canonical.data());
    const assets::Sha256Digest digest = assets::sha256(
        std::span<const std::uint8_t>{bytes, canonical.size()});
    return core::Result<std::string>::success("carto.d3d12." + digest.hex());
}

D3D12NormalMapBakeReport compile_tangent_normal_map_d3d12(
    D3D12Device& device,
    const production::BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    D3D12NormalMapBakeReport result;
    std::optional<D3D12ProjectionReport> projection_evidence;
    result.ray_worker_contract = production::compile_tangent_normal_map_with_ray_worker(
        recipe, source, target,
        [&device, &projection_evidence](
            const production::BakeRayQueryRequest& neutral_request) {
            production::BakeRayQueryReport neutral_report;
            D3D12ProjectionRequest request;
            request.source_digest = neutral_request.recipe.source.content_digest;
            request.target_digest = neutral_request.recipe.target.content_digest;
            request.settings_digest = neutral_request.settings_digest;
            request.backface_policy =
                neutral_request.recipe.backface_policy == production::BakeBackfacePolicy::front_only
                ? ProjectionBackfacePolicy::front_only
                : ProjectionBackfacePolicy::both_sides;
            request.triangles.reserve(neutral_request.triangles.size());
            for (const production::BakeRayQueryTriangle& triangle : neutral_request.triangles) {
                request.triangles.push_back(ProjectionTriangle{
                    triangle.face_id,
                    triangle.fan_index,
                    triangle.positions[0U],
                    triangle.positions[1U],
                    triangle.positions[2U],
                });
            }
            request.rays.reserve(neutral_request.rays.size());
            for (const production::BakeRayQueryRay& ray : neutral_request.rays) {
                if (ray.sample_index > std::numeric_limits<std::uint32_t>::max()) {
                    neutral_report.error = "D3D12 ray sample identity exceeds its protocol range";
                    return neutral_report;
                }
                request.rays.push_back(ProjectionRay{
                    ray.target_face_id,
                    ray.texel_x,
                    ray.texel_y,
                    static_cast<std::uint32_t>(ray.sample_index),
                    ray.origin,
                    ray.direction,
                    ray.maximum_distance,
                });
            }

            const auto projection = run_d3d12_projection_worker(device, request);
            if (!projection) {
                neutral_report.error = projection.error().message;
                return neutral_report;
            }
            projection_evidence = projection.value();
            const D3D12ProjectionReport& evidence = *projection_evidence;
            if (evidence.worker_version != kD3D12ProjectionWorkerVersion ||
                evidence.source_digest != request.source_digest ||
                evidence.target_digest != request.target_digest ||
                evidence.settings_digest != request.settings_digest ||
                evidence.request_digest.is_zero() ||
                !evidence.output_digest_matches() ||
                !evidence.evidence_digest_matches() ||
                evidence.shader_source_digest.is_zero() ||
                evidence.shader_binary_digest.is_zero() ||
                evidence.compiler_invocation_digest.empty() ||
                evidence.stable_adapter_id.empty() || evidence.driver.empty() ||
                evidence.hits.size() != neutral_request.rays.size()) {
                neutral_report.error =
                    "D3D12 worker report is incomplete or does not match its submitted request";
                return neutral_report;
            }

            neutral_report.worker_version = evidence.worker_version;
            neutral_report.evidence_class = device.is_software_adapter()
                ? production::BakeRayEvidenceClass::software
                : production::BakeRayEvidenceClass::hardware;
            neutral_report.worker_id = evidence.stable_adapter_id;
            neutral_report.elapsed_nanoseconds = evidence.wall_time_nanoseconds;
            neutral_report.success = true;
            const std::string worker_build =
                "CARTO_D3D12_RAY_WORKER_BUILD 1\nVERSION " +
                std::to_string(evidence.worker_version) + "\nDRIVER " +
                hex_text(evidence.driver) + "\nCOMPILER " +
                hex_text(evidence.compiler_invocation_digest) + "\nSHADER_SOURCE " +
                evidence.shader_source_digest.hex() + "\nSHADER_BINARY " +
                evidence.shader_binary_digest.hex() + "\n";
            const auto* worker_build_bytes =
                reinterpret_cast<const std::uint8_t*>(worker_build.data());
            neutral_report.worker_build_digest = assets::sha256(
                std::span<const std::uint8_t>{worker_build_bytes, worker_build.size()});

            neutral_report.outcomes.reserve(evidence.hits.size());
            for (std::size_t index = 0U; index < evidence.hits.size(); ++index) {
                const production::BakeRayQueryRay& ray = neutral_request.rays[index];
                const ProjectionHit& hit = evidence.hits[index];
                if (hit.target_face_id != ray.target_face_id ||
                    hit.texel_x != ray.texel_x || hit.texel_y != ray.texel_y ||
                    hit.sample_index != ray.sample_index) {
                    neutral_report.success = false;
                    neutral_report.error =
                        "D3D12 worker reordered or changed a canonical ray identity";
                    neutral_report.outcomes.clear();
                    return neutral_report;
                }
                production::BakeRayQueryOutcome outcome;
                outcome.sample_index = hit.sample_index;
                outcome.target_face_id = hit.target_face_id;
                outcome.texel_x = hit.texel_x;
                outcome.texel_y = hit.texel_y;
                outcome.hit = hit.hit;
                outcome.backface = hit.backface;
                if (hit.hit) {
                    outcome.source_face_id = hit.source_face_id;
                    outcome.source_fan_index = hit.source_fan_index;
                    outcome.distance = hit.distance;
                    outcome.barycentric = {
                        1.0 - static_cast<double>(hit.barycentric_u) -
                            static_cast<double>(hit.barycentric_v),
                        static_cast<double>(hit.barycentric_u),
                        static_cast<double>(hit.barycentric_v),
                    };
                }
                if (hit.has_rejected_backface) {
                    outcome.rejected_backface = std::pair{
                        hit.rejected_source_face_id,
                        hit.rejected_source_fan_index};
                }
                neutral_report.outcomes.push_back(std::move(outcome));
            }
            if (auto binding = production::refresh_bake_ray_query_report(
                    neutral_request, neutral_report); !binding) {
                neutral_report.success = false;
                neutral_report.error = binding.error().message;
            }
            return neutral_report;
        });
    result.bake = result.ray_worker_contract->bake;
    if (projection_evidence.has_value()) {
        result.projection = std::move(projection_evidence);
    }
    if (!result.ray_worker_contract->cpu_reference_artifact_digest.is_zero()) {
        result.cpu_reference_artifact_digest =
            result.ray_worker_contract->cpu_reference_artifact_digest;
    }
    return result;
}

D3D12AmbientOcclusionBakeReport compile_ambient_occlusion_d3d12(
    D3D12Device& device,
    const production::AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    D3D12AmbientOcclusionBakeReport result;
    auto work = production::detail::prepare_ambient_occlusion_projection_work(
        recipe, source, target);
    const bool has_validation_errors = std::any_of(
        work.validation.diagnostics.begin(), work.validation.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    if (has_validation_errors) {
        result.bake = std::move(work.validation);
        return result;
    }
    if (work.rays.empty() || work.source_triangles.empty()) {
        result.bake = std::move(work.validation);
        add_error(result.bake, "d3d12_ao_projection_work_empty",
            "canonical AO rasterization produced no projection rays or source triangles");
        return result;
    }

    const auto settings_digest = recipe.canonical_digest();
    if (!settings_digest) {
        result.bake = std::move(work.validation);
        add_error(result.bake, "d3d12_ao_recipe_invalid", settings_digest.error().message);
        return result;
    }
    D3D12ProjectionRequest request;
    request.source_digest = recipe.binding.source.content_digest;
    request.target_digest = recipe.binding.target.content_digest;
    request.settings_digest = settings_digest.value();
    request.backface_policy = ProjectionBackfacePolicy::both_sides;
    request.triangles.reserve(work.source_triangles.size());
    std::map<SourceTriangleIdentity, std::size_t> source_indices;
    for (std::size_t index = 0U; index < work.source_triangles.size(); ++index) {
        const auto& triangle = work.source_triangles[index];
        request.triangles.push_back(ProjectionTriangle{
            triangle.face_id,
            triangle.fan_index,
            triangle.positions[0U],
            triangle.positions[1U],
            triangle.positions[2U],
        });
        if (!source_indices.emplace(
                SourceTriangleIdentity{triangle.face_id, triangle.fan_index}, index).second) {
            result.bake = std::move(work.validation);
            add_error(result.bake, "d3d12_ao_source_identity_collision",
                "canonical AO source snapshot contains duplicate triangle identities");
            return result;
        }
    }

    std::map<AmbientOcclusionRayIdentity, std::size_t> ray_indices;
    std::vector<std::size_t> ordered_rays;
    ordered_rays.reserve(work.rays.size());
    request.rays.reserve(work.rays.size());
    for (std::size_t index = 0U; index < work.rays.size(); ++index) {
        const auto& ray = work.rays[index];
        const AmbientOcclusionRayIdentity identity{
            ray.target_face.value, ray.texel_y, ray.texel_x, ray.sample_index};
        if (!ray_indices.emplace(identity, index).second) {
            result.bake = std::move(work.validation);
            add_error(result.bake, "d3d12_ao_ray_identity_collision",
                "canonical AO rasterization produced duplicate ray identities");
            return result;
        }
        ordered_rays.push_back(index);
    }
    std::stable_sort(ordered_rays.begin(), ordered_rays.end(),
        [&work](std::size_t left, std::size_t right) {
            const auto& a = work.rays[left];
            const auto& b = work.rays[right];
            return AmbientOcclusionRayIdentity{
                a.target_face.value, a.texel_y, a.texel_x, a.sample_index} <
                AmbientOcclusionRayIdentity{
                    b.target_face.value, b.texel_y, b.texel_x, b.sample_index};
        });
    for (const std::size_t index : ordered_rays) {
        const auto& ray = work.rays[index];
        request.rays.push_back(ProjectionRay{
            ray.target_face.value,
            ray.texel_x,
            ray.texel_y,
            ray.sample_index,
            ray.origin,
            ray.direction,
            recipe.max_distance,
        });
    }

    const auto projection = run_d3d12_projection_worker(device, request);
    if (!projection) {
        result.bake = std::move(work.validation);
        add_error(result.bake, "d3d12_ao_projection_failed", projection.error().message);
        return result;
    }
    result.projection = projection.value();
    if (projection.value().hits.size() != request.rays.size()) {
        result.bake = std::move(work.validation);
        add_error(result.bake, "d3d12_ao_projection_result_count_invalid",
            "D3D12 AO worker returned a different hit count than the submitted ray count");
        return result;
    }

    std::vector<std::uint8_t> hit_flags(work.rays.size(), 0U);
    for (const ProjectionHit& hit : projection.value().hits) {
        const AmbientOcclusionRayIdentity identity{
            hit.target_face_id, hit.texel_y, hit.texel_x, hit.sample_index};
        const auto ray_index = ray_indices.find(identity);
        if (ray_index == ray_indices.end()) {
            result.bake = std::move(work.validation);
            add_error(result.bake, "d3d12_ao_projection_identity_invalid",
                "D3D12 AO worker returned a result with an unknown target ray identity");
            return result;
        }
        if (hit.hit && !source_indices.contains(
                SourceTriangleIdentity{hit.source_face_id, hit.source_fan_index})) {
            result.bake = std::move(work.validation);
            add_error(result.bake, "d3d12_ao_source_identity_invalid",
                "D3D12 AO worker selected a source triangle outside the bake snapshot");
            return result;
        }
        hit_flags[ray_index->second] = hit.hit ? 1U : 0U;
    }

    result.bake = production::detail::compile_ambient_occlusion_from_projection(
        std::move(work), hit_flags);
    const production::AmbientOcclusionExecutionReport reference =
        production::compile_ambient_occlusion_cpu(recipe, source, target);
    if (result.bake.succeeded() && !reference.succeeded()) {
        add_error(result.bake, "d3d12_ao_cpu_reference_rejected",
            "GPU AO bake produced an artifact while the CPU reference rejected the same inputs");
        return result;
    }
    if (!result.bake.succeeded() && reference.succeeded()) {
        add_error(result.bake, "d3d12_ao_cpu_reference_status_mismatch",
            "GPU AO bake rejected inputs accepted by the CPU reference");
        return result;
    }
    if (!result.bake.succeeded()) return result;

    result.cpu_reference_artifact_digest = reference.artifact->artifact_digest;
    if (result.bake.artifact->artifact_digest != reference.artifact->artifact_digest) {
        add_error(result.bake, "d3d12_ao_cpu_reference_artifact_mismatch",
            "GPU AO output differs from the canonical CPU bake artifact");
    }
    return result;
}

core::Result<void> publish_ambient_occlusion_map_d3d12(
    production::ProductionArtifactStore& store,
    production::ProductionArtifactReceipt receipt,
    const D3D12AmbientOcclusionBakeReport& report,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const production::AdmissionPolicy& policy,
    std::string platform_profile) {
    if (!report.succeeded()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 AO report lacks verified CPU parity or worker provenance"));
    }
    const production::AmbientOcclusionMapArtifact& artifact = *report.bake.artifact;
    const auto worker_profile = d3d12_bake_platform_profile(*report.projection);
    if (!worker_profile || worker_profile.value() != platform_profile) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 AO receipt profile does not bind the reported adapter and shader"));
    }
    if (auto validation = artifact.validate(); !validation) return validation;
    if (auto validation = production::validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !validation) {
        return validation;
    }
    const auto reference = production::compile_ambient_occlusion_cpu(
        artifact.recipe, current_source, current_target);
    if (!reference.succeeded() || !reference.artifact.has_value() ||
        reference.artifact->artifact_digest != artifact.artifact_digest ||
        report.cpu_reference_artifact_digest != artifact.artifact_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 AO output no longer matches the deterministic CPU reference"));
    }
    const auto expected_key = production::bake_ambient_occlusion_cook_key(
        artifact.recipe, platform_profile);
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 AO receipt key differs from its source, target, or worker profile"));
    }
    return production::publish_ambient_occlusion_map(store, std::move(receipt), artifact,
        current_source, current_target, policy, std::move(platform_profile));
}

core::Result<void> publish_tangent_normal_map_d3d12(
    production::ProductionArtifactStore& store,
    production::ProductionArtifactReceipt receipt,
    const D3D12NormalMapBakeReport& report,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const production::AdmissionPolicy& policy,
    std::string platform_profile) {
    if (!report.succeeded()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 normal-map report lacks verified CPU parity or worker provenance"));
    }
    const production::NormalMapArtifact& artifact = *report.bake.artifact;
    const auto worker_profile = d3d12_bake_platform_profile(*report.projection);
    if (!worker_profile || worker_profile.value() != platform_profile) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 normal-map receipt profile does not bind the reported adapter and shader"));
    }
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = production::validate_bake_sources(
            artifact.recipe, current_source, current_target); !result) {
        return result;
    }
    if (!report.ray_worker_contract.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 normal-map report lacks the neutral ray-worker contract"));
    }
    if (auto result = production::validate_tangent_normal_map_ray_worker_result(
            *report.ray_worker_contract, current_source, current_target); !result) {
        return result;
    }
    if (report.cpu_reference_artifact_digest != artifact.artifact_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 normal-map output no longer matches its sealed CPU reference"));
    }
    const auto expected_key = production::bake_production_cook_key(
        artifact.recipe, production::BakeNormalSpace::tangent, platform_profile);
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 normal-map receipt key differs from its source, target, or worker profile"));
    }
    return production::publish_normal_map(store, std::move(receipt), artifact,
        current_source, current_target, policy, std::move(platform_profile));
}

} // namespace carto::d3d12
