#include <carto/production/bake_ray_worker.hpp>

#include "normal_map_projection.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace carto::production {
namespace {

constexpr double kRayGeometryEpsilon = 1e-12;
constexpr double kWorkerDistanceRelativeTolerance = 1e-5;
constexpr double kWorkerBarycentricTolerance = 1e-4;

core::Diagnostic validation_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Result<void> failed_validation(std::string message) {
    return core::Result<void>::failure(validation_error(std::move(message)));
}

void append_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value) {
    bytes.push_back(value);
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_double(std::vector<std::uint8_t>& bytes, double value) {
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_string(std::vector<std::uint8_t>& bytes, std::string_view value) {
    append_u64(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_digest(
    std::vector<std::uint8_t>& bytes,
    const assets::Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.bytes.begin(), digest.bytes.end());
}

void append_vec3(std::vector<std::uint8_t>& bytes, core::Vec3d value) {
    append_double(bytes, value.x);
    append_double(bytes, value.y);
    append_double(bytes, value.z);
}

assets::Sha256Digest hash_bytes(const std::vector<std::uint8_t>& bytes) {
    return assets::sha256(std::span<const std::uint8_t>{bytes});
}

assets::Sha256Digest worker_provenance_digest(const BakeRayQueryReport& report) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(report.worker_id.size() + 96U);
    append_string(bytes, "CARTO_BAKE_RAY_WORKER_PROVENANCE 1");
    append_u32(bytes, report.protocol_version);
    append_u32(bytes, report.worker_version);
    append_u8(bytes, static_cast<std::uint8_t>(report.evidence_class));
    append_string(bytes, report.worker_id);
    append_digest(bytes, report.worker_build_digest);
    return hash_bytes(bytes);
}

bool finite_triangle(const BakeRayQueryTriangle& triangle) {
    return std::all_of(triangle.positions.begin(), triangle.positions.end(),
        [](core::Vec3d position) { return position.finite(); });
}

bool valid_worker_id(std::string_view value) {
    return !value.empty() && value.size() <= 512U &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20U && character != 0x7fU;
        });
}

bool valid_evidence_class(BakeRayEvidenceClass value) {
    return value == BakeRayEvidenceClass::software ||
        value == BakeRayEvidenceClass::hardware ||
        value == BakeRayEvidenceClass::simulated;
}

struct RayTriangleIntersection {
    double distance = 0.0;
    std::array<double, 3U> barycentric{};
    bool front_facing = true;
};

std::optional<RayTriangleIntersection> intersect_triangle(
    const BakeRayQueryRay& ray,
    const BakeRayQueryTriangle& triangle) {
    const core::Vec3d edge_one = triangle.positions[1U] - triangle.positions[0U];
    const core::Vec3d edge_two = triangle.positions[2U] - triangle.positions[0U];
    const core::Vec3d p = core::cross(ray.direction, edge_two);
    const double determinant = core::dot(edge_one, p);
    if (!std::isfinite(determinant) || std::abs(determinant) <= kRayGeometryEpsilon) {
        return std::nullopt;
    }
    const double inverse = 1.0 / determinant;
    const core::Vec3d from_first = ray.origin - triangle.positions[0U];
    const double u = core::dot(from_first, p) * inverse;
    if (u < -kRayGeometryEpsilon || u > 1.0 + kRayGeometryEpsilon) {
        return std::nullopt;
    }
    const core::Vec3d q = core::cross(from_first, edge_one);
    const double v = core::dot(ray.direction, q) * inverse;
    if (v < -kRayGeometryEpsilon || u + v > 1.0 + kRayGeometryEpsilon) {
        return std::nullopt;
    }
    const double distance = core::dot(edge_two, q) * inverse;
    if (!std::isfinite(distance) || distance < 0.0 ||
        distance > ray.maximum_distance) {
        return std::nullopt;
    }
    return RayTriangleIntersection{
        distance,
        {1.0 - u - v, u, v},
        determinant > 0.0};
}

void append_compile_error(
    BakeExecutionReport& report,
    std::string code,
    std::string message) {
    report.artifact.reset();
    constexpr std::size_t maximum_diagnostics = 64U;
    if (report.diagnostics.size() < maximum_diagnostics) {
        report.diagnostics.push_back(BakeDiagnostic{
            std::move(code), std::move(message), true,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    } else if (report.diagnostics.back().code == "diagnostics_truncated") {
        report.diagnostics.back().is_error = true;
    } else {
        report.diagnostics.back() = BakeDiagnostic{
            "diagnostics_truncated",
            "additional ray-worker diagnostics were omitted after the bounded report limit",
            true, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    }
}

core::Result<assets::Sha256Digest> compile_result_digest(
    const BakeRayQueryCompileResult& result) {
    if (!result.bake.artifact.has_value()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation_error("ray-worker compile result has no artifact to seal"));
    }
    const auto request_digest = bake_ray_query_request_digest(result.request);
    const auto artifact_bytes = result.bake.artifact->serialize();
    if (!request_digest || !artifact_bytes) {
        return core::Result<assets::Sha256Digest>::failure(
            !request_digest ? request_digest.error() : artifact_bytes.error());
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(320U + result.bake.diagnostics.size() * 64U);
    append_string(bytes, "CARTO_BAKE_RAY_COMPILE_RESULT 1");
    append_digest(bytes, request_digest.value());
    append_digest(bytes, result.cpu_reference_artifact_digest);
    append_digest(bytes, assets::sha256(artifact_bytes.value()));
    append_digest(bytes, result.worker.worker_provenance_digest);
    append_digest(bytes, result.worker.request_digest);
    append_digest(bytes, result.worker.geometry_digest);
    append_digest(bytes, result.worker.source_digest);
    append_digest(bytes, result.worker.target_digest);
    append_digest(bytes, result.worker.recipe_digest);
    append_digest(bytes, result.worker.settings_digest);
    append_digest(bytes, result.worker.output_digest);
    append_u64(bytes, result.worker.elapsed_nanoseconds);
    append_u64(bytes, result.bake.target_texels_visited);
    append_u64(bytes, result.bake.covered_texels);
    append_u64(bytes, result.bake.projected_texels);
    append_u64(bytes, result.bake.projection_misses);
    append_u64(bytes, result.bake.projection_backface_hits);
    append_u64(bytes, result.bake.projection_backface_rejections);
    append_u64(bytes, static_cast<std::uint64_t>(result.bake.diagnostics.size()));
    for (const BakeDiagnostic& diagnostic : result.bake.diagnostics) {
        append_string(bytes, diagnostic.code);
        append_string(bytes, diagnostic.message);
        append_u8(bytes, diagnostic.is_error ? 1U : 0U);
        append_u8(bytes, diagnostic.target_face.has_value() ? 1U : 0U);
        if (diagnostic.target_face.has_value()) append_u64(bytes, diagnostic.target_face->value);
        append_u8(bytes, diagnostic.source_face.has_value() ? 1U : 0U);
        if (diagnostic.source_face.has_value()) append_u64(bytes, diagnostic.source_face->value);
        append_u8(bytes, diagnostic.texel_x.has_value() ? 1U : 0U);
        if (diagnostic.texel_x.has_value()) append_u32(bytes, *diagnostic.texel_x);
        append_u8(bytes, diagnostic.texel_y.has_value() ? 1U : 0U);
        if (diagnostic.texel_y.has_value()) append_u32(bytes, *diagnostic.texel_y);
    }
    return core::Result<assets::Sha256Digest>::success(hash_bytes(bytes));
}

core::Result<BakeRayQueryRequest> make_request(
    const BakeRecipe& recipe,
    const detail::NormalMapBakeWork& work) {
    BakeRayQueryRequest request;
    request.recipe = recipe;
    request.normal_space = BakeNormalSpace::tangent;
    const auto recipe_digest = recipe.canonical_digest();
    if (!recipe_digest) {
        return core::Result<BakeRayQueryRequest>::failure(recipe_digest.error());
    }
    request.recipe_digest = recipe_digest.value();
    request.settings_digest = work.settings_digest;

    request.triangles.reserve(work.source_triangles.size());
    for (const detail::NormalMapProjectionTriangle& triangle : work.source_triangles) {
        request.triangles.push_back(BakeRayQueryTriangle{
            triangle.face_id, triangle.fan_index, triangle.positions});
    }

    std::vector<std::size_t> ordered_samples(work.samples.size());
    for (std::size_t index = 0U; index < ordered_samples.size(); ++index) {
        ordered_samples[index] = index;
    }
    std::stable_sort(ordered_samples.begin(), ordered_samples.end(),
        [&work](std::size_t left, std::size_t right) {
            const detail::NormalMapProjectionSample& a = work.samples[left];
            const detail::NormalMapProjectionSample& b = work.samples[right];
            return std::tuple{a.target_face.value, a.texel_y, a.texel_x, left} <
                std::tuple{b.target_face.value, b.texel_y, b.texel_x, right};
        });
    request.rays.reserve(ordered_samples.size());
    for (const std::size_t index : ordered_samples) {
        const detail::NormalMapProjectionSample& sample = work.samples[index];
        request.rays.push_back(BakeRayQueryRay{
            static_cast<std::uint64_t>(index),
            sample.target_face.value,
            sample.texel_x,
            sample.texel_y,
            sample.ray_origin,
            sample.ray_direction,
            recipe.projection_half_extent * 2.0});
    }
    if (auto validation = request.validate(); !validation) {
        return core::Result<BakeRayQueryRequest>::failure(validation.error());
    }
    return core::Result<BakeRayQueryRequest>::success(std::move(request));
}

bool worker_outcomes_match_cpu_projection(
    const BakeRayQueryReport& report,
    const detail::NormalMapBakeWork& work,
    std::span<const detail::NormalMapProjectionOutcome> expected) {
    if (expected.size() != work.samples.size()) return false;
    for (const BakeRayQueryOutcome& outcome : report.outcomes) {
        if (outcome.sample_index >= expected.size()) return false;
        const detail::NormalMapProjectionOutcome& reference =
            expected[static_cast<std::size_t>(outcome.sample_index)];
        if (outcome.hit != reference.hit || outcome.backface != reference.backface) {
            return false;
        }
        if (reference.hit) {
            if (reference.triangle_index >= work.source_triangles.size()) return false;
            const detail::NormalMapProjectionTriangle& triangle =
                work.source_triangles[reference.triangle_index];
            if (outcome.source_face_id != triangle.face_id ||
                outcome.source_fan_index != triangle.fan_index) {
                return false;
            }
            const double distance_tolerance = kWorkerDistanceRelativeTolerance *
                std::max(1.0, std::abs(reference.distance));
            if (std::abs(outcome.distance - reference.distance) > distance_tolerance) {
                return false;
            }
            for (std::size_t coordinate = 0U; coordinate < 3U; ++coordinate) {
                if (std::abs(outcome.barycentric[coordinate] -
                        reference.barycentric[coordinate]) >
                    kWorkerBarycentricTolerance) {
                    return false;
                }
            }
        }

        if (reference.rejected_backface_triangle_index.has_value()) {
            const std::size_t index = *reference.rejected_backface_triangle_index;
            if (index >= work.source_triangles.size() ||
                !outcome.rejected_backface.has_value()) {
                return false;
            }
            const detail::NormalMapProjectionTriangle& triangle = work.source_triangles[index];
            if (*outcome.rejected_backface !=
                std::pair{triangle.face_id, triangle.fan_index}) {
                return false;
            }
        } else if (outcome.rejected_backface.has_value()) {
            return false;
        }
    }
    return true;
}

} // namespace

core::Result<void> BakeRayQueryRequest::validate() const {
    if (protocol_version != kBakeRayQueryProtocolVersion) {
        return failed_validation("ray-query request protocol version is unsupported");
    }
    if (auto validation = recipe.validate(); !validation) return validation;
    if (normal_space != BakeNormalSpace::tangent) {
        return failed_validation("ray-query worker currently accepts tangent-space normal maps only");
    }
    const auto expected_recipe_digest = recipe.canonical_digest();
    const auto expected_settings_digest = recipe.settings_digest();
    if (!expected_recipe_digest || !expected_settings_digest ||
        recipe_digest != expected_recipe_digest.value() ||
        settings_digest != expected_settings_digest.value()) {
        return failed_validation("ray-query request recipe or settings digest is stale");
    }
    if (triangles.empty() || triangles.size() > kMaxBakeRayQueryTriangles ||
        rays.empty() || rays.size() > kMaxBakeRayQueryRays) {
        return failed_validation("ray-query request exceeds its bounded triangle or ray count");
    }
    const std::uint64_t triangle_bytes =
        static_cast<std::uint64_t>(triangles.size()) * sizeof(BakeRayQueryTriangle);
    const std::uint64_t ray_bytes =
        static_cast<std::uint64_t>(rays.size()) * sizeof(BakeRayQueryRay);
    if (triangle_bytes > kMaxBakeRayQueryRequestBytes ||
        ray_bytes > kMaxBakeRayQueryRequestBytes - triangle_bytes) {
        return failed_validation("ray-query request exceeds its deterministic byte budget");
    }

    std::set<std::pair<std::uint64_t, std::uint32_t>> triangle_ids;
    for (const BakeRayQueryTriangle& triangle : triangles) {
        if (triangle.face_id == 0U || !finite_triangle(triangle) ||
            !triangle_ids.emplace(triangle.face_id, triangle.fan_index).second) {
            return failed_validation("ray-query request has invalid or duplicate source triangle identity");
        }
        const core::Vec3d normal = core::cross(
            triangle.positions[1U] - triangle.positions[0U],
            triangle.positions[2U] - triangle.positions[0U]);
        if (!normal.finite() || normal.length() <= kRayGeometryEpsilon) {
            return failed_validation("ray-query request contains degenerate source geometry");
        }
    }

    std::vector<bool> sample_indices(rays.size(), false);
    std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> ray_ids;
    std::optional<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint64_t>>
        previous_key;
    for (const BakeRayQueryRay& ray : rays) {
        if (ray.sample_index >= rays.size() || ray.target_face_id == 0U ||
            !ray.origin.finite() || !ray.direction.finite() ||
            !std::isfinite(ray.maximum_distance) || ray.maximum_distance <= 0.0 ||
            ray.maximum_distance != recipe.projection_half_extent * 2.0) {
            return failed_validation("ray-query request contains a malformed or unbounded ray");
        }
        const double direction_length = ray.direction.length();
        if (!std::isfinite(direction_length) || std::abs(direction_length - 1.0) > 1e-8) {
            return failed_validation("ray-query direction must be finite and normalized");
        }
        if (sample_indices[static_cast<std::size_t>(ray.sample_index)]) {
            return failed_validation("ray-query request repeats a raster sample index");
        }
        sample_indices[static_cast<std::size_t>(ray.sample_index)] = true;
        if (!ray_ids.emplace(ray.target_face_id, ray.texel_y, ray.texel_x).second) {
            return failed_validation("ray-query request repeats a target texel identity");
        }
        const auto key = std::tuple{
            ray.target_face_id, ray.texel_y, ray.texel_x, ray.sample_index};
        if (previous_key.has_value() && key <= *previous_key) {
            return failed_validation("ray-query rays are not in canonical stable order");
        }
        previous_key = key;
    }
    return core::Result<void>::success();
}

bool BakeRayQueryReport::completed() const noexcept {
    return success && error.empty() &&
        protocol_version == kBakeRayQueryProtocolVersion && worker_version != 0U &&
        valid_worker_id(worker_id) && valid_evidence_class(evidence_class) &&
        !worker_build_digest.is_zero() && !request_digest.is_zero() &&
        !geometry_digest.is_zero() && !source_digest.is_zero() &&
        !target_digest.is_zero() && !recipe_digest.is_zero() &&
        !settings_digest.is_zero() && !worker_provenance_digest.is_zero() &&
        !output_digest.is_zero();
}

core::Result<void> BakeRayQueryReport::validate_for(
    const BakeRayQueryRequest& request) const {
    if (auto validation = request.validate(); !validation) return validation;
    if (!completed()) {
        return failed_validation("ray worker did not return complete, identified evidence");
    }
    if (error.size() > 1024U) {
        return failed_validation("ray worker error text exceeds its bounded size");
    }
    if (outcomes.size() != request.rays.size()) {
        return failed_validation("ray worker returned a different outcome count than the request");
    }
    const auto expected_request_digest = bake_ray_query_request_digest(request);
    const auto expected_geometry_digest = bake_ray_query_geometry_digest(request);
    if (!expected_request_digest || !expected_geometry_digest ||
        request_digest != expected_request_digest.value() ||
        geometry_digest != expected_geometry_digest.value() ||
        source_digest != request.recipe.source.content_digest ||
        target_digest != request.recipe.target.content_digest ||
        recipe_digest != request.recipe_digest ||
        settings_digest != request.settings_digest ||
        worker_provenance_digest != ::carto::production::worker_provenance_digest(*this) ||
        output_digest != bake_ray_query_output_digest(*this)) {
        return failed_validation("ray worker provenance or output digest does not match the request");
    }

    std::map<std::pair<std::uint64_t, std::uint32_t>, const BakeRayQueryTriangle*> triangles_by_id;
    for (const BakeRayQueryTriangle& triangle : request.triangles) {
        triangles_by_id.emplace(
            std::pair{triangle.face_id, triangle.fan_index}, &triangle);
    }
    for (std::size_t index = 0U; index < outcomes.size(); ++index) {
        const BakeRayQueryRay& ray = request.rays[index];
        const BakeRayQueryOutcome& outcome = outcomes[index];
        if (outcome.sample_index != ray.sample_index ||
            outcome.target_face_id != ray.target_face_id ||
            outcome.texel_x != ray.texel_x || outcome.texel_y != ray.texel_y) {
            return failed_validation("ray worker reordered or changed a target sample identity");
        }
        if (!outcome.hit) {
            if (outcome.source_face_id != 0U || outcome.source_fan_index != 0U ||
                outcome.backface || outcome.distance != 0.0 ||
                std::any_of(outcome.barycentric.begin(), outcome.barycentric.end(),
                    [](double value) { return value != 0.0; })) {
                return failed_validation("ray worker miss contains contradictory hit data");
            }
        } else {
            const auto triangle = triangles_by_id.find(
                {outcome.source_face_id, outcome.source_fan_index});
            if (triangle == triangles_by_id.end()) {
                return failed_validation("ray worker selected a source triangle outside the request");
            }
            const auto expected_hit = intersect_triangle(ray, *triangle->second);
            if (!expected_hit || !std::isfinite(outcome.distance) ||
                outcome.distance <= 0.0 || outcome.distance > ray.maximum_distance ||
                outcome.backface == expected_hit->front_facing) {
                return failed_validation("ray worker hit does not match the submitted source geometry");
            }
            const double distance_tolerance = kWorkerDistanceRelativeTolerance *
                std::max({1.0, std::abs(expected_hit->distance), ray.maximum_distance});
            if (std::abs(outcome.distance - expected_hit->distance) > distance_tolerance ||
                !std::all_of(outcome.barycentric.begin(), outcome.barycentric.end(),
                    [](double value) { return std::isfinite(value); })) {
                return failed_validation("ray worker hit distance or barycentrics disagree with geometry");
            }
            for (std::size_t coordinate = 0U; coordinate < 3U; ++coordinate) {
                if (std::abs(outcome.barycentric[coordinate] -
                        expected_hit->barycentric[coordinate]) >
                    kWorkerBarycentricTolerance) {
                    return failed_validation("ray worker barycentrics disagree with the submitted triangle");
                }
            }
            if (request.recipe.backface_policy == BakeBackfacePolicy::front_only &&
                outcome.backface) {
                return failed_validation("front-only ray policy returned an accepted backface");
            }
        }

        if (outcome.rejected_backface.has_value()) {
            if (request.recipe.backface_policy != BakeBackfacePolicy::front_only) {
                return failed_validation("two-sided ray policy returned a rejected backface");
            }
            const auto rejected = triangles_by_id.find(*outcome.rejected_backface);
            if (rejected == triangles_by_id.end()) {
                return failed_validation("ray worker rejected a backface outside the request");
            }
            const auto rejected_hit = intersect_triangle(ray, *rejected->second);
            if (!rejected_hit || rejected_hit->front_facing) {
                return failed_validation("ray worker rejected geometry that is not a ray backface");
            }
        }
    }
    return core::Result<void>::success();
}

bool BakeRayQueryReport::matches_request(const BakeRayQueryRequest& request) const {
    return static_cast<bool>(validate_for(request));
}

core::Result<assets::Sha256Digest> bake_ray_query_geometry_digest(
    const BakeRayQueryRequest& request) {
    if (auto validation = request.validate(); !validation) {
        return core::Result<assets::Sha256Digest>::failure(validation.error());
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(16U + request.triangles.size() * 84U);
    append_string(bytes, "CARTO_BAKE_RAY_GEOMETRY 1");
    append_u64(bytes, static_cast<std::uint64_t>(request.triangles.size()));
    for (const BakeRayQueryTriangle& triangle : request.triangles) {
        append_u64(bytes, triangle.face_id);
        append_u32(bytes, triangle.fan_index);
        for (const core::Vec3d position : triangle.positions) append_vec3(bytes, position);
    }
    return core::Result<assets::Sha256Digest>::success(hash_bytes(bytes));
}

core::Result<assets::Sha256Digest> bake_ray_query_request_digest(
    const BakeRayQueryRequest& request) {
    if (auto validation = request.validate(); !validation) {
        return core::Result<assets::Sha256Digest>::failure(validation.error());
    }
    const auto geometry_digest = bake_ray_query_geometry_digest(request);
    if (!geometry_digest) {
        return core::Result<assets::Sha256Digest>::failure(geometry_digest.error());
    }
    const std::string recipe_text = request.recipe.serialize();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(recipe_text.size() + request.rays.size() * 88U + 160U);
    append_string(bytes, "CARTO_BAKE_RAY_REQUEST 1");
    append_u32(bytes, request.protocol_version);
    append_string(bytes, recipe_text);
    append_u8(bytes, static_cast<std::uint8_t>(request.normal_space));
    append_digest(bytes, request.recipe_digest);
    append_digest(bytes, request.settings_digest);
    append_digest(bytes, geometry_digest.value());
    append_u64(bytes, static_cast<std::uint64_t>(request.rays.size()));
    for (const BakeRayQueryRay& ray : request.rays) {
        append_u64(bytes, ray.sample_index);
        append_u64(bytes, ray.target_face_id);
        append_u32(bytes, ray.texel_x);
        append_u32(bytes, ray.texel_y);
        append_vec3(bytes, ray.origin);
        append_vec3(bytes, ray.direction);
        append_double(bytes, ray.maximum_distance);
    }
    return core::Result<assets::Sha256Digest>::success(hash_bytes(bytes));
}

assets::Sha256Digest bake_ray_query_output_digest(const BakeRayQueryReport& report) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(report.outcomes.size() * 112U + 40U);
    append_string(bytes, "CARTO_BAKE_RAY_OUTPUT 1");
    append_u64(bytes, static_cast<std::uint64_t>(report.outcomes.size()));
    for (const BakeRayQueryOutcome& outcome : report.outcomes) {
        append_u64(bytes, outcome.sample_index);
        append_u64(bytes, outcome.target_face_id);
        append_u32(bytes, outcome.texel_x);
        append_u32(bytes, outcome.texel_y);
        append_u8(bytes, outcome.hit ? 1U : 0U);
        append_u64(bytes, outcome.source_face_id);
        append_u32(bytes, outcome.source_fan_index);
        append_u8(bytes, outcome.backface ? 1U : 0U);
        append_double(bytes, outcome.distance);
        for (const double barycentric : outcome.barycentric) {
            append_double(bytes, barycentric);
        }
        append_u8(bytes, outcome.rejected_backface.has_value() ? 1U : 0U);
        if (outcome.rejected_backface.has_value()) {
            append_u64(bytes, outcome.rejected_backface->first);
            append_u32(bytes, outcome.rejected_backface->second);
        }
    }
    return hash_bytes(bytes);
}

core::Result<void> refresh_bake_ray_query_report(
    const BakeRayQueryRequest& request,
    BakeRayQueryReport& report) {
    if (auto validation = request.validate(); !validation) return validation;
    if (!report.success || !report.error.empty() || report.worker_version == 0U ||
        !valid_worker_id(report.worker_id) ||
        !valid_evidence_class(report.evidence_class) ||
        report.worker_build_digest.is_zero()) {
        return failed_validation("cannot bind a failed or unidentified ray worker report");
    }
    const auto request_digest = bake_ray_query_request_digest(request);
    const auto geometry_digest = bake_ray_query_geometry_digest(request);
    if (!request_digest || !geometry_digest) {
        return core::Result<void>::failure(
            !request_digest ? request_digest.error() : geometry_digest.error());
    }
    report.protocol_version = kBakeRayQueryProtocolVersion;
    report.request_digest = request_digest.value();
    report.geometry_digest = geometry_digest.value();
    report.source_digest = request.recipe.source.content_digest;
    report.target_digest = request.recipe.target.content_digest;
    report.recipe_digest = request.recipe_digest;
    report.settings_digest = request.settings_digest;
    report.worker_provenance_digest = worker_provenance_digest(report);
    report.output_digest = bake_ray_query_output_digest(report);
    return core::Result<void>::success();
}

core::Result<std::string> bake_ray_worker_platform_profile(
    const BakeRayQueryReport& report) {
    if (!report.completed() ||
        report.worker_provenance_digest != worker_provenance_digest(report)) {
        return core::Result<std::string>::failure(
            validation_error("ray worker profile requires complete worker provenance"));
    }
    std::vector<std::uint8_t> bytes;
    append_string(bytes, "CARTO_BAKE_RAY_WORKER_PROFILE 1");
    append_u32(bytes, report.protocol_version);
    append_u32(bytes, report.worker_version);
    append_digest(bytes, report.worker_provenance_digest);
    const assets::Sha256Digest digest = hash_bytes(bytes);
    return core::Result<std::string>::success(
        "carto.ray-worker." + std::to_string(report.worker_version) + "." + digest.hex());
}

bool BakeRayQueryCompileResult::succeeded() const {
    if (!bake.succeeded() || !bake.artifact.has_value() ||
        !worker.matches_request(request) ||
        bake.artifact->normal_space != BakeNormalSpace::tangent ||
        bake.artifact->recipe != request.recipe ||
        cpu_reference_artifact_digest != bake.artifact->artifact_digest ||
        !bake.artifact->validate() || validation_digest_.is_zero()) {
        return false;
    }
    const auto current_digest = compile_result_digest(*this);
    return current_digest && current_digest.value() == validation_digest_;
}

BakeRayQueryCompileResult compile_tangent_normal_map_with_ray_worker(
    const BakeRecipe& recipe,
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& target,
    const BakeRayQueryCallback& worker) {
    BakeRayQueryCompileResult result;
    detail::NormalMapBakeWork work = detail::prepare_normal_map_work(
        recipe, source, target, BakeNormalSpace::tangent);
    const bool preparation_failed = std::any_of(
        work.report.diagnostics.begin(), work.report.diagnostics.end(),
        [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; });
    if (preparation_failed) {
        result.bake = std::move(work.report);
        return result;
    }
    if (work.samples.empty()) {
        result.bake = detail::finalize_normal_map_work(
            std::move(work), std::span<const detail::NormalMapProjectionOutcome>{});
        return result;
    }
    if (!worker) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_missing",
            "normal-map ray worker callback is not configured");
        return result;
    }

    auto request = make_request(recipe, work);
    if (!request) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_request_invalid", request.error().message);
        return result;
    }
    result.request = std::move(request).value();

    const auto worker_started = std::chrono::steady_clock::now();
    try {
        result.worker = worker(result.request);
    } catch (const std::exception& exception) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_exception", exception.what());
        return result;
    } catch (...) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_exception",
            "normal-map ray worker threw a non-standard exception");
        return result;
    }
    const auto worker_finished = std::chrono::steady_clock::now();
    result.worker.elapsed_nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            worker_finished - worker_started).count());

    if (!result.worker.success) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_failed",
            result.worker.error.empty()
                ? "ray worker reported failure without a diagnostic"
                : result.worker.error);
        return result;
    }
    if (auto validation = result.worker.validate_for(result.request); !validation) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_report_invalid",
            validation.error().message);
        return result;
    }
    if (auto validation = validate_bake_sources(recipe, source, target); !validation) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_input_stale",
            "bake inputs changed while the ray worker was running: " +
                validation.error().message);
        return result;
    }
    std::vector<detail::NormalMapProjectionOutcome> cpu_projection =
        detail::cast_normal_map_cpu_reference(work, source);
    if (!worker_outcomes_match_cpu_projection(result.worker, work, cpu_projection)) {
        result.bake = std::move(work.report);
        append_compile_error(result.bake, "ray_worker_cpu_projection_mismatch",
            "ray worker hit selection differs from the canonical CPU projection results");
        return result;
    }

    BakeExecutionReport reference = detail::finalize_normal_map_work(
        std::move(work), cpu_projection);
    if (!reference.succeeded()) {
        result.bake = std::move(reference);
        return result;
    }
    const bool cpu_reference_succeeded = reference.succeeded();
    result.cpu_reference_artifact_digest = reference.artifact->artifact_digest;
    reference.artifact.reset();
    reference.diagnostics.clear();
    std::vector<detail::NormalMapProjectionOutcome>{}.swap(
        cpu_projection);

    detail::NormalMapBakeWork worker_work = detail::prepare_normal_map_work(
        recipe, source, target, BakeNormalSpace::tangent);
    if (std::any_of(worker_work.report.diagnostics.begin(), worker_work.report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        result.bake = std::move(worker_work.report);
        append_compile_error(result.bake, "ray_worker_reprepare_failed",
            "inputs no longer prepare after CPU hit-selection verification");
        return result;
    }
    const auto canonical_request = make_request(recipe, worker_work);
    const auto submitted_request_digest = bake_ray_query_request_digest(result.request);
    if (!canonical_request || !submitted_request_digest) {
        result.bake = std::move(worker_work.report);
        append_compile_error(result.bake, "ray_worker_reprepare_failed",
            "could not verify the current request against the submitted worker request");
        return result;
    }
    const auto canonical_request_digest = bake_ray_query_request_digest(
        canonical_request.value());
    if (!canonical_request_digest ||
        canonical_request_digest.value() != submitted_request_digest.value()) {
        result.bake = std::move(worker_work.report);
        append_compile_error(result.bake, "ray_worker_input_stale",
            "bake inputs changed after CPU hit-selection verification");
        return result;
    }

    std::map<std::pair<std::uint64_t, std::uint32_t>, std::size_t> triangle_indices;
    for (std::size_t index = 0U; index < worker_work.source_triangles.size(); ++index) {
        const detail::NormalMapProjectionTriangle& triangle = worker_work.source_triangles[index];
        triangle_indices.emplace(
            std::pair{triangle.face_id, triangle.fan_index}, index);
    }
    std::vector<detail::NormalMapProjectionOutcome> outcomes(worker_work.samples.size());
    for (const BakeRayQueryOutcome& hit : result.worker.outcomes) {
        const std::size_t sample_index = static_cast<std::size_t>(hit.sample_index);
        detail::NormalMapProjectionOutcome& outcome = outcomes[sample_index];
        outcome.hit = hit.hit;
        outcome.backface = hit.backface;
        if (hit.hit) {
            const auto triangle_index = triangle_indices.find(
                {hit.source_face_id, hit.source_fan_index});
            if (triangle_index == triangle_indices.end()) {
                result.bake = std::move(worker_work.report);
                append_compile_error(result.bake, "ray_worker_source_identity_invalid",
                    "ray worker selected a source triangle outside the prepared snapshot");
                return result;
            }
            outcome.triangle_index = triangle_index->second;
            outcome.distance = hit.distance;
            outcome.barycentric = hit.barycentric;
        }
        if (hit.rejected_backface.has_value()) {
            const auto triangle_index = triangle_indices.find(*hit.rejected_backface);
            if (triangle_index == triangle_indices.end()) {
                result.bake = std::move(worker_work.report);
                append_compile_error(result.bake, "ray_worker_rejected_identity_invalid",
                    "ray worker rejected a source triangle outside the prepared snapshot");
                return result;
            }
            outcome.rejected_backface_triangle_index = triangle_index->second;
        }
    }

    result.bake = detail::finalize_normal_map_work(std::move(worker_work), outcomes);
    if (result.bake.succeeded() && !cpu_reference_succeeded) {
        append_compile_error(result.bake, "ray_worker_cpu_reference_rejected",
            "ray-worker bake produced an artifact while the CPU reference rejected the same inputs");
        return result;
    }
    if (!result.bake.succeeded() && cpu_reference_succeeded) {
        append_compile_error(result.bake, "ray_worker_cpu_reference_status_mismatch",
            "ray-worker bake rejected inputs accepted by the CPU reference");
        return result;
    }
    if (!result.bake.succeeded()) return result;
    if (result.bake.artifact->artifact_digest != result.cpu_reference_artifact_digest) {
        append_compile_error(result.bake, "ray_worker_cpu_reference_artifact_mismatch",
            "ray-worker projection output differs from the canonical CPU bake artifact");
        return result;
    }
    if (auto validation = validate_bake_sources(recipe, source, target); !validation) {
        append_compile_error(result.bake, "ray_worker_input_stale",
            "bake inputs changed during CPU parity verification: " +
                validation.error().message);
        return result;
    }
    const auto validation_digest = compile_result_digest(result);
    if (!validation_digest) {
        append_compile_error(result.bake, "ray_worker_result_seal_failed",
            validation_digest.error().message);
        return result;
    }
    result.validation_digest_ = validation_digest.value();
    return result;
}

core::Result<void> validate_tangent_normal_map_ray_worker_result(
    const BakeRayQueryCompileResult& result,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target) {
    if (!result.succeeded() || !result.worker.matches_request(result.request) ||
        !result.bake.artifact.has_value() ||
        result.bake.artifact->normal_space != BakeNormalSpace::tangent ||
        result.bake.artifact->recipe != result.request.recipe ||
        result.cpu_reference_artifact_digest !=
            result.bake.artifact->artifact_digest) {
        return failed_validation(
            "ray-worker publication requires a successful, parity-checked tangent bake");
    }
    if (auto validation = validate_bake_sources(
            result.bake.artifact->recipe, current_source, current_target); !validation) {
        return validation;
    }
    detail::NormalMapBakeWork work = detail::prepare_normal_map_work(
        result.bake.artifact->recipe, current_source, current_target,
        BakeNormalSpace::tangent);
    if (std::any_of(work.report.diagnostics.begin(), work.report.diagnostics.end(),
            [](const BakeDiagnostic& diagnostic) { return diagnostic.is_error; })) {
        return failed_validation("current inputs no longer prepare for ray-worker publication");
    }
    const auto canonical_request = make_request(result.bake.artifact->recipe, work);
    const auto submitted_request_digest = bake_ray_query_request_digest(result.request);
    if (!canonical_request || !submitted_request_digest) {
        return core::Result<void>::failure(
            !canonical_request ? canonical_request.error() : submitted_request_digest.error());
    }
    const auto canonical_request_digest = bake_ray_query_request_digest(
        canonical_request.value());
    if (!canonical_request_digest ||
        canonical_request_digest.value() != submitted_request_digest.value()) {
        return failed_validation(
            "ray-worker publication request differs from current prepared inputs");
    }
    const std::vector<detail::NormalMapProjectionOutcome> cpu_projection =
        detail::cast_normal_map_cpu_reference(work, current_source);
    if (!worker_outcomes_match_cpu_projection(result.worker, work, cpu_projection)) {
        return failed_validation(
            "ray-worker publication evidence differs from current CPU hit selection");
    }
    const BakeExecutionReport current_reference = detail::finalize_normal_map_work(
        std::move(work), cpu_projection);
    if (!current_reference.succeeded() ||
        current_reference.artifact->artifact_digest !=
            result.bake.artifact->artifact_digest ||
        current_reference.artifact->artifact_digest !=
            result.cpu_reference_artifact_digest) {
        return failed_validation(
            "ray-worker artifact no longer matches the current CPU reference");
    }
    return core::Result<void>::success();
}

core::Result<void> publish_tangent_normal_map_with_ray_worker(
    ProductionArtifactStore& store,
    ProductionArtifactReceipt receipt,
    const BakeRayQueryCompileResult& result,
    const geometry::EditableMesh& current_source,
    const geometry::EditableMesh& current_target,
    const AdmissionPolicy& policy) {
    if (auto validation = validate_tangent_normal_map_ray_worker_result(
            result, current_source, current_target); !validation) {
        return validation;
    }
    const auto profile = bake_ray_worker_platform_profile(result.worker);
    if (!profile) return core::Result<void>::failure(profile.error());
    return publish_normal_map(
        store, std::move(receipt), *result.bake.artifact,
        current_source, current_target, policy, profile.value());
}

} // namespace carto::production
