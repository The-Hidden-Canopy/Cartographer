#include <carto/d3d12/bake_graph.hpp>

#include "ambient_occlusion_projection.hpp"
#include "bent_normal_validation.hpp"
#include "normal_map_projection.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

template <typename TileBakeReport>
core::Diagnostic failed_tile_schedule(
    std::string_view map_name,
    production::BakeTileCoordinate tile,
    const TileBakeReport& report) {
    const auto diagnostic = std::find_if(
        report.bake.diagnostics.begin(), report.bake.diagnostics.end(),
        [](const production::BakeDiagnostic& value) { return value.is_error; });
    const std::string detail = diagnostic == report.bake.diagnostics.end()
        ? "D3D12 tile compiler failed without an error diagnostic"
        : diagnostic->code + ": " + diagnostic->message;
    return core::Diagnostic(core::ErrorCode::validation_failed,
        std::string(map_name) + " D3D12 tile (" + std::to_string(tile.tile_u) +
            "," + std::to_string(tile.tile_v) + ") failed: " + detail);
}

core::Diagnostic cancelled_tile_schedule(std::string_view map_name) {
    return core::Diagnostic(core::ErrorCode::invalid_state,
        std::string(map_name) +
            " D3D12 tile schedule cancelled at a tile boundary; no schedule was returned");
}

struct AmbientOcclusionD3D12Projection {
    production::detail::AmbientOcclusionProjectionWork work;
    std::optional<D3D12ProjectionReport> report;
    std::vector<std::uint8_t> hit_flags;
    bool succeeded = false;
};

AmbientOcclusionD3D12Projection project_ambient_occlusion_d3d12(
    D3D12Device& device,
    const production::AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    AmbientOcclusionD3D12Projection result;
    result.work = production::detail::prepare_ambient_occlusion_projection_work(
        recipe, source, target);
    const bool has_validation_errors = std::any_of(
        result.work.validation.diagnostics.begin(),
        result.work.validation.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    if (has_validation_errors) return result;
    if (result.work.rays.empty() || result.work.source_triangles.empty()) {
        add_error(result.work.validation, "d3d12_ao_projection_work_empty",
            "canonical AO rasterization produced no projection rays or source triangles");
        return result;
    }

    const auto settings_digest = recipe.canonical_digest();
    if (!settings_digest) {
        add_error(result.work.validation, "d3d12_ao_recipe_invalid",
            settings_digest.error().message);
        return result;
    }
    D3D12ProjectionRequest request;
    request.source_digest = recipe.binding.source.content_digest;
    request.target_digest = recipe.binding.target.content_digest;
    request.settings_digest = settings_digest.value();
    request.backface_policy = ProjectionBackfacePolicy::both_sides;
    request.triangles.reserve(result.work.source_triangles.size());
    std::map<SourceTriangleIdentity, std::size_t> source_indices;
    for (std::size_t index = 0U; index < result.work.source_triangles.size(); ++index) {
        const auto& triangle = result.work.source_triangles[index];
        request.triangles.push_back(ProjectionTriangle{
            triangle.face_id,
            triangle.fan_index,
            triangle.positions[0U],
            triangle.positions[1U],
            triangle.positions[2U],
        });
        if (!source_indices.emplace(
                SourceTriangleIdentity{triangle.face_id, triangle.fan_index}, index).second) {
            add_error(result.work.validation, "d3d12_ao_source_identity_collision",
                "canonical AO source snapshot contains duplicate triangle identities");
            return result;
        }
    }

    std::map<AmbientOcclusionRayIdentity, std::size_t> ray_indices;
    std::vector<std::size_t> ordered_rays;
    ordered_rays.reserve(result.work.rays.size());
    request.rays.reserve(result.work.rays.size());
    for (std::size_t index = 0U; index < result.work.rays.size(); ++index) {
        const auto& ray = result.work.rays[index];
        const AmbientOcclusionRayIdentity identity{
            ray.target_face.value, ray.texel_y, ray.texel_x, ray.sample_index};
        if (!ray_indices.emplace(identity, index).second) {
            add_error(result.work.validation, "d3d12_ao_ray_identity_collision",
                "canonical AO rasterization produced duplicate ray identities");
            return result;
        }
        ordered_rays.push_back(index);
    }
    std::stable_sort(ordered_rays.begin(), ordered_rays.end(),
        [&result](std::size_t left, std::size_t right) {
            const auto& a = result.work.rays[left];
            const auto& b = result.work.rays[right];
            return AmbientOcclusionRayIdentity{
                a.target_face.value, a.texel_y, a.texel_x, a.sample_index} <
                AmbientOcclusionRayIdentity{
                    b.target_face.value, b.texel_y, b.texel_x, b.sample_index};
        });
    for (const std::size_t index : ordered_rays) {
        const auto& ray = result.work.rays[index];
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

    auto projection = run_d3d12_projection_worker(device, request);
    if (!projection) {
        add_error(result.work.validation, "d3d12_ao_projection_failed",
            projection.error().message);
        return result;
    }
    result.report = std::move(projection.value());
    const D3D12ProjectionReport& evidence = *result.report;
    if (evidence.worker_version != kD3D12ProjectionWorkerVersion ||
        evidence.source_digest != request.source_digest ||
        evidence.target_digest != request.target_digest ||
        evidence.settings_digest != request.settings_digest ||
        evidence.request_digest.is_zero() ||
        !evidence.output_digest_matches() || !evidence.evidence_digest_matches() ||
        evidence.shader_source_digest.is_zero() ||
        evidence.shader_binary_digest.is_zero() ||
        evidence.compiler_invocation_digest.empty() || evidence.adapter_name.empty() ||
        evidence.stable_adapter_id.empty() || evidence.driver.empty()) {
        add_error(result.work.validation, "d3d12_ao_projection_evidence_invalid",
            "D3D12 worker report is incomplete or differs from the submitted ray request");
        return result;
    }
    if (evidence.hits.size() != request.rays.size()) {
        add_error(result.work.validation, "d3d12_ao_projection_result_count_invalid",
            "D3D12 AO worker returned a different hit count than the submitted ray count");
        return result;
    }

    result.hit_flags.assign(result.work.rays.size(), 0U);
    std::vector<bool> seen(result.work.rays.size(), false);
    for (const ProjectionHit& hit : evidence.hits) {
        const AmbientOcclusionRayIdentity identity{
            hit.target_face_id, hit.texel_y, hit.texel_x, hit.sample_index};
        const auto ray_index = ray_indices.find(identity);
        if (ray_index == ray_indices.end() || seen[ray_index->second]) {
            add_error(result.work.validation, "d3d12_ao_projection_identity_invalid",
                "D3D12 AO worker returned an unknown or duplicate target ray identity");
            return result;
        }
        seen[ray_index->second] = true;
        if (hit.hit && !source_indices.contains(
                SourceTriangleIdentity{hit.source_face_id, hit.source_fan_index})) {
            add_error(result.work.validation, "d3d12_ao_source_identity_invalid",
                "D3D12 AO worker selected a source triangle outside the bake snapshot");
            return result;
        }
        result.hit_flags[ray_index->second] = hit.hit ? 1U : 0U;
    }
    result.succeeded = true;
    return result;
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
    AmbientOcclusionD3D12Projection projection = project_ambient_occlusion_d3d12(
        device, recipe, source, target);
    result.projection = std::move(projection.report);
    if (!projection.succeeded) {
        result.bake = std::move(projection.work.validation);
        return result;
    }
    result.bake = production::detail::compile_ambient_occlusion_from_projection(
        std::move(projection.work), projection.hit_flags);
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

D3D12BentNormalBakeReport compile_bent_normal_d3d12(
    D3D12Device& device,
    const production::AmbientOcclusionRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target) {
    D3D12BentNormalBakeReport result;
    // The AO and bent-normal maps share canonical rays. Reconstruct each map
    // separately from the same checked D3D12 projection evidence.
    AmbientOcclusionD3D12Projection projection = project_ambient_occlusion_d3d12(
        device, recipe, source, target);
    result.projection = std::move(projection.report);
    if (!projection.succeeded) {
        result.bake = detail::bent_normal_validation_failure(
            std::move(projection.work.validation));
        if (result.bake.diagnostics.empty()) {
            add_error(result.bake, "d3d12_bent_normal_projection_failed",
                "D3D12 bent-normal projection did not produce verified ray evidence");
        }
        return result;
    }
    result.bake = production::detail::compile_bent_normal_from_projection(
        recipe, source, target, projection.work.rays, projection.hit_flags);
    if (!result.bake.succeeded()) return result;

    const production::BentNormalExecutionReport reference =
        production::compile_bent_normal_cpu(recipe, source, target);
    if (!reference.succeeded() || !reference.artifact.has_value()) {
        add_error(result.bake, "d3d12_bent_normal_cpu_reference_rejected",
            "D3D12 bent-normal bake produced an artifact while the CPU reference rejected the same inputs");
        return result;
    }
    result.cpu_reference_artifact_digest = reference.artifact->artifact_digest;
    if (result.bake.artifact->artifact_digest != reference.artifact->artifact_digest) {
        add_error(result.bake, "d3d12_bent_normal_cpu_reference_artifact_mismatch",
            "D3D12 bent-normal output differs from the canonical CPU bake artifact");
    }
    return result;
}

core::Result<D3D12NormalMapTileScheduleResult>
schedule_tangent_normal_map_tiles_d3d12(
    D3D12Device& device,
    const production::BakeRecipe& base_recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token) {
    using Output = core::Result<D3D12NormalMapTileScheduleResult>;
    if (stop_token.stop_requested()) {
        return Output::failure(cancelled_tile_schedule("normal-map"));
    }
    if (auto result = base_recipe.validate(); !result) {
        return Output::failure(result.error());
    }
    if (auto result = production::validate_bake_sources(base_recipe, source, target); !result) {
        return Output::failure(result.error());
    }
    const auto tiles = production::enumerate_bake_tiles(
        target, base_recipe.target_uv_set);
    if (!tiles) return Output::failure(tiles.error());
    if (tiles.value().empty() ||
        tiles.value().size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "D3D12 normal-map tile schedule is empty or exceeds its tile bound"));
    }
    const std::uint64_t tile_texels =
        static_cast<std::uint64_t>(base_recipe.width) * base_recipe.height;
    const std::uint64_t tile_bytes = tile_texels * 4ULL;
    if (tile_bytes == 0U || tiles.value().size() >
            production::kMaxBakeTileScheduleRgbaBytes / tile_bytes) {
        return Output::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "D3D12 normal-map schedule exceeds its aggregate RGBA8 budget"));
    }

    D3D12NormalMapTileScheduleResult schedule;
    schedule.entries.reserve(tiles.value().size());
    for (const production::BakeTileCoordinate tile : tiles.value()) {
        if (stop_token.stop_requested()) {
            return Output::failure(cancelled_tile_schedule("normal-map"));
        }
        production::BakeRecipe tile_recipe = base_recipe;
        tile_recipe.tile_u = tile.tile_u;
        tile_recipe.tile_v = tile.tile_v;
        tile_recipe.identity = base_recipe.identity + "/tile/" +
            std::to_string(tile.tile_u) + "/" + std::to_string(tile.tile_v);
        if (auto result = tile_recipe.validate(); !result) {
            return Output::failure(result.error().with_context(
                "D3D12 UDIM tile " + std::to_string(tile.tile_u) + "," +
                std::to_string(tile.tile_v)));
        }
        const auto started = std::chrono::steady_clock::now();
        D3D12NormalMapBakeReport bake = compile_tangent_normal_map_d3d12(
            device, tile_recipe, source, target);
        const auto finished = std::chrono::steady_clock::now();
        if (!bake.succeeded()) {
            return Output::failure(failed_tile_schedule("normal-map", tile, bake));
        }
        if (stop_token.stop_requested()) {
            return Output::failure(cancelled_tile_schedule("normal-map"));
        }
        const double compile_milliseconds = std::chrono::duration<double, std::milli>(
            finished - started).count();
        if (!std::isfinite(compile_milliseconds) || compile_milliseconds < 0.0 ||
            compile_milliseconds > std::numeric_limits<double>::max() -
                schedule.compile_milliseconds) {
            return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "D3D12 normal-map schedule compile duration is invalid"));
        }
        schedule.compile_milliseconds += compile_milliseconds;
        schedule.entries.push_back(D3D12NormalMapTileScheduleEntry{
            tile, std::move(bake), compile_milliseconds});
    }
    if (stop_token.stop_requested()) {
        return Output::failure(cancelled_tile_schedule("normal-map"));
    }
    if (auto result = production::validate_bake_sources(base_recipe, source, target); !result) {
        return Output::failure(result.error().with_context(
            "revalidating D3D12 normal-map sources after tile scheduling"));
    }
    return Output::success(std::move(schedule));
}

core::Result<D3D12AmbientOcclusionTileScheduleResult>
schedule_ambient_occlusion_tiles_d3d12(
    D3D12Device& device,
    const production::AmbientOcclusionRecipe& base_recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token) {
    using Output = core::Result<D3D12AmbientOcclusionTileScheduleResult>;
    if (stop_token.stop_requested()) {
        return Output::failure(cancelled_tile_schedule("ambient-occlusion"));
    }
    if (auto result = base_recipe.validate(); !result) {
        return Output::failure(result.error());
    }
    if (auto result = production::validate_bake_sources(
            base_recipe.binding, source, target); !result) {
        return Output::failure(result.error());
    }
    const auto tiles = production::enumerate_bake_tiles(
        target, base_recipe.binding.target_uv_set);
    if (!tiles) return Output::failure(tiles.error());
    if (tiles.value().empty() ||
        tiles.value().size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "D3D12 AO tile schedule is empty or exceeds its tile bound"));
    }
    const std::uint64_t tile_texels =
        static_cast<std::uint64_t>(base_recipe.binding.width) *
        base_recipe.binding.height;
    const std::uint64_t tile_bytes = tile_texels * 4ULL;
    const std::uint64_t tile_rays = tile_texels * base_recipe.sample_count;
    if (tile_bytes == 0U || tile_rays == 0U ||
        tiles.value().size() > production::kMaxBakeTileScheduleRgbaBytes / tile_bytes ||
        tiles.value().size() > production::kMaxBakeAmbientOcclusionRays / tile_rays) {
        return Output::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "D3D12 AO schedule exceeds its aggregate output or ray budget"));
    }

    D3D12AmbientOcclusionTileScheduleResult schedule;
    schedule.entries.reserve(tiles.value().size());
    for (const production::BakeTileCoordinate tile : tiles.value()) {
        if (stop_token.stop_requested()) {
            return Output::failure(cancelled_tile_schedule("ambient-occlusion"));
        }
        production::AmbientOcclusionRecipe tile_recipe = base_recipe;
        tile_recipe.binding.tile_u = tile.tile_u;
        tile_recipe.binding.tile_v = tile.tile_v;
        tile_recipe.binding.identity = base_recipe.binding.identity + "/tile/" +
            std::to_string(tile.tile_u) + "/" + std::to_string(tile.tile_v);
        if (auto result = tile_recipe.validate(); !result) {
            return Output::failure(result.error().with_context(
                "D3D12 AO UDIM tile " + std::to_string(tile.tile_u) + "," +
                std::to_string(tile.tile_v)));
        }
        const auto started = std::chrono::steady_clock::now();
        D3D12AmbientOcclusionBakeReport bake = compile_ambient_occlusion_d3d12(
            device, tile_recipe, source, target);
        const auto finished = std::chrono::steady_clock::now();
        if (!bake.succeeded()) {
            return Output::failure(failed_tile_schedule("ambient-occlusion", tile, bake));
        }
        if (stop_token.stop_requested()) {
            return Output::failure(cancelled_tile_schedule("ambient-occlusion"));
        }
        const production::AmbientOcclusionMapArtifact& artifact = *bake.bake.artifact;
        const double compile_milliseconds = std::chrono::duration<double, std::milli>(
            finished - started).count();
        if (!std::isfinite(compile_milliseconds) || compile_milliseconds < 0.0 ||
            compile_milliseconds > std::numeric_limits<double>::max() -
                schedule.compile_milliseconds ||
            artifact.covered_texels > std::numeric_limits<std::uint64_t>::max() -
                schedule.covered_texels ||
            artifact.rays_cast > std::numeric_limits<std::uint64_t>::max() -
                schedule.rays_cast) {
            return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "D3D12 AO schedule totals are invalid"));
        }
        schedule.covered_texels += artifact.covered_texels;
        schedule.rays_cast += artifact.rays_cast;
        schedule.compile_milliseconds += compile_milliseconds;
        schedule.entries.push_back(D3D12AmbientOcclusionTileScheduleEntry{
            tile, std::move(bake), compile_milliseconds});
    }
    if (stop_token.stop_requested()) {
        return Output::failure(cancelled_tile_schedule("ambient-occlusion"));
    }
    if (auto result = production::validate_bake_sources(
            base_recipe.binding, source, target); !result) {
        return Output::failure(result.error().with_context(
            "revalidating D3D12 AO sources after tile scheduling"));
    }
    return Output::success(std::move(schedule));
}

core::Result<D3D12BentNormalTileScheduleResult>
schedule_bent_normal_tiles_d3d12(
    D3D12Device& device,
    const production::AmbientOcclusionRecipe& base_recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    std::stop_token stop_token) {
    using Output = core::Result<D3D12BentNormalTileScheduleResult>;
    if (stop_token.stop_requested()) {
        return Output::failure(cancelled_tile_schedule("bent-normal"));
    }
    if (auto result = base_recipe.validate(); !result) {
        return Output::failure(result.error());
    }
    if (auto result = production::validate_bake_sources(
            base_recipe.binding, source, target); !result) {
        return Output::failure(result.error());
    }
    const auto tiles = production::enumerate_bake_tiles(
        target, base_recipe.binding.target_uv_set);
    if (!tiles) return Output::failure(tiles.error());
    if (tiles.value().empty() ||
        tiles.value().size() > production::kMaxBakeTileScheduleTiles) {
        return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
            "D3D12 bent-normal tile schedule is empty or exceeds its tile bound"));
    }
    const std::uint64_t tile_texels =
        static_cast<std::uint64_t>(base_recipe.binding.width) *
        base_recipe.binding.height;
    if (tile_texels == 0U ||
        tile_texels > std::numeric_limits<std::uint64_t>::max() / 4ULL ||
        tile_texels > std::numeric_limits<std::uint64_t>::max() /
            base_recipe.sample_count) {
        return Output::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "D3D12 bent-normal tile size exceeds aggregate accounting limits"));
    }
    const std::uint64_t tile_bytes = tile_texels * 4ULL;
    const std::uint64_t tile_rays = tile_texels * base_recipe.sample_count;
    if (tiles.value().size() > production::kMaxBakeTileScheduleRgbaBytes / tile_bytes ||
        tiles.value().size() > production::kMaxBakeAmbientOcclusionRays / tile_rays) {
        return Output::failure(core::Diagnostic(core::ErrorCode::unsupported,
            "D3D12 bent-normal schedule exceeds its aggregate output or ray budget"));
    }

    D3D12BentNormalTileScheduleResult schedule;
    schedule.entries.reserve(tiles.value().size());
    for (const production::BakeTileCoordinate tile : tiles.value()) {
        if (stop_token.stop_requested()) {
            return Output::failure(cancelled_tile_schedule("bent-normal"));
        }
        production::AmbientOcclusionRecipe tile_recipe = base_recipe;
        tile_recipe.binding.tile_u = tile.tile_u;
        tile_recipe.binding.tile_v = tile.tile_v;
        tile_recipe.binding.identity = base_recipe.binding.identity + "/tile/" +
            std::to_string(tile.tile_u) + "/" + std::to_string(tile.tile_v);
        if (auto result = tile_recipe.validate(); !result) {
            return Output::failure(result.error().with_context(
                "D3D12 bent-normal UDIM tile " + std::to_string(tile.tile_u) + "," +
                std::to_string(tile.tile_v)));
        }
        const auto started = std::chrono::steady_clock::now();
        D3D12BentNormalBakeReport bake = compile_bent_normal_d3d12(
            device, tile_recipe, source, target);
        const auto finished = std::chrono::steady_clock::now();
        if (!bake.succeeded()) {
            return Output::failure(failed_tile_schedule("bent-normal", tile, bake));
        }
        if (stop_token.stop_requested()) {
            return Output::failure(cancelled_tile_schedule("bent-normal"));
        }
        const production::BentNormalMapArtifact& artifact = *bake.bake.artifact;
        const double compile_milliseconds = std::chrono::duration<double, std::milli>(
            finished - started).count();
        if (!std::isfinite(compile_milliseconds) || compile_milliseconds < 0.0 ||
            compile_milliseconds > std::numeric_limits<double>::max() -
                schedule.compile_milliseconds ||
            artifact.covered_texels > std::numeric_limits<std::uint64_t>::max() -
                schedule.covered_texels ||
            artifact.rays_cast > std::numeric_limits<std::uint64_t>::max() -
                schedule.rays_cast ||
            artifact.occluded_rays > std::numeric_limits<std::uint64_t>::max() -
                schedule.occluded_rays) {
            return Output::failure(core::Diagnostic(core::ErrorCode::validation_failed,
                "D3D12 bent-normal schedule totals are invalid"));
        }
        schedule.covered_texels += artifact.covered_texels;
        schedule.rays_cast += artifact.rays_cast;
        schedule.occluded_rays += artifact.occluded_rays;
        schedule.compile_milliseconds += compile_milliseconds;
        schedule.entries.push_back(D3D12BentNormalTileScheduleEntry{
            tile, std::move(bake), compile_milliseconds});
    }
    if (stop_token.stop_requested()) {
        return Output::failure(cancelled_tile_schedule("bent-normal"));
    }
    if (auto result = production::validate_bake_sources(
            base_recipe.binding, source, target); !result) {
        return Output::failure(result.error().with_context(
            "revalidating D3D12 bent-normal sources after tile scheduling"));
    }
    return Output::success(std::move(schedule));
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

core::Result<void> publish_bent_normal_map_d3d12(
    production::ProductionArtifactStore& store,
    production::ProductionArtifactReceipt receipt,
    const D3D12BentNormalBakeReport& report,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const production::AdmissionPolicy& policy,
    std::string platform_profile) {
    if (!report.succeeded()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 bent-normal report lacks verified CPU parity or worker provenance"));
    }
    const production::BentNormalMapArtifact& artifact = *report.bake.artifact;
    const auto worker_profile = d3d12_bake_platform_profile(*report.projection);
    if (!worker_profile || worker_profile.value() != platform_profile) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 bent-normal receipt profile does not bind the reported adapter and shader"));
    }
    if (auto result = artifact.validate(); !result) return result;
    if (auto result = production::validate_bake_sources(
            artifact.recipe.binding, current_source, current_target); !result) {
        return result;
    }
    const production::BentNormalExecutionReport reference =
        production::compile_bent_normal_cpu(
            artifact.recipe, current_source, current_target);
    if (!reference.succeeded() || !reference.artifact.has_value() ||
        reference.artifact->artifact_digest != artifact.artifact_digest ||
        report.cpu_reference_artifact_digest != artifact.artifact_digest) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 bent-normal output no longer matches the deterministic CPU reference"));
    }
    const auto expected_key = production::bake_bent_normal_cook_key(
        artifact.recipe, platform_profile);
    if (!expected_key) return core::Result<void>::failure(expected_key.error());
    if (receipt.key != expected_key.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "D3D12 bent-normal receipt key differs from its source, target, or worker profile"));
    }
    return production::publish_bent_normal_map(store, std::move(receipt), artifact,
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
