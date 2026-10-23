#include <carto/surface/core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::surface {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kEpsilon = 1e-12;
constexpr std::size_t kMaxSurfaceViewSelectionCandidates = 64U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

bool finite_unit(double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool valid_hex_digest(std::string_view value) {
    return value.size() == 64U && assets::Sha256Digest::from_hex(value).has_value();
}

bool valid_representation_kind(SurfaceRepresentationKind kind) {
    switch (kind) {
    case SurfaceRepresentationKind::authored:
    case SurfaceRepresentationKind::dense_geometry:
    case SurfaceRepresentationKind::simplified_geometry:
    case SurfaceRepresentationKind::displacement_surface:
    case SurfaceRepresentationKind::normal_distribution:
    case SurfaceRepresentationKind::closure_proxy:
    case SurfaceRepresentationKind::collision_proxy:
    case SurfaceRepresentationKind::runtime_cluster:
        return true;
    }
    return false;
}

bool valid_representation_mode(RepresentationMode mode) {
    switch (mode) {
    case RepresentationMode::geometry_only:
    case RepresentationMode::fidelity_closure:
        return true;
    }
    return false;
}

bool valid_migration_target(MigrationTarget target) {
    switch (target) {
    case MigrationTarget::displacement:
    case MigrationTarget::normal_field:
    case MigrationTarget::ndf:
    case MigrationTarget::roughness:
    case MigrationTarget::anisotropy:
    case MigrationTarget::cavity:
    case MigrationTarget::directional_visibility:
    case MigrationTarget::silhouette_descriptor:
        return true;
    }
    return false;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::string digest_hex(std::string_view value);
std::string vec3_canonical(core::Vec3d value);

std::string compiled_mesh_canonical(const geometry::CompiledMesh& mesh) {
    std::ostringstream output;
    output << std::setprecision(17) << "compiled-mesh-v1;revision="
           << mesh.source_revision.value() << ";positions=";
    for (const auto& position : mesh.positions) output << vec3_canonical(position) << ';';
    output << ";normals=";
    for (const auto& normal : mesh.normals) output << vec3_canonical(normal) << ';';
    output << ";indices=";
    for (const std::uint32_t index : mesh.indices) output << index << ',';
    output << ";vertex_ids=";
    for (const geometry::VertexId id : mesh.vertex_ids) output << id.value << ',';
    output << ";triangle_faces=";
    for (const geometry::FaceId face : mesh.triangle_faces) output << face.value << ',';
    output << ";triangle_edges=";
    for (const auto& edges : mesh.triangle_edges) {
        for (const auto& edge : edges) output << (edge.has_value() ? edge->value : 0U) << ',';
        output << ';';
    }
    output << ";bounds=" << vec3_canonical(mesh.bounds.minimum) << ':'
           << vec3_canonical(mesh.bounds.maximum);
    return output.str();
}

std::string compiled_mesh_digest(const geometry::CompiledMesh& mesh) {
    return digest_hex(compiled_mesh_canonical(mesh));
}

std::string digest_hex(std::string_view value) {
    return digest_text(value).hex();
}

std::string vec3_canonical(core::Vec3d value) {
    std::ostringstream output;
    output << std::setprecision(17) << value.x << ',' << value.y << ',' << value.z;
    return output.str();
}

std::string digest_prefix_id(std::string_view value) {
    const auto digest = digest_text(value);
    std::uint64_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        result = (result << 8U) | digest.bytes[index];
    }
    if (result == 0U) result = 1U;
    return std::to_string(result);
}

core::Vec3d safe_normal(core::Vec3d value, core::Vec3d fallback) {
    const core::Vec3d normalized = value.normalized();
    return normalized.finite() ? normalized : fallback;
}

double clamp_dot(core::Vec3d left, core::Vec3d right) noexcept {
    return std::clamp(core::dot(left, right), -1.0, 1.0);
}

struct Basis {
    core::Vec3d normal;
    core::Vec3d tangent;
    core::Vec3d bitangent;
};

Basis make_basis(core::Vec3d normal) {
    normal = safe_normal(normal, {0.0, 0.0, 1.0});
    const core::Vec3d reference = std::abs(normal.z) < 0.8
        ? core::Vec3d{0.0, 0.0, 1.0}
        : core::Vec3d{0.0, 1.0, 0.0};
    core::Vec3d tangent = safe_normal(core::cross(reference, normal), {1.0, 0.0, 0.0});
    core::Vec3d bitangent = safe_normal(core::cross(normal, tangent), {0.0, 1.0, 0.0});
    return {normal, tangent, bitangent};
}

struct TriangleInfo {
    std::uint32_t source_index = 0U;
    std::array<std::uint32_t, 3U> vertices{};
    geometry::FaceId face;
    core::Vec3d normal{};
    core::Vec3d centroid{};
    double area = 0.0;
};

struct EdgeIncident {
    std::uint32_t triangle = 0U;
    std::uint32_t first_vertex = 0U;
    std::uint32_t second_vertex = 0U;
    core::Vec3d normal{};
};

core::Result<std::vector<TriangleInfo>> collect_triangles(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain) {
    if (!mesh.valid()) {
        return core::Result<std::vector<TriangleInfo>>::failure(
            validation("surface analysis requires a valid compiled mesh"));
    }
    std::set<geometry::FaceId> faces(domain.source_faces.begin(), domain.source_faces.end());
    std::vector<TriangleInfo> result;
    result.reserve(mesh.indices.size() / 3U);
    for (std::size_t index = 0U; index < mesh.indices.size(); index += 3U) {
        const std::uint32_t first = mesh.indices[index];
        const std::uint32_t second = mesh.indices[index + 1U];
        const std::uint32_t third = mesh.indices[index + 2U];
        const geometry::FaceId face = mesh.triangle_faces[index / 3U];
        if (!faces.contains(face)) continue;
        const core::Vec3d raw_normal = core::cross(
            mesh.positions[second] - mesh.positions[first],
            mesh.positions[third] - mesh.positions[first]);
        const double double_area = raw_normal.length();
        if (!std::isfinite(double_area) || double_area <= kEpsilon) {
            return core::Result<std::vector<TriangleInfo>>::failure(
                validation("surface analysis encountered a degenerate triangle"));
        }
        const core::Vec3d normal = raw_normal * (1.0 / double_area);
        result.push_back(TriangleInfo{
            static_cast<std::uint32_t>(index / 3U),
            {first, second, third},
            face,
            normal,
            (mesh.positions[first] + mesh.positions[second] + mesh.positions[third]) * (1.0 / 3.0),
            double_area * 0.5,
        });
    }
    if (result.empty()) {
        return core::Result<std::vector<TriangleInfo>>::failure(
            validation("surface domain does not cover any compiled triangles"));
    }
    return core::Result<std::vector<TriangleInfo>>::success(std::move(result));
}

core::Vec3d weighted_normal(const std::vector<TriangleInfo>& triangles) {
    core::Vec3d sum{};
    for (const TriangleInfo& triangle : triangles) {
        sum = sum + triangle.normal * triangle.area;
    }
    return safe_normal(sum, {0.0, 0.0, 1.0});
}

std::map<geometry::EdgeId, std::vector<EdgeIncident>> collect_edges(
    const geometry::CompiledMesh& mesh,
    const std::vector<TriangleInfo>& triangles) {
    std::map<geometry::EdgeId, std::vector<EdgeIncident>> edges;
    for (const TriangleInfo& triangle : triangles) {
        const auto& edge_ids = mesh.triangle_edges[triangle.source_index];
        const std::array<std::pair<std::uint32_t, std::uint32_t>, 3U> vertices = {{
            {triangle.vertices[0U], triangle.vertices[1U]},
            {triangle.vertices[1U], triangle.vertices[2U]},
            {triangle.vertices[2U], triangle.vertices[0U]},
        }};
        for (std::size_t slot = 0U; slot < edge_ids.size(); ++slot) {
            if (!edge_ids[slot].has_value()) continue;
            edges[*edge_ids[slot]].push_back(EdgeIncident{
                triangle.source_index,
                vertices[slot].first,
                vertices[slot].second,
                triangle.normal,
            });
        }
    }
    return edges;
}

double triangle_angle(core::Vec3d first, core::Vec3d second, core::Vec3d vertex) {
    const core::Vec3d left = safe_normal(first - vertex, {});
    const core::Vec3d right = safe_normal(second - vertex, {});
    if (!left.finite() || !right.finite()) return 0.0;
    return std::acos(clamp_dot(left, right));
}

struct NdfSample {
    core::Vec3d normal;
    double weight = 1.0;
};

NdfClosure fit_ndf(
    core::Vec3d reference_normal,
    const std::vector<NdfSample>& samples) {
    const Basis basis = make_basis(reference_normal);
    NdfClosure result;
    result.reference_normal = basis.normal;
    if (samples.empty()) return result;

    double total_weight = 0.0;
    double mean_x = 0.0;
    double mean_y = 0.0;
    std::vector<std::pair<double, double>> slopes;
    slopes.reserve(samples.size());
    for (const NdfSample& sample : samples) {
        const double z = std::max(kEpsilon, core::dot(sample.normal, basis.normal));
        const double x = core::dot(sample.normal, basis.tangent) / z;
        const double y = core::dot(sample.normal, basis.bitangent) / z;
        const double weight = std::max(kEpsilon, sample.weight);
        slopes.emplace_back(x, y);
        mean_x += x * weight;
        mean_y += y * weight;
        total_weight += weight;
    }
    mean_x /= total_weight;
    mean_y /= total_weight;

    double covariance_xx = 0.0;
    double covariance_yy = 0.0;
    double covariance_xy = 0.0;
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        const double weight = std::max(kEpsilon, samples[index].weight);
        const double dx = slopes[index].first - mean_x;
        const double dy = slopes[index].second - mean_y;
        covariance_xx += dx * dx * weight;
        covariance_yy += dy * dy * weight;
        covariance_xy += dx * dy * weight;
    }
    covariance_xx /= total_weight;
    covariance_yy /= total_weight;
    covariance_xy /= total_weight;

    const double orientation = 0.5 * std::atan2(
        2.0 * covariance_xy, covariance_xx - covariance_yy);
    const double trace = covariance_xx + covariance_yy;
    const double discriminant = std::sqrt(std::max(
        0.0,
        (covariance_xx - covariance_yy) * (covariance_xx - covariance_yy) +
            4.0 * covariance_xy * covariance_xy));
    result.alpha_x = std::clamp(std::sqrt(std::max(1e-6, 0.5 * (trace + discriminant))), 0.001, 4.0);
    result.alpha_y = std::clamp(std::sqrt(std::max(1e-6, 0.5 * (trace - discriminant))), 0.001, 4.0);
    result.orientation = orientation;
    result.fit_error = std::sqrt(std::max(0.0, covariance_xy * covariance_xy));
    return result;
}

bool ray_hits_triangle(
    core::Vec3d origin,
    core::Vec3d direction,
    core::Vec3d first,
    core::Vec3d second,
    core::Vec3d third,
    double max_distance) {
    const core::Vec3d edge_one = second - first;
    const core::Vec3d edge_two = third - first;
    const core::Vec3d p = core::cross(direction, edge_two);
    const double determinant = core::dot(edge_one, p);
    if (std::abs(determinant) <= kEpsilon) return false;
    const double inverse = 1.0 / determinant;
    const core::Vec3d offset = origin - first;
    const double u = core::dot(offset, p) * inverse;
    if (u < 0.0 || u > 1.0) return false;
    const core::Vec3d q = core::cross(offset, edge_one);
    const double v = core::dot(direction, q) * inverse;
    if (v < 0.0 || u + v > 1.0) return false;
    const double distance = core::dot(edge_two, q) * inverse;
    return distance > 1e-7 && distance < max_distance;
}

bool occluded(
    const geometry::CompiledMesh& mesh,
    const std::vector<TriangleInfo>& triangles,
    core::Vec3d origin,
    core::Vec3d direction,
    std::uint32_t ignored_triangle,
    double max_distance) {
    for (const TriangleInfo& triangle : triangles) {
        if (triangle.source_index == ignored_triangle) continue;
        if (ray_hits_triangle(
                origin,
                direction,
                mesh.positions[triangle.vertices[0U]],
                mesh.positions[triangle.vertices[1U]],
                mesh.positions[triangle.vertices[2U]],
                max_distance)) {
            return true;
        }
    }
    return false;
}

VisibilityClosure build_visibility(
    const geometry::CompiledMesh& mesh,
    const std::vector<TriangleInfo>& triangles,
    const SurfaceAnalysisOptions& options,
    core::Vec3d reference_normal,
    const std::map<geometry::EdgeId, std::vector<EdgeIncident>>& edges) {
    const Basis basis = make_basis(reference_normal);
    VisibilityClosure result;
    result.reference_normal = basis.normal;
    const std::uint32_t samples_per_bin = std::max(
        1U, options.visibility_sample_count / static_cast<std::uint32_t>(
                VisibilityClosure::directional_bin_count));
    double total_visible = 0.0;
    core::Vec3d bent_sum{};
    for (std::size_t bin = 0U; bin < VisibilityClosure::directional_bin_count; ++bin) {
        const double phi = 2.0 * kPi * (static_cast<double>(bin) + 0.5) /
            static_cast<double>(VisibilityClosure::directional_bin_count);
        const core::Vec3d tangent_direction =
            basis.tangent * std::cos(phi) + basis.bitangent * std::sin(phi);
        const core::Vec3d direction = safe_normal(
            basis.normal * std::cos(kPi * 0.25) + tangent_direction * std::sin(kPi * 0.25),
            basis.normal);
        std::uint32_t visible = 0U;
        for (std::uint32_t sample = 0U; sample < samples_per_bin; ++sample) {
            const TriangleInfo& triangle = triangles[
                (bin * static_cast<std::size_t>(samples_per_bin) + sample) % triangles.size()];
            const core::Vec3d origin = triangle.centroid + basis.normal * 1e-7;
            if (!occluded(mesh, triangles, origin, direction, triangle.source_index,
                          options.thickness_max_distance)) {
                ++visible;
                bent_sum = bent_sum + direction;
            }
        }
        result.directional_visibility[bin] = static_cast<float>(visible) /
            static_cast<float>(samples_per_bin);
        total_visible += static_cast<double>(visible);
    }
    const double total_samples = static_cast<double>(samples_per_bin) *
        static_cast<double>(VisibilityClosure::directional_bin_count);
    result.ambient_visibility = total_visible / total_samples;
    result.cavity = 1.0 - result.ambient_visibility;
    result.bent_normal = safe_normal(bent_sum, basis.normal);
    std::size_t boundary_edges = 0U;
    for (const auto& [unused, incidents] : edges) {
        if (incidents.size() == 1U) ++boundary_edges;
    }
    result.silhouette_density = edges.empty()
        ? 0.0
        : static_cast<double>(boundary_edges) / static_cast<double>(edges.size());
    result.rms_error = 0.0;
    result.maximum_error = 0.0;
    return result;
}

ThicknessStats build_thickness(
    const geometry::CompiledMesh& mesh,
    const std::vector<TriangleInfo>& triangles,
    const SurfaceAnalysisOptions& options) {
    ThicknessStats result;
    double total = 0.0;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = 0.0;
    std::uint32_t measured = 0U;
    for (const TriangleInfo& triangle : triangles) {
        std::array<double, 2U> distances{};
        std::size_t hit_count = 0U;
        for (std::size_t side = 0U; side < distances.size(); ++side) {
            const core::Vec3d direction = triangle.normal * (side == 0U ? 1.0 : -1.0);
            double nearest = options.thickness_max_distance;
            std::size_t hits = 0U;
            for (const TriangleInfo& other : triangles) {
                if (other.source_index == triangle.source_index) continue;
                const core::Vec3d edge_one = mesh.positions[other.vertices[1U]] -
                    mesh.positions[other.vertices[0U]];
                const core::Vec3d edge_two = mesh.positions[other.vertices[2U]] -
                    mesh.positions[other.vertices[0U]];
                const core::Vec3d p = core::cross(direction, edge_two);
                const double determinant = core::dot(edge_one, p);
                if (std::abs(determinant) <= kEpsilon) continue;
                const double inverse = 1.0 / determinant;
                const core::Vec3d offset = triangle.centroid -
                    mesh.positions[other.vertices[0U]];
                const double u = core::dot(offset, p) * inverse;
                if (u < 0.0 || u > 1.0) continue;
                const core::Vec3d q = core::cross(offset, edge_one);
                const double v = core::dot(direction, q) * inverse;
                if (v < 0.0 || u + v > 1.0) continue;
                const double distance = core::dot(edge_two, q) * inverse;
                if (distance > 1e-7 && distance < options.thickness_max_distance) {
                    ++hits;
                    nearest = std::min(nearest, distance);
                }
            }
            if (hits != 0U) {
                distances[side] = nearest;
                ++hit_count;
            }
            if (hits > 1U) result.ambiguous = true;
        }
        if (hit_count == 2U) {
            const double thickness = distances[0U] + distances[1U];
            total += thickness;
            minimum = std::min(minimum, thickness);
            maximum = std::max(maximum, thickness);
            ++measured;
        }
    }
    result.measured_samples = measured;
    result.confidence = triangles.empty()
        ? 0.0
        : static_cast<double>(measured) / static_cast<double>(triangles.size());
    if (measured != 0U) {
        result.minimum = minimum;
        result.maximum = maximum;
        result.mean = total / static_cast<double>(measured);
    }
    return result;
}

core::Vec3d proxy_mean_normal(const GeometryProxy& proxy) {
    core::Vec3d sum{};
    for (std::size_t index = 0U; index < proxy.indices.size(); index += 3U) {
        const core::Vec3d raw = core::cross(
            proxy.positions[proxy.indices[index + 1U]] - proxy.positions[proxy.indices[index]],
            proxy.positions[proxy.indices[index + 2U]] - proxy.positions[proxy.indices[index]]);
        const double area = raw.length() * 0.5;
        if (area > kEpsilon) sum = sum + raw.normalized() * area;
    }
    return safe_normal(sum, {0.0, 0.0, 1.0});
}

NdfClosure proxy_ndf(const GeometryProxy& proxy) {
    const core::Vec3d reference = proxy_mean_normal(proxy);
    std::vector<NdfSample> samples;
    samples.reserve(proxy.triangle_count());
    for (std::size_t index = 0U; index < proxy.indices.size(); index += 3U) {
        const core::Vec3d raw = core::cross(
            proxy.positions[proxy.indices[index + 1U]] - proxy.positions[proxy.indices[index]],
            proxy.positions[proxy.indices[index + 2U]] - proxy.positions[proxy.indices[index]]);
        const double area = raw.length() * 0.5;
        if (area > kEpsilon) samples.push_back({raw.normalized(), area});
    }
    return fit_ndf(reference, samples);
}

VisibilityClosure proxy_visibility(core::Vec3d normal) {
    VisibilityClosure result;
    result.reference_normal = safe_normal(normal, {0.0, 0.0, 1.0});
    result.bent_normal = result.reference_normal;
    result.directional_visibility.fill(1.0F);
    result.ambient_visibility = 1.0;
    result.cavity = 0.0;
    return result;
}

struct Response {
    double diffuse = 0.0;
    double specular = 0.0;
};

Response evaluate_response(
    core::Vec3d normal,
    const NdfClosure& ndf,
    const VisibilityClosure& visibility,
    const RadiometricSample& sample) {
    normal = safe_normal(normal, {0.0, 0.0, 1.0});
    const core::Vec3d light = safe_normal(sample.light_direction, normal);
    const core::Vec3d view = safe_normal(sample.view_direction, normal);
    const core::Vec3d half = safe_normal(light + view, normal);
    const double visibility_scale = std::clamp(
        visibility.ambient_visibility *
            (0.5 + 0.5 * std::max(0.0, core::dot(visibility.bent_normal, light))),
        0.0,
        1.0);
    const double diffuse = std::max(0.0, core::dot(normal, light)) * visibility_scale;
    const double alpha = std::clamp(0.5 * (ndf.alpha_x + ndf.alpha_y), 0.001, 4.0);
    const double exponent = std::clamp(2.0 / (alpha * alpha), 1.0, 256.0);
    const double specular = std::pow(std::max(0.0, core::dot(normal, half)), exponent) *
        visibility_scale;
    return {diffuse, specular};
}

std::vector<RadiometricSample> make_samples(const RadiometricEvaluationOptions& options) {
    std::vector<RadiometricSample> result;
    result.reserve(static_cast<std::size_t>(options.view_count) * options.light_count);
    for (std::uint32_t view = 0U; view < options.view_count; ++view) {
        const double view_phi = 2.0 * kPi * static_cast<double>(view) /
            static_cast<double>(options.view_count);
        const double view_theta = kPi * (0.15 + 0.7 *
            (static_cast<double>(view) + 0.5) / static_cast<double>(options.view_count));
        const core::Vec3d view_direction{
            std::sin(view_theta) * std::cos(view_phi),
            std::sin(view_theta) * std::sin(view_phi),
            std::cos(view_theta),
        };
        for (std::uint32_t light = 0U; light < options.light_count; ++light) {
            const double light_phi = 2.0 * kPi * (static_cast<double>(light) + 0.5) /
                static_cast<double>(options.light_count);
            const double light_theta = kPi * (0.1 + 0.35 *
                (static_cast<double>(light) + 0.5) / static_cast<double>(options.light_count));
            result.push_back({
                view_direction,
                {
                    std::sin(light_theta) * std::cos(light_phi),
                    std::sin(light_theta) * std::sin(light_phi),
                    std::cos(light_theta),
                },
            });
        }
    }
    return result;
}

double normalized_position_error(
    const geometry::CompiledMesh& source,
    const std::vector<TriangleInfo>& source_triangles,
    const GeometryProxy& proxy) {
    if (source_triangles.empty() || proxy.triangle_count() == 0U) return 1.0;
    const core::Vec3d extent = source.bounds.maximum - source.bounds.minimum;
    const double scale = std::max(kEpsilon, extent.length());
    double maximum = 0.0;
    for (const TriangleInfo& source_triangle : source_triangles) {
        double nearest = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0U; index < proxy.indices.size(); index += 3U) {
            const core::Vec3d centroid = (
                proxy.positions[proxy.indices[index]] +
                proxy.positions[proxy.indices[index + 1U]] +
                proxy.positions[proxy.indices[index + 2U]]) * (1.0 / 3.0);
            nearest = std::min(nearest, (source_triangle.centroid - centroid).length());
        }
        maximum = std::max(maximum, nearest / scale);
    }
    return std::isfinite(maximum) ? maximum : 1.0;
}

bool has_id(const std::vector<RepresentationId>& ids, RepresentationId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

std::string SurfaceDomain::canonical() const {
    std::ostringstream output;
    output << "surface-domain-v1;id=" << id.value
           << ";revision=" << source_revision.value()
           << ";semantic=" << semantic_class
           << ";material=" << material_class
           << ";source=" << source_content_digest << ";faces=";
    for (const geometry::FaceId face : source_faces) output << face.value << ',';
    return output.str();
}

core::Result<void> SurfaceDomain::validate(const geometry::CompiledMesh& mesh) const {
    if (!mesh.valid()) {
        return core::Result<void>::failure(validation("surface domain source mesh is invalid"));
    }
    if (!id || source_revision.exhausted() || source_revision != mesh.source_revision) {
        return core::Result<void>::failure(validation(
            "surface domain source identity or revision is invalid"));
    }
    if (source_faces.empty() || !std::is_sorted(source_faces.begin(), source_faces.end()) ||
        std::adjacent_find(source_faces.begin(), source_faces.end()) != source_faces.end()) {
        return core::Result<void>::failure(validation(
            "surface domain source faces must be sorted, unique, and non-empty"));
    }
    const std::set<geometry::FaceId> mesh_faces(
        mesh.triangle_faces.begin(), mesh.triangle_faces.end());
    for (const geometry::FaceId face : source_faces) {
        if (!face || !mesh_faces.contains(face)) {
            return core::Result<void>::failure(validation(
                "surface domain references a face outside the source mesh"));
        }
    }
    if (semantic_class.size() > 256U || material_class.size() > 256U ||
        semantic_class.find('\n') != std::string::npos ||
        material_class.find('\n') != std::string::npos) {
        return core::Result<void>::failure(invalid(
            "surface domain semantic and material labels are not bounded"));
    }
    if (!valid_hex_digest(source_content_digest) ||
        source_content_digest != compiled_mesh_digest(mesh) ||
        !valid_hex_digest(digest) || digest != digest_hex(canonical())) {
        return core::Result<void>::failure(validation(
            "surface domain digest does not match canonical content"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceDomain::refresh_digest(const geometry::CompiledMesh& mesh) {
    if (!mesh.valid() || !id || source_revision.exhausted() ||
        source_revision != mesh.source_revision || source_faces.empty() ||
        !std::is_sorted(source_faces.begin(), source_faces.end()) ||
        std::adjacent_find(source_faces.begin(), source_faces.end()) != source_faces.end()) {
        return core::Result<void>::failure(validation(
            "surface domain cannot refresh digest from invalid source identity"));
    }
    source_content_digest = compiled_mesh_digest(mesh);
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<SurfaceDomain> build_surface_domain(
    const geometry::CompiledMesh& mesh,
    SurfaceDomainId id,
    std::string semantic_class,
    std::string material_class) {
    if (!mesh.valid() || !id) {
        return core::Result<SurfaceDomain>::failure(invalid(
            "surface domain builder requires a valid mesh and non-zero ID"));
    }
    std::vector<geometry::FaceId> faces = mesh.triangle_faces;
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    SurfaceDomain result{
        id,
        std::move(faces),
        mesh.source_revision,
        std::move(semantic_class),
        std::move(material_class),
        {},
        {},
    };
    if (auto refreshed = result.refresh_digest(mesh); !refreshed) {
        return core::Result<SurfaceDomain>::failure(refreshed.error());
    }
    return core::Result<SurfaceDomain>::success(std::move(result));
}

core::Result<void> SurfacePatch::validate() const {
    if (!domain || faces.empty() || !std::is_sorted(faces.begin(), faces.end()) ||
        std::adjacent_find(faces.begin(), faces.end()) != faces.end() ||
        !bounds.valid() || !finite_nonnegative(area) || area <= 0.0) {
        return core::Result<void>::failure(validation("surface patch is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> CurvatureStats::validate() const {
    if (!std::isfinite(mean_curvature) || !std::isfinite(gaussian_curvature) ||
        !finite_nonnegative(maximum_abs_curvature) || !principal_direction.finite() ||
        principal_direction.length_squared() <= kEpsilon) {
        return core::Result<void>::failure(validation("surface curvature statistics are invalid"));
    }
    return core::Result<void>::success();
}

std::uint64_t EdgeStats::total() const noexcept {
    return static_cast<std::uint64_t>(flat) + soft_convex + hard_convex + soft_concave +
        hard_concave + boundary + non_manifold;
}

core::Result<void> EdgeStats::validate() const {
    return core::Result<void>::success();
}

core::Result<void> ThicknessStats::validate() const {
    if (!finite_nonnegative(minimum) || !finite_nonnegative(maximum) ||
        !finite_nonnegative(mean) || minimum > maximum || !finite_unit(confidence)) {
        return core::Result<void>::failure(validation("surface thickness statistics are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> NdfClosure::validate() const {
    if (!reference_normal.finite() || reference_normal.length_squared() <= kEpsilon ||
        !std::isfinite(alpha_x) || !std::isfinite(alpha_y) || alpha_x <= 0.0 || alpha_y <= 0.0 ||
        alpha_x > 4.0 || alpha_y > 4.0 || !std::isfinite(orientation) ||
        !finite_nonnegative(fit_error)) {
        return core::Result<void>::failure(validation("NDF closure is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> VisibilityClosure::validate() const {
    if (!reference_normal.finite() || !bent_normal.finite() ||
        reference_normal.length_squared() <= kEpsilon || bent_normal.length_squared() <= kEpsilon ||
        !finite_unit(ambient_visibility) || !finite_unit(cavity) ||
        !finite_unit(silhouette_density) || !finite_nonnegative(rms_error) ||
        !finite_nonnegative(maximum_error)) {
        return core::Result<void>::failure(validation("visibility closure is invalid"));
    }
    for (const float value : directional_visibility) {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            return core::Result<void>::failure(validation(
                "visibility closure contains an invalid directional bin"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceAnalysisOptions::validate() const {
    if (max_source_triangles == 0U || max_source_triangles > 1'000'000U ||
        visibility_sample_count == 0U || visibility_sample_count > 4096U ||
        !std::isfinite(thickness_max_distance) || thickness_max_distance <= 0.0 ||
        !std::isfinite(soft_edge_angle) || soft_edge_angle < 0.0 || soft_edge_angle > kPi) {
        return core::Result<void>::failure(invalid("surface analysis options are invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceAnalysisOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "max=" << max_source_triangles
           << ";samples=" << visibility_sample_count
           << ";thickness=" << thickness_max_distance
           << ";soft_edge=" << soft_edge_angle;
    return output.str();
}

assets::Sha256Digest SurfaceAnalysisOptions::digest() const {
    return digest_text(canonical());
}

core::Result<void> SurfaceAnalysisRecord::validate(
    const geometry::CompiledMesh& mesh) const {
    if (auto result = domain.validate(mesh); !result) return result;
    if (auto result = patch.validate(); !result) return result;
    if (patch.domain != domain.id || patch.faces != domain.source_faces || !mean_normal.finite()) {
        return core::Result<void>::failure(validation(
            "surface analysis patch does not match its surface domain"));
    }
    if (auto result = curvature.validate(); !result) return result;
    if (auto result = thickness.validate(); !result) return result;
    if (auto result = edges.validate(); !result) return result;
    if (auto result = ndf.validate(); !result) return result;
    if (auto result = visibility.validate(); !result) return result;
    if (algorithm_version.empty() || !valid_hex_digest(digest) ||
        digest != digest_hex(canonical())) {
        return core::Result<void>::failure(validation(
            "surface analysis provenance is incomplete"));
    }
    return core::Result<void>::success();
}

std::string SurfaceAnalysisRecord::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-analysis-v1;domain=" << domain.digest
           << ";patch=" << patch.area << ':' << vec3_canonical(patch.bounds.minimum) << ':'
           << vec3_canonical(patch.bounds.maximum)
           << ";mean_normal=" << vec3_canonical(mean_normal)
           << ";curvature=" << curvature.mean_curvature << ',' << curvature.gaussian_curvature << ','
           << curvature.maximum_abs_curvature << ':' << vec3_canonical(curvature.principal_direction)
           << ";thickness=" << thickness.minimum << ',' << thickness.maximum << ',' << thickness.mean
           << ',' << thickness.confidence << ',' << thickness.measured_samples << ',' << thickness.ambiguous
           << ";edges=" << edges.flat << ',' << edges.soft_convex << ',' << edges.hard_convex << ','
           << edges.soft_concave << ',' << edges.hard_concave << ',' << edges.boundary << ','
           << edges.non_manifold
           << ";ndf=" << vec3_canonical(ndf.reference_normal) << ',' << ndf.alpha_x << ',' << ndf.alpha_y
           << ',' << ndf.orientation << ',' << ndf.fit_error
           << ";visibility=" << vec3_canonical(visibility.bent_normal) << ','
           << visibility.ambient_visibility << ',' << visibility.cavity << ','
           << visibility.silhouette_density;
    for (const float value : visibility.directional_visibility) output << ',' << value;
    output << ";algorithm=" << algorithm_version;
    return output.str();
}

core::Result<SurfaceAnalysisRecord> analyze_surface(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceAnalysisOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<SurfaceAnalysisRecord>::failure(result.error());
    }
    if (auto result = domain.validate(mesh); !result) {
        return core::Result<SurfaceAnalysisRecord>::failure(result.error());
    }
    const auto collected = collect_triangles(mesh, domain);
    if (!collected) return core::Result<SurfaceAnalysisRecord>::failure(collected.error());
    const auto& triangles = collected.value();
    if (triangles.size() > options.max_source_triangles) {
        return core::Result<SurfaceAnalysisRecord>::failure(unsupported(
            "bounded CPU surface analysis refuses an unbounded source triangle count"));
    }
    const core::Vec3d mean_normal = weighted_normal(triangles);
    SurfacePatch patch;
    patch.domain = domain.id;
    patch.faces = domain.source_faces;
    for (const TriangleInfo& triangle : triangles) {
        patch.area += triangle.area;
        patch.bounds.include(mesh.positions[triangle.vertices[0U]]);
        patch.bounds.include(mesh.positions[triangle.vertices[1U]]);
        patch.bounds.include(mesh.positions[triangle.vertices[2U]]);
    }

    std::map<std::uint32_t, double> vertex_angle;
    std::map<std::uint32_t, double> vertex_area;
    std::set<std::uint32_t> boundary_vertices;
    const auto edges = collect_edges(mesh, triangles);
    EdgeStats edge_stats;
    double mean_dihedral = 0.0;
    std::uint32_t dihedral_count = 0U;
    for (const TriangleInfo& triangle : triangles) {
        const auto& positions = mesh.positions;
        vertex_angle[triangle.vertices[0U]] += triangle_angle(
            positions[triangle.vertices[1U]], positions[triangle.vertices[2U]], positions[triangle.vertices[0U]]);
        vertex_angle[triangle.vertices[1U]] += triangle_angle(
            positions[triangle.vertices[2U]], positions[triangle.vertices[0U]], positions[triangle.vertices[1U]]);
        vertex_angle[triangle.vertices[2U]] += triangle_angle(
            positions[triangle.vertices[0U]], positions[triangle.vertices[1U]], positions[triangle.vertices[2U]]);
        vertex_area[triangle.vertices[0U]] += triangle.area / 3.0;
        vertex_area[triangle.vertices[1U]] += triangle.area / 3.0;
        vertex_area[triangle.vertices[2U]] += triangle.area / 3.0;
    }
    for (const auto& [edge, incidents] : edges) {
        if (incidents.size() == 1U) {
            ++edge_stats.boundary;
            boundary_vertices.insert(incidents.front().first_vertex);
            boundary_vertices.insert(incidents.front().second_vertex);
            continue;
        }
        if (incidents.size() > 2U) {
            ++edge_stats.non_manifold;
            continue;
        }
        const double angle = std::acos(clamp_dot(incidents[0U].normal, incidents[1U].normal));
        const bool flat = angle <= 1e-6;
        const bool soft = angle <= options.soft_edge_angle;
        if (flat) {
            ++edge_stats.flat;
        } else {
            const core::Vec3d direction = safe_normal(
                mesh.positions[incidents[0U].second_vertex] -
                    mesh.positions[incidents[0U].first_vertex],
                {1.0, 0.0, 0.0});
            const bool convex = core::dot(
                core::cross(direction, incidents[0U].normal), incidents[1U].normal) >= 0.0;
            if (convex && soft) ++edge_stats.soft_convex;
            else if (convex) ++edge_stats.hard_convex;
            else if (soft) ++edge_stats.soft_concave;
            else ++edge_stats.hard_concave;
        }
        mean_dihedral += angle;
        ++dihedral_count;
    }

    double gaussian_sum = 0.0;
    double maximum_abs_curvature = 0.0;
    for (const auto& [vertex, angle_sum] : vertex_angle) {
        const double defect = (boundary_vertices.contains(vertex) ? kPi : 2.0 * kPi) - angle_sum;
        const double curvature = vertex_area[vertex] > kEpsilon
            ? defect / vertex_area[vertex]
            : 0.0;
        gaussian_sum += curvature;
        maximum_abs_curvature = std::max(maximum_abs_curvature, std::abs(curvature));
    }
    const double vertex_count = static_cast<double>(std::max<std::size_t>(1U, vertex_angle.size()));
    CurvatureStats curvature{
        dihedral_count == 0U ? 0.0 : mean_dihedral / static_cast<double>(dihedral_count),
        gaussian_sum / vertex_count,
        maximum_abs_curvature,
        make_basis(mean_normal).tangent,
    };
    std::vector<NdfSample> ndf_samples;
    ndf_samples.reserve(triangles.size());
    for (const TriangleInfo& triangle : triangles) ndf_samples.push_back({triangle.normal, triangle.area});
    SurfaceAnalysisRecord record{
        domain,
        patch,
        mean_normal,
        curvature,
        build_thickness(mesh, triangles, options),
        edge_stats,
        fit_ndf(mean_normal, ndf_samples),
        build_visibility(mesh, triangles, options, mean_normal, edges),
        "cartographer-surface-analysis/0.1",
        {},
    };
    record.digest = digest_hex(record.canonical());
    return core::Result<SurfaceAnalysisRecord>::success(std::move(record));
}

core::Result<void> SurfaceBand::validate() const {
    if (!std::isfinite(min_frequency) || !std::isfinite(max_frequency) ||
        min_frequency < 0.0 || max_frequency <= min_frequency ||
        !finite_nonnegative(rms_displacement) || !finite_nonnegative(rms_normal_change) ||
        !finite_unit(silhouette_contribution)) {
        return core::Result<void>::failure(validation("surface frequency band is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> FrequencyDecompositionOptions::validate() const {
    if (!std::isfinite(feature_scale) || feature_scale <= 0.0) {
        return core::Result<void>::failure(invalid("frequency decomposition scale is invalid"));
    }
    return core::Result<void>::success();
}

std::string FrequencyDecompositionOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << "feature_scale=" << feature_scale;
    return output.str();
}

core::Result<std::vector<SurfaceBand>> decompose_frequency(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const FrequencyDecompositionOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<std::vector<SurfaceBand>>::failure(result.error());
    }
    if (auto result = domain.validate(mesh); !result) {
        return core::Result<std::vector<SurfaceBand>>::failure(result.error());
    }
    const auto collected = collect_triangles(mesh, domain);
    if (!collected) return core::Result<std::vector<SurfaceBand>>::failure(collected.error());
    const auto& triangles = collected.value();
    const core::Vec3d normal = weighted_normal(triangles);
    core::Vec3d center{};
    for (const TriangleInfo& triangle : triangles) center = center + triangle.centroid;
    center = center * (1.0 / static_cast<double>(triangles.size()));
    const std::array<std::pair<double, double>, 4U> ranges = {{
        {0.0, 0.25}, {0.25, 1.0}, {1.0, 4.0}, {4.0, std::numeric_limits<double>::max()},
    }};
    std::vector<SurfaceBand> bands;
    for (const auto& [minimum, maximum] : ranges) bands.push_back({minimum, maximum});
    const auto edges = collect_edges(mesh, triangles);
    for (const TriangleInfo& triangle : triangles) {
        const double frequency = options.feature_scale /
            std::max(kEpsilon, std::sqrt(triangle.area));
        std::size_t band = frequency < 0.25 ? 0U : frequency < 1.0 ? 1U : frequency < 4.0 ? 2U : 3U;
        const double displacement = std::abs(core::dot(triangle.centroid - center, normal));
        const double normal_change = std::acos(clamp_dot(triangle.normal, normal));
        const bool boundary = std::any_of(
            mesh.triangle_edges[triangle.source_index].begin(),
            mesh.triangle_edges[triangle.source_index].end(),
            [&edges](const auto& edge) {
                return edge.has_value() && edges.at(*edge).size() == 1U;
            });
        SurfaceBand& target = bands[band];
        const double count = static_cast<double>(target.triangle_count);
        target.rms_displacement = std::sqrt((target.rms_displacement * target.rms_displacement * count +
            displacement * displacement) / (count + 1.0));
        target.rms_normal_change = std::sqrt((target.rms_normal_change * target.rms_normal_change * count +
            normal_change * normal_change) / (count + 1.0));
        target.silhouette_contribution = (target.silhouette_contribution * count +
            (boundary ? 1.0 : 0.0)) / (count + 1.0);
        ++target.triangle_count;
    }
    return core::Result<std::vector<SurfaceBand>>::success(std::move(bands));
}

core::Result<void> GeometryProxy::validate(const geometry::CompiledMesh& source) const {
    if (!source.valid() || positions.size() != source.positions.size() ||
        normals.size() != source.normals.size() || indices.empty() || indices.size() % 3U != 0U ||
        source_triangles.size() != indices.size() / 3U || source_triangles.empty()) {
        return core::Result<void>::failure(validation("geometry proxy cardinality is invalid"));
    }
    const auto same_vectors = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) return false;
        for (std::size_t index = 0U; index < left.size(); ++index) {
            if (left[index].x != right[index].x || left[index].y != right[index].y ||
                left[index].z != right[index].z) {
                return false;
            }
        }
        return true;
    };
    if (!same_vectors(positions, source.positions) || !same_vectors(normals, source.normals)) {
        return core::Result<void>::failure(validation(
            "geometry proxy must remain bound to its source vertex realization"));
    }
    if (!std::is_sorted(source_triangles.begin(), source_triangles.end()) ||
        std::adjacent_find(source_triangles.begin(), source_triangles.end()) != source_triangles.end()) {
        return core::Result<void>::failure(validation("geometry proxy triangle identities are not deterministic"));
    }
    for (const std::uint32_t triangle : source_triangles) {
        if (triangle >= source.indices.size() / 3U) {
            return core::Result<void>::failure(validation(
                "geometry proxy references a missing source triangle"));
        }
    }
    for (const std::uint32_t index : indices) {
        if (index >= positions.size()) {
            return core::Result<void>::failure(validation("geometry proxy index is out of range"));
        }
    }
    return core::Result<void>::success();
}

core::Result<GeometryProxy> build_geometry_proxy(
    const geometry::CompiledMesh& mesh,
    std::uint32_t target_triangles) {
    if (!mesh.valid() || target_triangles == 0U ||
        target_triangles > mesh.indices.size() / 3U) {
        return core::Result<GeometryProxy>::failure(invalid(
            "geometry proxy target must be within the source triangle count"));
    }
    GeometryProxy result;
    result.positions = mesh.positions;
    result.normals = mesh.normals;
    const std::size_t source_count = mesh.indices.size() / 3U;
    result.source_triangles.reserve(target_triangles);
    result.indices.reserve(static_cast<std::size_t>(target_triangles) * 3U);
    for (std::uint32_t index = 0U; index < target_triangles; ++index) {
        const std::size_t selected = std::min(
            source_count - 1U,
            (static_cast<std::size_t>(index) * source_count + target_triangles / 2U) /
                static_cast<std::size_t>(target_triangles));
        result.source_triangles.push_back(static_cast<std::uint32_t>(selected));
        result.indices.insert(
            result.indices.end(),
            mesh.indices.begin() + static_cast<std::ptrdiff_t>(selected * 3U),
            mesh.indices.begin() + static_cast<std::ptrdiff_t>(selected * 3U + 3U));
    }
    if (auto valid = result.validate(mesh); !valid) {
        return core::Result<GeometryProxy>::failure(valid.error());
    }
    return core::Result<GeometryProxy>::success(std::move(result));
}

core::Result<void> SurfaceErrorEnvelope::validate() const {
    if (!finite_nonnegative(max_position_error) || !finite_nonnegative(max_normal_error) ||
        !finite_nonnegative(max_silhouette_error) || !finite_nonnegative(rms_radiance_error) ||
        !finite_nonnegative(max_radiance_error) || !finite_nonnegative(diffuse_error) ||
        !finite_nonnegative(specular_error) || !finite_nonnegative(visibility_error) ||
        !finite_nonnegative(energy_error)) {
        return core::Result<void>::failure(validation("surface error envelope is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceQualityPolicy::validate() const {
    if (!finite_nonnegative(max_rms_radiance_error) || !finite_nonnegative(max_radiance_error) ||
        !finite_nonnegative(max_diffuse_error) || !finite_nonnegative(max_specular_error) ||
        !finite_nonnegative(max_visibility_error) || !finite_nonnegative(max_silhouette_error)) {
        return core::Result<void>::failure(invalid("surface quality policy is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceQualityPolicy::canonical() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17)
           << max_rms_radiance_error << ',' << max_radiance_error << ',' << max_diffuse_error << ','
           << max_specular_error << ',' << max_visibility_error << ',' << max_silhouette_error;
    return output.str();
}

assets::Sha256Digest SurfaceQualityPolicy::digest() const {
    return digest_text(canonical());
}

core::Result<void> RadiometricSample::validate() const {
    if (!view_direction.finite() || !light_direction.finite() ||
        view_direction.length_squared() <= kEpsilon || light_direction.length_squared() <= kEpsilon) {
        return core::Result<void>::failure(invalid("radiometric sample directions are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> RadiometricEvaluationOptions::validate() const {
    if (view_count == 0U || view_count > 64U || light_count == 0U || light_count > 64U ||
        !finite_nonnegative(specular_weight)) {
        return core::Result<void>::failure(invalid("radiometric evaluation options are invalid"));
    }
    return core::Result<void>::success();
}

std::string RadiometricEvaluationOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17) << view_count << ',' << light_count << ',' << specular_weight;
    return output.str();
}

assets::Sha256Digest RadiometricEvaluationOptions::digest() const {
    return digest_text(canonical());
}

core::Result<void> RadiometricEvaluation::validate() const {
    if (sample_count == 0U || !valid_hex_digest(policy_digest) || !valid_hex_digest(source_digest) ||
        !valid_hex_digest(candidate_digest)) {
        return core::Result<void>::failure(validation("radiometric evaluation receipt is incomplete"));
    }
    return envelope.validate();
}

core::Result<void> SurfaceClosureCompileOptions::validate() const {
    if (target_triangles == 0U) {
        return core::Result<void>::failure(invalid("surface closure target triangle count is zero"));
    }
    if (auto result = analysis.validate(); !result) return result;
    return evaluation.validate();
}

std::string SurfaceClosureCompileOptions::canonical() const {
    std::ostringstream output;
    output << "target=" << target_triangles << ";analysis=" << analysis.canonical()
           << ";evaluation=" << evaluation.canonical();
    return output.str();
}

std::string SurfaceClosurePolygon::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-closure-v1;domain=" << domain.value
           << ";representation=" << representation.value
           << ";kind=" << static_cast<int>(kind)
           << ";mode=" << static_cast<int>(mode)
           << ";revision=" << source_revision.value()
           << ";algorithm=" << algorithm_version
           << ";triangles=";
    for (const std::uint32_t triangle : geometry.source_triangles) output << triangle << ',';
    output << ";indices=";
    for (const std::uint32_t index : geometry.indices) output << index << ',';
    output << ";ndf=" << vec3_canonical(ndf.reference_normal) << ',' << ndf.alpha_x << ',' << ndf.alpha_y
           << ',' << ndf.orientation << ',' << ndf.fit_error
           << ";visibility=" << vec3_canonical(visibility.reference_normal) << ','
           << vec3_canonical(visibility.bent_normal) << ',' << visibility.ambient_visibility << ','
           << visibility.cavity << ',' << visibility.silhouette_density;
    for (const float value : visibility.directional_visibility) output << ',' << value;
    output << ";error=" << error.max_position_error << ',' << error.max_normal_error << ','
           << error.max_silhouette_error << ',' << error.rms_radiance_error << ','
           << error.max_radiance_error << ',' << error.diffuse_error << ',' << error.specular_error << ','
           << error.visibility_error << ',' << error.energy_error << ";faces=";
    for (const geometry::FaceId face : source_faces) output << face.value << ',';
    return output.str();
}

core::Result<void> SurfaceClosurePolygon::refresh_digest() {
    if (!domain || !representation || source_revision.exhausted() || source_faces.empty() ||
        algorithm_version.empty() || !valid_representation_kind(kind) ||
        !valid_representation_mode(mode)) {
        return core::Result<void>::failure(validation(
            "surface closure cannot refresh digest from incomplete provenance"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceClosurePolygon::validate(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& source_domain) const {
    if (auto result = source_domain.validate(source); !result) return result;
    if (domain != source_domain.id || source_revision != source_domain.source_revision ||
        source_faces != source_domain.source_faces || !representation || algorithm_version.empty() ||
        !valid_representation_kind(kind) || !valid_representation_mode(mode) ||
        !valid_hex_digest(digest) || digest != digest_hex(canonical())) {
        return core::Result<void>::failure(validation(
            "surface closure provenance does not match its source domain"));
    }
    if (auto result = geometry.validate(source); !result) return result;
    if (auto result = ndf.validate(); !result) return result;
    if (auto result = visibility.validate(); !result) return result;
    return error.validate();
}

core::Result<SurfaceClosurePolygon> compile_surface_representation(
    const geometry::CompiledMesh& mesh,
    const SurfaceDomain& domain,
    const SurfaceClosureCompileOptions& options,
    RepresentationMode mode) {
    if (!valid_representation_mode(mode)) {
        return core::Result<SurfaceClosurePolygon>::failure(
            invalid("surface closure representation mode is invalid"));
    }
    if (auto result = options.validate(); !result) {
        return core::Result<SurfaceClosurePolygon>::failure(result.error());
    }
    if (auto result = domain.validate(mesh); !result) {
        return core::Result<SurfaceClosurePolygon>::failure(result.error());
    }
    const std::size_t source_triangles = mesh.indices.size() / 3U;
    if (options.target_triangles > source_triangles) {
        return core::Result<SurfaceClosurePolygon>::failure(invalid(
            "surface closure target exceeds source triangle count"));
    }
    const auto analysis = analyze_surface(mesh, domain, options.analysis);
    if (!analysis) return core::Result<SurfaceClosurePolygon>::failure(analysis.error());
    const auto proxy = build_geometry_proxy(mesh, options.target_triangles);
    if (!proxy) return core::Result<SurfaceClosurePolygon>::failure(proxy.error());
    const std::string representation_seed = domain.digest + ";" + options.canonical() + ";mode=" +
        std::to_string(static_cast<int>(mode));
    const std::string representation_value = digest_prefix_id(representation_seed);
    SurfaceClosurePolygon result{
        domain.id,
        RepresentationId{static_cast<std::uint64_t>(std::stoull(representation_value))},
        mode == RepresentationMode::fidelity_closure
            ? SurfaceRepresentationKind::closure_proxy
            : SurfaceRepresentationKind::simplified_geometry,
        mode,
        proxy.value(),
        mode == RepresentationMode::fidelity_closure ? analysis.value().ndf : proxy_ndf(proxy.value()),
        mode == RepresentationMode::fidelity_closure
            ? analysis.value().visibility
            : proxy_visibility(proxy_mean_normal(proxy.value())),
        {},
        domain.source_faces,
        domain.source_revision,
        mode == RepresentationMode::fidelity_closure
            ? "cartographer-surface-closure/0.1"
            : "cartographer-surface-baseline/0.1",
        {},
    };
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceClosurePolygon>::failure(refreshed.error());
    }
    const auto evaluation = evaluate_radiometric_error(mesh, domain, result, options.evaluation);
    if (!evaluation) return core::Result<SurfaceClosurePolygon>::failure(evaluation.error());
    result.error = evaluation.value().envelope;
    if (auto refreshed = result.refresh_digest(); !refreshed) {
        return core::Result<SurfaceClosurePolygon>::failure(refreshed.error());
    }
    if (auto valid = result.validate(mesh, domain); !valid) {
        return core::Result<SurfaceClosurePolygon>::failure(valid.error());
    }
    return core::Result<SurfaceClosurePolygon>::success(std::move(result));
}

core::Result<RadiometricEvaluation> evaluate_radiometric_error(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceClosurePolygon& candidate,
    const RadiometricEvaluationOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<RadiometricEvaluation>::failure(result.error());
    }
    if (auto result = candidate.validate(source, domain); !result) {
        return core::Result<RadiometricEvaluation>::failure(result.error());
    }
    SurfaceAnalysisOptions analysis_options;
    analysis_options.max_source_triangles = std::max<std::uint32_t>(
        analysis_options.max_source_triangles,
        static_cast<std::uint32_t>(source.indices.size() / 3U));
    analysis_options.visibility_sample_count = 32U;
    const auto source_analysis = analyze_surface(source, domain, analysis_options);
    if (!source_analysis) return core::Result<RadiometricEvaluation>::failure(source_analysis.error());
    const auto source_triangles = collect_triangles(source, domain);
    if (!source_triangles) return core::Result<RadiometricEvaluation>::failure(source_triangles.error());
    const core::Vec3d candidate_normal = candidate.mode == RepresentationMode::fidelity_closure
        ? candidate.ndf.reference_normal
        : proxy_mean_normal(candidate.geometry);
    const auto samples = make_samples(options);
    double squared_sum = 0.0;
    double maximum = 0.0;
    double diffuse_sum = 0.0;
    double specular_sum = 0.0;
    double source_energy = 0.0;
    double candidate_energy = 0.0;
    for (const RadiometricSample& sample : samples) {
        const Response source_response = evaluate_response(
            source_analysis.value().mean_normal,
            source_analysis.value().ndf,
            source_analysis.value().visibility,
            sample);
        const Response candidate_response = evaluate_response(
            candidate_normal, candidate.ndf, candidate.visibility, sample);
        const double diffuse_error = std::abs(candidate_response.diffuse - source_response.diffuse);
        const double specular_error = std::abs(candidate_response.specular - source_response.specular) *
            options.specular_weight;
        const double radiance_error = diffuse_error + specular_error;
        squared_sum += radiance_error * radiance_error;
        maximum = std::max(maximum, radiance_error);
        diffuse_sum += diffuse_error;
        specular_sum += specular_error;
        source_energy += source_response.diffuse + source_response.specular * options.specular_weight;
        candidate_energy += candidate_response.diffuse + candidate_response.specular * options.specular_weight;
    }
    const double count = static_cast<double>(samples.size());
    const double visibility_error = std::max(
        std::abs(candidate.visibility.ambient_visibility - source_analysis.value().visibility.ambient_visibility),
        std::abs(candidate.visibility.cavity - source_analysis.value().visibility.cavity));
    const double energy_scale = std::max(kEpsilon, source_energy / count);
    SurfaceErrorEnvelope envelope{
        normalized_position_error(source, source_triangles.value(), candidate.geometry),
        std::acos(clamp_dot(candidate_normal, source_analysis.value().mean_normal)),
        1.0 - static_cast<double>(candidate.geometry.triangle_count()) /
            static_cast<double>(source_triangles.value().size()),
        std::sqrt(squared_sum / count),
        maximum,
        diffuse_sum / count,
        specular_sum / count,
        visibility_error,
        std::abs(candidate_energy - source_energy) / energy_scale,
    };
    RadiometricEvaluation result{
        envelope,
        static_cast<std::uint32_t>(samples.size()),
        options.digest().hex(),
        source_analysis.value().digest,
        candidate.digest,
    };
    return core::Result<RadiometricEvaluation>::success(std::move(result));
}

core::Result<void> admit_surface_representation(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceClosurePolygon& candidate,
    const SurfaceQualityPolicy& policy) {
    if (auto result = policy.validate(); !result) return result;
    if (auto result = candidate.validate(source, domain); !result) return result;
    const SurfaceErrorEnvelope& error = candidate.error;
    if (error.rms_radiance_error > policy.max_rms_radiance_error ||
        error.max_radiance_error > policy.max_radiance_error ||
        error.diffuse_error > policy.max_diffuse_error ||
        error.specular_error > policy.max_specular_error ||
        error.visibility_error > policy.max_visibility_error ||
        error.max_silhouette_error > policy.max_silhouette_error) {
        return core::Result<void>::failure(validation(
            "surface representation exceeds its appearance conservation policy"));
    }
    return core::Result<void>::success();
}

core::Result<void> RepresentationMigration::validate() const {
    if (!domain || !source_representation || !target_representation ||
        source_representation == target_representation || targets.empty() ||
        !std::all_of(targets.begin(), targets.end(), valid_migration_target) ||
        !std::is_sorted(targets.begin(), targets.end()) ||
        std::adjacent_find(targets.begin(), targets.end()) != targets.end() ||
        !finite_nonnegative(residual_radiometric_error) || source_revision.exhausted() ||
        algorithm_version.empty() || !valid_hex_digest(digest) ||
        digest != digest_hex(canonical())) {
        return core::Result<void>::failure(validation("surface representation migration is invalid"));
    }
    return core::Result<void>::success();
}

std::string RepresentationMigration::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-migration-v1;domain=" << domain.value
           << ";source=" << source_representation.value
           << ";target=" << target_representation.value
           << ";revision=" << source_revision.value()
           << ";residual=" << residual_radiometric_error
           << ";algorithm=" << algorithm_version << ";targets=";
    for (const MigrationTarget target : targets) output << static_cast<int>(target) << ',';
    return output.str();
}

core::Result<void> RepresentationMigration::refresh_digest() {
    if (!domain || !source_representation || !target_representation ||
        source_representation == target_representation || targets.empty() ||
        !std::all_of(targets.begin(), targets.end(), valid_migration_target) ||
        !std::is_sorted(targets.begin(), targets.end()) ||
        std::adjacent_find(targets.begin(), targets.end()) != targets.end() ||
        !finite_nonnegative(residual_radiometric_error) || source_revision.exhausted() ||
        algorithm_version.empty()) {
        return core::Result<void>::failure(validation(
            "surface representation migration cannot refresh from incomplete data"));
    }
    digest = digest_hex(canonical());
    return core::Result<void>::success();
}

core::Result<void> SurfaceRepresentationNode::validate() const {
    if (!id || !domain || has_id(children, id) ||
        std::adjacent_find(children.begin(), children.end()) != children.end() ||
        !std::is_sorted(children.begin(), children.end()) ||
        (parent.has_value() && (!*parent || *parent == id)) ||
        !valid_representation_kind(kind) ||
        !valid_hex_digest(content_digest)) {
        return core::Result<void>::failure(validation("surface representation node is invalid"));
    }
    return error.validate();
}

core::Result<void> SurfaceRepresentationTree::add(SurfaceRepresentationNode node) {
    if (auto result = node.validate(); !result) return result;
    if (std::any_of(nodes_.begin(), nodes_.end(), [&node](const auto& existing) {
            return existing.id == node.id;
        })) {
        return core::Result<void>::failure(invalid("surface representation ID is duplicated"));
    }
    nodes_.push_back(std::move(node));
    std::sort(nodes_.begin(), nodes_.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return core::Result<void>::success();
}

core::Result<void> SurfaceRepresentationTree::validate(SurfaceDomainId domain) const {
    if (!domain || nodes_.empty()) {
        return core::Result<void>::failure(validation("surface representation tree is empty"));
    }
    std::map<RepresentationId, const SurfaceRepresentationNode*> by_id;
    for (const auto& node : nodes_) {
        if (auto result = node.validate(); !result) return result;
        if (node.domain != domain || !by_id.emplace(node.id, &node).second) {
            return core::Result<void>::failure(validation(
                "surface representation tree has an invalid domain or duplicate node"));
        }
    }
    std::size_t roots = 0U;
    for (const auto& node : nodes_) {
        if (!node.parent.has_value()) {
            ++roots;
        } else {
            const auto parent = by_id.find(*node.parent);
            if (parent == by_id.end() || !has_id(parent->second->children, node.id)) {
                return core::Result<void>::failure(validation(
                    "surface representation parent/child links are not reciprocal"));
            }
        }
        for (const RepresentationId child : node.children) {
            const auto child_node = by_id.find(child);
            if (child_node == by_id.end() || !child_node->second->parent.has_value() ||
                *child_node->second->parent != node.id) {
                return core::Result<void>::failure(validation(
                    "surface representation child link is not reciprocal"));
            }
        }
        std::set<RepresentationId> seen;
        const SurfaceRepresentationNode* current = &node;
        while (current->parent.has_value()) {
            if (!seen.insert(current->id).second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::cycle_detected,
                    "surface representation tree contains a cycle"));
            }
            current = by_id.at(*current->parent);
        }
    }
    if (roots != 1U) {
        return core::Result<void>::failure(validation(
            "surface representation tree must have exactly one root"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceViewSelectionOptions::validate() const {
    if (!std::isfinite(minimum_view_depth) || minimum_view_depth <= 0.0 ||
        minimum_view_depth > 1.0e12 ||
        !finite_nonnegative(maximum_projected_radius_pixels) ||
        maximum_projected_radius_pixels > 1.0e9 ||
        !std::isfinite(vertical_fov_radians) ||
        vertical_fov_radians <= 1.0e-6 || vertical_fov_radians >= kPi - 1.0e-6 ||
        viewport_height_pixels == 0U || viewport_height_pixels > 32'768U ||
        !finite_nonnegative(maximum_estimated_position_error_pixels) ||
        maximum_estimated_position_error_pixels > 1.0e6) {
        return core::Result<void>::failure(invalid(
            "surface view selection camera projection or error budget is invalid"));
    }
    return quality_policy.validate();
}

std::string SurfaceViewSelectionOptions::canonical() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "surface-view-selection-options-v1\n"
        << "minimum_view_depth=" << minimum_view_depth << '\n'
        << "maximum_projected_radius_pixels=" << maximum_projected_radius_pixels << '\n'
        << "vertical_fov_radians=" << vertical_fov_radians << '\n'
        << "viewport_height_pixels=" << viewport_height_pixels << '\n'
        << "maximum_estimated_position_error_pixels=" << maximum_estimated_position_error_pixels << '\n'
        << "quality_policy=" << quality_policy.canonical() << '\n';
    return output.str();
}

assets::Sha256Digest SurfaceViewSelectionOptions::digest() const {
    return digest_text(canonical());
}

std::string SurfaceViewSelection::canonical() const {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "surface-view-selection-v1\n"
        << "domain=" << domain.value << '\n'
        << "representation=" << representation.value << '\n'
        << "source_revision=" << source_revision.value() << '\n'
        << "domain_digest=" << domain_digest << '\n'
        << "representation_digest=" << representation_digest << '\n'
        << "candidate_set_digest=" << candidate_set_digest << '\n'
        << "options_digest=" << options_digest << '\n'
        << "triangle_count=" << triangle_count << '\n'
        << "estimated_projected_position_error_pixels=" << estimated_projected_position_error_pixels << '\n';
    return output.str();
}

core::Result<SurfaceViewSelection> select_surface_representation_for_view(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& domain,
    const SurfaceRepresentationTree& tree,
    const std::vector<SurfaceClosurePolygon>& candidates,
    const SurfaceViewSelectionOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<SurfaceViewSelection>::failure(valid.error());
    }
    if (auto valid = domain.validate(source); !valid) {
        return core::Result<SurfaceViewSelection>::failure(valid.error());
    }
    if (auto valid = tree.validate(domain.id); !valid) {
        return core::Result<SurfaceViewSelection>::failure(valid.error());
    }
    if (candidates.empty() || candidates.size() > kMaxSurfaceViewSelectionCandidates) {
        return core::Result<SurfaceViewSelection>::failure(invalid(
            "surface view selection candidate count is outside its supported bound"));
    }

    const double focal_pixels = static_cast<double>(options.viewport_height_pixels) /
        (2.0 * std::tan(options.vertical_fov_radians * 0.5));
    if (!std::isfinite(focal_pixels) || focal_pixels <= 0.0) {
        return core::Result<SurfaceViewSelection>::failure(invalid(
            "surface view selection projection is not finite"));
    }

    struct CandidateEvaluation {
        const SurfaceClosurePolygon* polygon = nullptr;
        std::uint32_t triangle_count = 0U;
        double projected_error = 0.0;
        bool quality_admitted = false;
        bool within_view_budget = false;
    };
    std::vector<CandidateEvaluation> evaluated;
    evaluated.reserve(candidates.size());
    std::set<RepresentationId> seen;
    std::map<RepresentationId, const SurfaceRepresentationNode*> nodes;
    for (const auto& node : tree.nodes()) nodes.emplace(node.id, &node);

    for (const SurfaceClosurePolygon& candidate : candidates) {
        if (auto valid = candidate.validate(source, domain); !valid) {
            return core::Result<SurfaceViewSelection>::failure(valid.error());
        }
        if (!seen.insert(candidate.representation).second) {
            return core::Result<SurfaceViewSelection>::failure(invalid(
                "surface view selection contains a duplicate representation"));
        }
        const auto node = nodes.find(candidate.representation);
        if (node == nodes.end() || node->second->kind != candidate.kind ||
            node->second->content_digest != candidate.digest ||
            node->second->error.max_position_error != candidate.error.max_position_error ||
            node->second->error.max_normal_error != candidate.error.max_normal_error ||
            node->second->error.max_silhouette_error != candidate.error.max_silhouette_error ||
            node->second->error.rms_radiance_error != candidate.error.rms_radiance_error ||
            node->second->error.max_radiance_error != candidate.error.max_radiance_error ||
            node->second->error.diffuse_error != candidate.error.diffuse_error ||
            node->second->error.specular_error != candidate.error.specular_error ||
            node->second->error.visibility_error != candidate.error.visibility_error ||
            node->second->error.energy_error != candidate.error.energy_error) {
            return core::Result<SurfaceViewSelection>::failure(validation(
                "surface view selection candidate does not match its tree receipt"));
        }
        const std::size_t triangles = candidate.geometry.triangle_count();
        if (triangles == 0U || triangles > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<SurfaceViewSelection>::failure(invalid(
                "surface view selection candidate has an invalid triangle count"));
        }

        const core::Vec3d source_extent = source.bounds.maximum - source.bounds.minimum;
        const double source_scale = std::max(kEpsilon, source_extent.length());
        const double world_error = candidate.error.max_position_error * source_scale;
        double projected_error = std::numeric_limits<double>::infinity();
        if (world_error < options.minimum_view_depth) {
            projected_error = world_error *
                (focal_pixels + options.maximum_projected_radius_pixels) /
                (options.minimum_view_depth - world_error);
        }
        const bool quality_admitted = static_cast<bool>(admit_surface_representation(
            source, domain, candidate, options.quality_policy));
        const bool within_view_budget = std::isfinite(projected_error) &&
            projected_error <= options.maximum_estimated_position_error_pixels;
        evaluated.push_back({
            &candidate,
            static_cast<std::uint32_t>(triangles),
            projected_error,
            quality_admitted,
            within_view_budget,
        });
    }

    std::vector<std::string> candidate_records;
    candidate_records.reserve(evaluated.size());
    for (const CandidateEvaluation& candidate : evaluated) {
        std::ostringstream record;
        record.imbue(std::locale::classic());
        record << std::setprecision(std::numeric_limits<double>::max_digits10)
            << "candidate=" << candidate.polygon->representation.value << ','
            << candidate.polygon->digest << ',' << candidate.triangle_count << ','
            << candidate.polygon->error.max_position_error << ',';
        if (std::isfinite(candidate.projected_error)) record << candidate.projected_error;
        else record << "unbounded";
        record << ",quality=" << (candidate.quality_admitted ? 1 : 0)
            << ",view=" << (candidate.within_view_budget ? 1 : 0) << '\n';
        candidate_records.push_back(record.str());
    }
    std::sort(candidate_records.begin(), candidate_records.end());
    std::string candidate_set_material = "surface-view-candidate-set-v1\n";
    for (const std::string& record : candidate_records) candidate_set_material.append(record);
    const std::string candidate_set_digest = digest_hex(candidate_set_material);

    const CandidateEvaluation* selected = nullptr;
    for (const CandidateEvaluation& candidate : evaluated) {
        if (!candidate.quality_admitted || !candidate.within_view_budget) continue;
        if (selected == nullptr || candidate.triangle_count < selected->triangle_count ||
            (candidate.triangle_count == selected->triangle_count &&
             (candidate.projected_error < selected->projected_error ||
              (candidate.projected_error == selected->projected_error &&
               candidate.polygon->representation < selected->polygon->representation)))) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        return core::Result<SurfaceViewSelection>::failure(validation(
            "no current surface representation satisfies the view and appearance budgets"));
    }

    SurfaceViewSelection result{
        domain.id,
        selected->polygon->representation,
        domain.source_revision,
        domain.digest,
        selected->polygon->digest,
        candidate_set_digest,
        options.digest().hex(),
        selected->triangle_count,
        selected->projected_error,
        {},
    };
    result.digest = digest_hex(result.canonical());
    return core::Result<SurfaceViewSelection>::success(std::move(result));
}

core::Result<void> SurfaceViewSelection::validate(
    const geometry::CompiledMesh& source,
    const SurfaceDomain& source_domain,
    const SurfaceRepresentationTree& tree,
    const std::vector<SurfaceClosurePolygon>& candidates,
    const SurfaceViewSelectionOptions& options) const {
    if (!valid_hex_digest(digest) || digest != digest_hex(canonical())) {
        return core::Result<void>::failure(validation(
            "surface view selection digest is malformed or stale"));
    }
    const auto current = select_surface_representation_for_view(
        source, source_domain, tree, candidates, options);
    if (!current) return core::Result<void>::failure(current.error());
    if (canonical() != current.value().canonical() || digest != current.value().digest) {
        return core::Result<void>::failure(validation(
            "surface view selection no longer matches current camera or candidates"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceRuntimeRecord::validate() const {
    if (!domain || !representation || !finite_nonnegative(geometric_error) ||
        !finite_nonnegative(radiometric_error) || continuity_id == 0U) {
        return core::Result<void>::failure(validation("surface runtime record is invalid"));
    }
    return core::Result<void>::success();
}

std::string FidelityPackage::canonical_without_digest() const {
    std::ostringstream output;
    output << "cartographer-fidelity-package-v2;schema=" << schema_version
           << ";revision=" << source_revision.value() << '\n';
    for (const auto& domain : domains) output << "domain=" << domain.canonical() << '\n';
    for (const auto& representation : representations) {
        output << "representation=" << representation.id.value << ':' << representation.domain.value
               << ':' << static_cast<int>(representation.kind) << ':' << representation.content_digest
               << '\n';
    }
    for (const auto& runtime : runtime_records) {
        output << "runtime=" << runtime.domain.value << ':' << runtime.representation.value << ':'
               << std::setprecision(17) << runtime.geometric_error << ':' << runtime.radiometric_error
               << ':' << runtime.continuity_id << ':' << runtime.material_group << ':'
               << runtime.semantic_group << '\n';
    }
    for (const auto& closure : closures) output << "closure=" << closure.canonical() << '\n';
    for (const auto& migration : migrations) output << "migration=" << migration.canonical() << '\n';
    return output.str();
}

core::Result<void> FidelityPackage::refresh_digest() {
    if (schema_version != current_schema_version || source_revision.exhausted() || domains.empty()) {
        return core::Result<void>::failure(validation(
            "fidelity package cannot refresh from incomplete source state"));
    }
    digest = digest_hex(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<void> FidelityPackage::validate(
    const geometry::CompiledMesh& source) const {
    if (!source.valid() || schema_version != current_schema_version ||
        source_revision != source.source_revision || domains.empty() || !valid_hex_digest(digest)) {
        return core::Result<void>::failure(validation("fidelity package header is invalid"));
    }
    std::set<SurfaceDomainId> domain_ids;
    for (const auto& domain : domains) {
        if (auto result = domain.validate(source); !result) return result;
        if (!domain_ids.insert(domain.id).second) {
            return core::Result<void>::failure(validation("fidelity package duplicates a domain"));
        }
    }
    std::set<RepresentationId> representation_ids;
    std::map<RepresentationId, SurfaceDomainId> representation_domains;
    for (const auto& representation : representations) {
        if (auto result = representation.validate(); !result) return result;
        if (!domain_ids.contains(representation.domain) ||
            !representation_ids.insert(representation.id).second ||
            !representation_domains.emplace(representation.id, representation.domain).second) {
            return core::Result<void>::failure(validation(
                "fidelity package has an invalid representation reference"));
        }
    }
    for (const auto& domain : domains) {
        SurfaceRepresentationTree tree;
        for (const auto& representation : representations) {
            if (representation.domain == domain.id) {
                if (auto result = tree.add(representation); !result) return result;
            }
        }
        if (auto result = tree.validate(domain.id); !result) return result;
    }
    for (const auto& runtime : runtime_records) {
        if (auto result = runtime.validate(); !result) return result;
        const auto representation = representation_domains.find(runtime.representation);
        if (!domain_ids.contains(runtime.domain) || representation == representation_domains.end() ||
            representation->second != runtime.domain) {
            return core::Result<void>::failure(validation(
                "fidelity package has an invalid runtime record reference"));
        }
    }
    for (const auto& closure : closures) {
        const auto domain = std::find_if(domains.begin(), domains.end(), [&closure](const auto& value) {
            return value.id == closure.domain;
        });
        if (domain == domains.end()) {
            return core::Result<void>::failure(validation(
                "fidelity package closure references an unknown domain"));
        }
        if (auto result = closure.validate(source, *domain); !result) return result;
        const auto representation = representation_domains.find(closure.representation);
        if (representation == representation_domains.end() ||
            representation->second != closure.domain) {
            return core::Result<void>::failure(validation(
                "fidelity package closure references an unknown representation"));
        }
    }
    for (const auto& migration : migrations) {
        if (auto result = migration.validate(); !result) return result;
        const auto source_representation =
            representation_domains.find(migration.source_representation);
        const auto target_representation =
            representation_domains.find(migration.target_representation);
        if (!domain_ids.contains(migration.domain) ||
            source_representation == representation_domains.end() ||
            target_representation == representation_domains.end() ||
            source_representation->second != migration.domain ||
            target_representation->second != migration.domain ||
            migration.source_revision != source_revision) {
            return core::Result<void>::failure(validation(
                "fidelity package migration references an unknown or stale representation"));
        }
    }
    const std::string expected = digest_hex(canonical_without_digest());
    if (digest != expected) {
        return core::Result<void>::failure(validation(
            "fidelity package digest does not match canonical content"));
    }
    return core::Result<void>::success();
}

} // namespace carto::surface
