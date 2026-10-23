#include <carto/editor/observation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

namespace carto::editor {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

core::Vec3d world_point(const core::Transform& transform, core::Vec3d point) noexcept {
    return transform.translation + transform.rotation.rotate(
        core::componentwise_multiply(transform.scale, point));
}

core::Result<double> validate_query(
    const PointSightlineQuery& query,
    const PointSightlineOptions& options) {
    if (!query.observer.finite() || !query.target.finite() ||
        (query.observer_object.has_value() && !*query.observer_object) ||
        (query.target_object.has_value() && !*query.target_object)) {
        return core::Result<double>::failure(invalid(
            "point sightline endpoints or excluded object identities are invalid"));
    }
    const double distance = (query.target - query.observer).length();
    if (!finite_positive(distance) || distance > options.max_distance ||
        distance <= 2.0 * options.endpoint_epsilon) {
        return core::Result<double>::failure(invalid(
            "point sightline segment is degenerate or outside the distance bounds"));
    }
    return core::Result<double>::success(distance);
}

bool is_endpoint_object(
    scene::ObjectId object,
    const PointSightlineQuery& query) noexcept {
    return (query.observer_object.has_value() && object == *query.observer_object) ||
        (query.target_object.has_value() && object == *query.target_object);
}

struct ValidatedRouteGeometry final {
    std::vector<double> cumulative_distance;
    double total_length = 0.0;
};

core::Result<ValidatedRouteGeometry> validate_route_geometry(
    const RouteVisibilityQuery& query,
    const RouteVisibilityOptions& options) {
    if (!query.target.finite() ||
        (query.target_object.has_value() && !*query.target_object) ||
        query.observer_positions.size() < 2U ||
        query.observer_positions.size() > options.max_samples ||
        (!query.observer_objects.empty() &&
            query.observer_objects.size() != query.observer_positions.size())) {
        return core::Result<ValidatedRouteGeometry>::failure(invalid(
            "route visibility query has invalid target, sample count, or observer bindings"));
    }

    ValidatedRouteGeometry geometry;
    geometry.cumulative_distance.reserve(query.observer_positions.size());
    geometry.cumulative_distance.push_back(0.0);
    for (std::size_t index = 0U; index < query.observer_positions.size(); ++index) {
        const core::Vec3d observer = query.observer_positions[index];
        if (!observer.finite() || (!query.observer_objects.empty() &&
                query.observer_objects[index].has_value() &&
                !*query.observer_objects[index])) {
            return core::Result<ValidatedRouteGeometry>::failure(invalid(
                "route visibility query contains invalid observer geometry or identity"));
        }
        const PointSightlineQuery point_query{
            observer, query.target, query.source_revision,
            query.observer_objects.empty() ? std::nullopt : query.observer_objects[index],
            query.target_object};
        const auto sightline = validate_query(point_query, options.sightline);
        if (!sightline) {
            return core::Result<ValidatedRouteGeometry>::failure(sightline.error());
        }
        if (index == 0U) continue;

        const double segment_length =
            (observer - query.observer_positions[index - 1U]).length();
        if (!finite_positive(segment_length) ||
            segment_length > options.max_route_length - geometry.total_length) {
            return core::Result<ValidatedRouteGeometry>::failure(invalid(
                "route visibility path is degenerate or exceeds its length bound"));
        }
        geometry.total_length += segment_length;
        if (!std::isfinite(geometry.total_length)) {
            return core::Result<ValidatedRouteGeometry>::failure(invalid(
                "route visibility path length overflowed"));
        }
        geometry.cumulative_distance.push_back(geometry.total_length);
    }
    return core::Result<ValidatedRouteGeometry>::success(std::move(geometry));
}

bool same_point(core::Vec3d left, core::Vec3d right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

assets::Sha256Digest curve_digest(const geometry::CurvePath& curve) {
    const std::string encoded = curve.serialize();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(encoded.data());
    return assets::sha256(std::span<const std::uint8_t>(bytes, encoded.size()));
}

core::Result<std::vector<core::Vec3d>> curve_route_positions(
    const geometry::CurvePath& curve,
    std::uint32_t samples_per_segment) {
    if (samples_per_segment == 0U ||
        samples_per_segment > geometry::CurvePath::kMaxSamplesPerSegment) {
        return core::Result<std::vector<core::Vec3d>>::failure(invalid(
            "curve visibility sample density is outside its bounded range"));
    }
    const auto samples = curve.sample_uniform(samples_per_segment);
    if (!samples) {
        return core::Result<std::vector<core::Vec3d>>::failure(samples.error());
    }
    std::vector<core::Vec3d> positions;
    positions.reserve(samples.value().size());
    for (const auto& sample : samples.value()) {
        if (positions.empty() || !same_point(positions.back(), sample.position)) {
            positions.push_back(sample.position);
        }
    }
    return core::Result<std::vector<core::Vec3d>>::success(std::move(positions));
}

core::Result<std::uint32_t> validate_viewshed_query(
    const LandmarkViewshedQuery& query,
    const LandmarkViewshedOptions& options) {
    const std::uint64_t cell_count =
        static_cast<std::uint64_t>(query.width) * query.height;
    if (!query.observer.finite() ||
        (query.observer_object.has_value() && !*query.observer_object) ||
        query.width < 2U || query.height < 2U || cell_count > options.max_cells ||
        cell_count != query.target_positions.size()) {
        return core::Result<std::uint32_t>::failure(invalid(
            "landmark viewshed query has invalid observer or grid dimensions"));
    }
    const PointSightlineQuery base{
        query.observer, {}, query.source_revision, query.observer_object, std::nullopt};
    for (const auto target : query.target_positions) {
        auto point_query = base;
        point_query.target = target;
        const auto valid = validate_query(point_query, options.sightline);
        if (!valid) return core::Result<std::uint32_t>::failure(valid.error());
    }
    return core::Result<std::uint32_t>::success(
        static_cast<std::uint32_t>(cell_count));
}

std::vector<double> sampled_axis(
    std::uint32_t source_count,
    std::uint32_t maximum_samples) {
    const std::size_t count = std::min<std::size_t>(source_count, maximum_samples);
    std::vector<double> result;
    result.reserve(count);
    if (count < 2U) return result;
    for (std::size_t index = 0U; index < count; ++index) {
        const double fraction = static_cast<double>(index) /
            static_cast<double>(count - 1U);
        result.push_back(fraction * static_cast<double>(source_count - 1U));
    }
    return result;
}

core::Result<core::Vec3d> sample_terrain_product_surface(
    const terrain::TerrainTileProduct& product,
    double source_u,
    double source_v) {
    const auto& mesh_u = product.mesh_u();
    const auto& mesh_v = product.mesh_v();
    if (!std::isfinite(source_u) || !std::isfinite(source_v) ||
        source_u < 0.0 || source_v < 0.0 ||
        source_u > static_cast<double>(product.source_width() - 1U) ||
        source_v > static_cast<double>(product.source_height() - 1U) ||
        mesh_u.size() < 2U || mesh_v.size() < 2U) {
        return core::Result<core::Vec3d>::failure(invalid(
            "terrain surface sample lies outside the validated product"));
    }
    const auto upper_cell = [](const std::vector<std::uint32_t>& axis, double value) {
        const auto upper = std::upper_bound(axis.begin(), axis.end(), value,
            [](double coordinate, std::uint32_t sample) {
                return coordinate < static_cast<double>(sample);
            });
        if (upper == axis.begin()) return std::size_t{1U};
        if (upper == axis.end()) return axis.size() - 1U;
        return static_cast<std::size_t>(upper - axis.begin());
    };
    const std::size_t u1 = upper_cell(mesh_u, source_u);
    const std::size_t v1 = upper_cell(mesh_v, source_v);
    const std::size_t u0 = u1 - 1U;
    const std::size_t v0 = v1 - 1U;
    const double u_span = static_cast<double>(mesh_u[u1] - mesh_u[u0]);
    const double v_span = static_cast<double>(mesh_v[v1] - mesh_v[v0]);
    if (!(u_span > 0.0) || !(v_span > 0.0)) {
        return core::Result<core::Vec3d>::failure(validation(
            "terrain product contains a degenerate sampling cell"));
    }
    const double fu = std::clamp(
        (source_u - static_cast<double>(mesh_u[u0])) / u_span, 0.0, 1.0);
    const double fv = std::clamp(
        (source_v - static_cast<double>(mesh_v[v0])) / v_span, 0.0, 1.0);
    const std::size_t row_width = mesh_u.size();
    const auto& vertices = product.vertices();
    const auto& a = vertices[v0 * row_width + u0].position;
    const auto& b = vertices[v0 * row_width + u1].position;
    const auto& c = vertices[v1 * row_width + u0].position;
    const auto& d = vertices[v1 * row_width + u1].position;
    double height = 0.0;
    if (fu + fv <= 1.0) {
        height = a.y * (1.0 - fu - fv) + b.y * fu + c.y * fv;
    } else {
        height = b.y * (1.0 - fv) + c.y * (1.0 - fu) +
            d.y * (fu + fv - 1.0);
    }
    const core::Vec3d position{
        product.source_origin_uv().x + source_u * product.source_cell_size(),
        height,
        product.source_origin_uv().y + source_v * product.source_cell_size()};
    if (!position.finite()) {
        return core::Result<core::Vec3d>::failure(validation(
            "terrain surface interpolation produced a non-finite target"));
    }
    return core::Result<core::Vec3d>::success(position);
}

core::Result<LandmarkViewshedQuery> terrain_viewshed_query(
    const terrain::TerrainTileProduct& product,
    core::Vec3d observer,
    core::Revision project_revision,
    std::optional<scene::ObjectId> observer_object,
    std::uint32_t max_axis) {
    if (auto valid = product.validate(); !valid) {
        return core::Result<LandmarkViewshedQuery>::failure(valid.error());
    }
    if (!observer.finite() || (observer_object.has_value() && !*observer_object) ||
        project_revision.exhausted() || max_axis < 2U ||
        max_axis > TerrainLandmarkViewshedGrid::kMaxAxis) {
        return core::Result<LandmarkViewshedQuery>::failure(invalid(
            "terrain viewshed requires a finite observer, current project revision, and bounded grid"));
    }

    const auto u_samples = sampled_axis(product.source_width(), max_axis);
    const auto v_samples = sampled_axis(product.source_height(), max_axis);
    if (u_samples.size() < 2U || v_samples.size() < 2U ||
        u_samples.size() * v_samples.size() > LandmarkViewshedOptions{}.max_cells) {
        return core::Result<LandmarkViewshedQuery>::failure(validation(
            "terrain viewshed source grid cannot produce a bounded sample set"));
    }

    LandmarkViewshedQuery query;
    query.observer = observer;
    query.observer_object = observer_object;
    query.width = static_cast<std::uint32_t>(u_samples.size());
    query.height = static_cast<std::uint32_t>(v_samples.size());
    query.source_revision = project_revision;
    query.target_positions.reserve(u_samples.size() * v_samples.size());
    for (const double v : v_samples) {
        for (const double u : u_samples) {
            const auto sample = sample_terrain_product_surface(product, u, v);
            if (!sample) {
                return core::Result<LandmarkViewshedQuery>::failure(sample.error());
            }
            query.target_positions.push_back(sample.value());
        }
    }
    return core::Result<LandmarkViewshedQuery>::success(std::move(query));
}

} // namespace

core::Result<void> TerrainLandmarkViewshedGrid::validate(
    const terrain::TerrainTileProduct& terrain_product,
    core::Revision current_project_revision) const {
    if (auto valid = terrain_product.validate(); !valid) return valid;
    if (terrain_product.source_identity() != terrain_identity ||
        terrain_product.source_revision() != terrain_revision ||
        terrain_product.source_digest() != terrain_digest ||
        terrain_product.lod_step() != terrain_lod_step) {
        return core::Result<void>::failure(stale(
            "terrain viewshed grid belongs to a different source revision or product LOD"));
    }
    if (query.source_revision != current_project_revision ||
        current_project_revision.exhausted()) {
        return core::Result<void>::failure(stale(
            "terrain viewshed grid belongs to a stale project revision"));
    }
    const auto expected = terrain_viewshed_query(
        terrain_product, query.observer, query.source_revision,
        query.observer_object, std::max(query.width, query.height));
    if (!expected) return core::Result<void>::failure(expected.error());
    if (query.width != expected.value().width ||
        query.height != expected.value().height ||
        query.target_positions.size() != expected.value().target_positions.size()) {
        return core::Result<void>::failure(validation(
            "terrain viewshed grid dimensions or sample count changed"));
    }
    for (std::size_t index = 0U; index < query.target_positions.size(); ++index) {
        if (!same_point(query.target_positions[index],
                        expected.value().target_positions[index])) {
            return core::Result<void>::failure(validation(
                "terrain viewshed target differs from its exact source vertex"));
        }
    }
    return core::Result<void>::success();
}

core::Result<TerrainLandmarkViewshedGrid> build_terrain_landmark_viewshed_grid(
    const terrain::TerrainTileProduct& terrain_product,
    core::Vec3d observer,
    core::Revision project_revision,
    std::optional<scene::ObjectId> observer_object,
    std::uint32_t max_axis) {
    const auto query = terrain_viewshed_query(
        terrain_product, observer, project_revision, observer_object, max_axis);
    if (!query) {
        return core::Result<TerrainLandmarkViewshedGrid>::failure(query.error());
    }
    TerrainLandmarkViewshedGrid grid;
    grid.terrain_identity = terrain_product.source_identity();
    grid.terrain_revision = terrain_product.source_revision();
    grid.terrain_digest = terrain_product.source_digest();
    grid.terrain_lod_step = terrain_product.lod_step();
    grid.query = query.value();
    if (auto valid = grid.validate(terrain_product, project_revision); !valid) {
        return core::Result<TerrainLandmarkViewshedGrid>::failure(valid.error());
    }
    return core::Result<TerrainLandmarkViewshedGrid>::success(std::move(grid));
}

core::Result<void> PointSightlineOptions::validate() const {
    if (!finite_positive(endpoint_epsilon) || endpoint_epsilon > 1.0 ||
        !finite_positive(parallel_epsilon) || parallel_epsilon > 1.0e-3 ||
        !finite_positive(max_distance) || max_distance > 1.0e6 ||
        max_occluders == 0U || max_occluders > 16384U ||
        max_triangles == 0U || max_triangles > 1'000'000U) {
        return core::Result<void>::failure(invalid(
            "point sightline options exceed bounded limits"));
    }
    return core::Result<void>::success();
}

core::Result<void> PointSightlineResult::validate(
    const PointSightlineQuery& query,
    const PointSightlineOptions& options,
    core::Revision current_source_revision) const {
    if (auto valid = options.validate(); !valid) return valid;
    const auto measured_length = validate_query(query, options);
    if (!measured_length) return core::Result<void>::failure(measured_length.error());
    if (current_source_revision != source_revision ||
        current_source_revision != query.source_revision) {
        return core::Result<void>::failure(stale(
            "point sightline result belongs to a stale project snapshot"));
    }
    const double length_tolerance = std::max(1.0, measured_length.value()) * 1.0e-12;
    if (algorithm != kAlgorithm || source_revision != query.source_revision ||
        observer.x != query.observer.x || observer.y != query.observer.y ||
        observer.z != query.observer.z || target.x != query.target.x ||
        target.y != query.target.y || target.z != query.target.z ||
        !finite_positive(segment_length) ||
        std::abs(segment_length - measured_length.value()) > length_tolerance ||
        triangles_tested > options.max_triangles || !std::isfinite(blocker_distance)) {
        return core::Result<void>::failure(validation(
            "point sightline receipt does not match its query or bounded result shape"));
    }
    if (visible) {
        if (blocker_object.has_value() || blocker_mesh_asset.has_value() ||
            blocker_mesh_revision.has_value() || blocker_face.has_value() ||
            blocker_distance != 0.0) {
            return core::Result<void>::failure(validation(
                "visible point sightline must not carry a blocker receipt"));
        }
    } else if (!blocker_object.has_value() || !*blocker_object ||
        !blocker_mesh_asset.has_value() || *blocker_mesh_asset == 0U ||
        !blocker_mesh_revision.has_value() || !blocker_face.has_value() ||
        !*blocker_face || blocker_distance <= options.endpoint_epsilon ||
        blocker_distance >= segment_length - options.endpoint_epsilon) {
        return core::Result<void>::failure(validation(
            "blocked point sightline is missing an admissible blocker receipt"));
    }
    return core::Result<void>::success();
}

core::Result<PointSightlineResult> evaluate_point_sightline(
    const render::RenderScene& scene,
    const PointSightlineQuery& query,
    const PointSightlineOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<PointSightlineResult>::failure(valid.error());
    }
    const auto measured_length = validate_query(query, options);
    if (!measured_length) {
        return core::Result<PointSightlineResult>::failure(measured_length.error());
    }
    if (scene.size() > options.max_occluders) {
        return core::Result<PointSightlineResult>::failure(validation(
            "point sightline scene exceeds the occluder budget"));
    }

    const core::Vec3d direction =
        (query.target - query.observer) * (1.0 / measured_length.value());
    std::uint64_t triangle_budget_used = 0U;
    double nearest_distance = measured_length.value();
    const render::RenderInstance* nearest_instance = nullptr;
    std::optional<geometry::FaceId> nearest_face;
    const double distance_tie_tolerance =
        std::max(1.0, measured_length.value()) * 1.0e-12;
    std::uint32_t triangles_tested = 0U;
    const auto instances = scene.instances();

    for (const auto& instance : instances) {
        if (!instance.object) {
            return core::Result<PointSightlineResult>::failure(validation(
                "point sightline scene contains an invalid object identity"));
        }
        if (is_endpoint_object(instance.object, query)) continue;
        if (!instance.mesh_asset.has_value() || *instance.mesh_asset == 0U ||
            instance.mesh == nullptr) {
            return core::Result<PointSightlineResult>::failure(validation(
                "point sightline occluder lacks a mesh asset identity or snapshot"));
        }
        if (instance.source_revision != instance.mesh->source_revision) {
            return core::Result<PointSightlineResult>::failure(stale(
                "point sightline occluder mesh revision is stale"));
        }
        if (!instance.world_transform.finite() || !instance.mesh->valid()) {
            return core::Result<PointSightlineResult>::failure(validation(
                "point sightline occluder transform or compiled mesh is invalid"));
        }
        const std::uint64_t mesh_triangles = static_cast<std::uint64_t>(
            instance.mesh->triangle_faces.size());
        if (mesh_triangles > options.max_triangles - triangle_budget_used) {
            return core::Result<PointSightlineResult>::failure(validation(
                "point sightline scene exceeds the triangle budget"));
        }
        triangle_budget_used += mesh_triangles;

        for (std::size_t triangle = 0U;
             triangle < instance.mesh->triangle_faces.size(); ++triangle) {
            ++triangles_tested;
            const std::size_t offset = triangle * 3U;
            const core::Vec3d first = world_point(instance.world_transform,
                instance.mesh->positions[instance.mesh->indices[offset]]);
            const core::Vec3d second = world_point(instance.world_transform,
                instance.mesh->positions[instance.mesh->indices[offset + 1U]]);
            const core::Vec3d third = world_point(instance.world_transform,
                instance.mesh->positions[instance.mesh->indices[offset + 2U]]);
            if (!first.finite() || !second.finite() || !third.finite()) {
                return core::Result<PointSightlineResult>::failure(validation(
                    "point sightline occluder produced a non-finite world-space triangle"));
            }

            const core::Vec3d edge_one = second - first;
            const core::Vec3d edge_two = third - first;
            const double edge_scale = edge_one.length() * edge_two.length();
            const core::Vec3d cross_direction = core::cross(direction, edge_two);
            const double determinant = core::dot(edge_one, cross_direction);
            if (!std::isfinite(edge_scale) || !std::isfinite(determinant)) {
                return core::Result<PointSightlineResult>::failure(validation(
                    "point sightline intersection arithmetic overflowed"));
            }
            if (edge_scale == 0.0 ||
                std::abs(determinant) <= options.parallel_epsilon * edge_scale) {
                continue;
            }

            const double inverse_determinant = 1.0 / determinant;
            const core::Vec3d observer_delta = query.observer - first;
            const double u = inverse_determinant *
                core::dot(observer_delta, cross_direction);
            const core::Vec3d cross_observer = core::cross(observer_delta, edge_one);
            const double v = inverse_determinant * core::dot(direction, cross_observer);
            const double hit_distance = inverse_determinant *
                core::dot(edge_two, cross_observer);
            if (!std::isfinite(u) || !std::isfinite(v) ||
                !std::isfinite(hit_distance)) {
                return core::Result<PointSightlineResult>::failure(validation(
                    "point sightline barycentric arithmetic overflowed"));
            }
            constexpr double kBarycentricEpsilon = 1.0e-12;
            if (u < -kBarycentricEpsilon || u > 1.0 + kBarycentricEpsilon ||
                v < -kBarycentricEpsilon || u + v > 1.0 + kBarycentricEpsilon ||
                hit_distance <= options.endpoint_epsilon ||
                hit_distance >= measured_length.value() - options.endpoint_epsilon) {
                continue;
            }

            const geometry::FaceId face = instance.mesh->triangle_faces[triangle];
            const bool nearer = hit_distance < nearest_distance - distance_tie_tolerance;
            const bool tied = nearest_instance != nullptr &&
                std::abs(hit_distance - nearest_distance) <= distance_tie_tolerance;
            const bool stable_tie_winner = tied && std::tie(
                    instance.object.value, face.value) <
                std::tie(nearest_instance->object.value, nearest_face->value);
            if (nearer || stable_tie_winner) {
                nearest_distance = hit_distance;
                nearest_instance = &instance;
                nearest_face = face;
            }
        }
    }

    PointSightlineResult result;
    result.source_revision = query.source_revision;
    result.observer = query.observer;
    result.target = query.target;
    result.segment_length = measured_length.value();
    result.triangles_tested = triangles_tested;
    result.visible = nearest_instance == nullptr;
    if (!result.visible) {
        result.blocker_object = nearest_instance->object;
        result.blocker_mesh_asset = nearest_instance->mesh_asset;
        result.blocker_mesh_revision = nearest_instance->source_revision;
        result.blocker_face = nearest_face;
        result.blocker_distance = nearest_distance;
    }
    if (auto valid = result.validate(query, options, query.source_revision); !valid) {
        return core::Result<PointSightlineResult>::failure(valid.error());
    }
    return core::Result<PointSightlineResult>::success(std::move(result));
}

core::Result<void> RouteVisibilityOptions::validate() const {
    if (auto valid = sightline.validate(); !valid) return valid;
    if (max_samples < 2U || max_samples > 4096U ||
        !finite_positive(max_route_length) || max_route_length > 1.0e6 ||
        max_total_triangles == 0U || max_total_triangles > 16'000'000U) {
        return core::Result<void>::failure(invalid(
            "route visibility options exceed bounded limits"));
    }
    return core::Result<void>::success();
}

core::Result<void> RouteVisibilityResult::validate(
    const RouteVisibilityQuery& query,
    const RouteVisibilityOptions& options,
    core::Revision current_source_revision) const {
    if (auto valid = options.validate(); !valid) return valid;
    const auto geometry = validate_route_geometry(query, options);
    if (!geometry) return core::Result<void>::failure(geometry.error());
    if (current_source_revision != source_revision ||
        current_source_revision != query.source_revision) {
        return core::Result<void>::failure(stale(
            "route visibility result belongs to a stale project snapshot"));
    }
    const double length_tolerance = std::max(1.0, geometry.value().total_length) * 1.0e-12;
    if (algorithm != kAlgorithm || source_revision != query.source_revision ||
        !same_point(target, query.target) || samples.size() != query.observer_positions.size() ||
        cumulative_route_distance.size() != query.observer_positions.size() ||
        !std::isfinite(route_length) ||
        std::abs(route_length - geometry.value().total_length) > length_tolerance ||
        triangles_tested > options.max_total_triangles) {
        return core::Result<void>::failure(validation(
            "route visibility receipt does not match its query or aggregate bounds"));
    }

    std::uint64_t measured_triangles = 0U;
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        const PointSightlineResult& sample = samples[index];
        const double expected_distance = geometry.value().cumulative_distance[index];
        const double distance_tolerance = std::max(1.0, expected_distance) * 1.0e-12;
        const PointSightlineQuery point_query{
            query.observer_positions[index], query.target, query.source_revision,
            query.observer_objects.empty() ? std::nullopt : query.observer_objects[index],
            query.target_object};
        if (!std::isfinite(cumulative_route_distance[index]) ||
            std::abs(cumulative_route_distance[index] - expected_distance) >
                distance_tolerance || !sample.validate(
                    point_query, options.sightline, current_source_revision)) {
            return core::Result<void>::failure(validation(
                "route visibility sample is stale or detached from its station"));
        }
        if (sample.triangles_tested > options.max_total_triangles - measured_triangles) {
            return core::Result<void>::failure(validation(
                "route visibility sample totals overflow their aggregate budget"));
        }
        measured_triangles += sample.triangles_tested;
    }
    if (measured_triangles != triangles_tested) {
        return core::Result<void>::failure(validation(
            "route visibility aggregate triangle receipt is inconsistent"));
    }
    return core::Result<void>::success();
}

core::Result<RouteVisibilityResult> evaluate_route_visibility(
    const render::RenderScene& scene,
    const RouteVisibilityQuery& query,
    const RouteVisibilityOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<RouteVisibilityResult>::failure(valid.error());
    }
    const auto geometry = validate_route_geometry(query, options);
    if (!geometry) {
        return core::Result<RouteVisibilityResult>::failure(geometry.error());
    }

    RouteVisibilityResult result;
    result.source_revision = query.source_revision;
    result.target = query.target;
    result.route_length = geometry.value().total_length;
    result.cumulative_route_distance = geometry.value().cumulative_distance;
    result.samples.reserve(query.observer_positions.size());
    for (std::size_t index = 0U; index < query.observer_positions.size(); ++index) {
        const PointSightlineQuery point_query{
            query.observer_positions[index], query.target, query.source_revision,
            query.observer_objects.empty() ? std::nullopt : query.observer_objects[index],
            query.target_object};
        auto sample = evaluate_point_sightline(scene, point_query, options.sightline);
        if (!sample) return core::Result<RouteVisibilityResult>::failure(sample.error());
        if (sample.value().triangles_tested >
            options.max_total_triangles - result.triangles_tested) {
            return core::Result<RouteVisibilityResult>::failure(validation(
                "route visibility query exceeds its aggregate triangle budget"));
        }
        result.triangles_tested += sample.value().triangles_tested;
        result.samples.push_back(std::move(sample.value()));
    }
    if (auto valid = result.validate(query, options, query.source_revision); !valid) {
        return core::Result<RouteVisibilityResult>::failure(valid.error());
    }
    return core::Result<RouteVisibilityResult>::success(std::move(result));
}

core::Result<void> CurveRouteVisibilityQuery::validate(
    const geometry::CurvePath& curve,
    core::Revision current_project_revision,
    const RouteVisibilityOptions& options) const {
    if (auto valid = options.validate(); !valid) return valid;
    if (auto valid = curve.validate(); !valid) return valid;
    if (curve_identity.empty() || curve_identity != curve.source_identity ||
        curve_revision != curve.source_revision || curve_digest.is_zero() ||
        curve_digest != carto::editor::curve_digest(curve) ||
        samples_per_segment == 0U ||
        (observer_object.has_value() && !*observer_object) ||
        (target_object.has_value() && !*target_object) ||
        route.source_revision != current_project_revision ||
        route.target_object != target_object) {
        return core::Result<void>::failure(stale(
            "curve visibility query is detached from its current source or project snapshot"));
    }
    const auto positions = curve_route_positions(curve, samples_per_segment);
    if (!positions) return core::Result<void>::failure(positions.error());
    if (route.observer_positions.size() != positions.value().size() ||
        route.observer_positions.size() > options.max_samples) {
        return core::Result<void>::failure(validation(
            "curve visibility stations do not match the bounded source sampling"));
    }
    for (std::size_t index = 0U; index < route.observer_positions.size(); ++index) {
        if (!same_point(route.observer_positions[index], positions.value()[index])) {
            return core::Result<void>::failure(stale(
                "curve visibility station differs from its authored source sample"));
        }
    }
    const std::vector<std::optional<scene::ObjectId>> expected_observers(
        positions.value().size(), observer_object);
    if (route.observer_objects != expected_observers) {
        return core::Result<void>::failure(validation(
            "curve visibility observer exclusions do not match the query receipt"));
    }
    const auto geometry = validate_route_geometry(route, options);
    if (!geometry) return core::Result<void>::failure(geometry.error());
    return core::Result<void>::success();
}

core::Result<void> CurveRouteVisibilityResult::validate(
    const geometry::CurvePath& curve,
    core::Revision current_project_revision,
    const RouteVisibilityOptions& options) const {
    if (auto valid = query.validate(curve, current_project_revision, options); !valid) {
        return valid;
    }
    return visibility.validate(
        query.route, options, current_project_revision);
}

core::Result<CurveRouteVisibilityQuery> build_curve_route_visibility_query(
    const geometry::CurvePath& curve,
    core::Vec3d target,
    core::Revision project_revision,
    std::uint32_t samples_per_segment,
    std::optional<scene::ObjectId> observer_object,
    std::optional<scene::ObjectId> target_object,
    const RouteVisibilityOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<CurveRouteVisibilityQuery>::failure(valid.error());
    }
    if (auto valid = curve.validate(); !valid) {
        return core::Result<CurveRouteVisibilityQuery>::failure(valid.error());
    }
    const auto positions = curve_route_positions(curve, samples_per_segment);
    if (!positions) {
        return core::Result<CurveRouteVisibilityQuery>::failure(positions.error());
    }

    CurveRouteVisibilityQuery query;
    query.curve_identity = curve.source_identity;
    query.curve_revision = curve.source_revision;
    query.curve_digest = curve_digest(curve);
    query.samples_per_segment = samples_per_segment;
    query.observer_object = observer_object;
    query.target_object = target_object;
    query.route.observer_positions = positions.value();
    query.route.target = target;
    query.route.source_revision = project_revision;
    query.route.target_object = target_object;
    query.route.observer_objects.assign(positions.value().size(), observer_object);
    if (auto valid = query.validate(curve, project_revision, options); !valid) {
        return core::Result<CurveRouteVisibilityQuery>::failure(valid.error());
    }
    return core::Result<CurveRouteVisibilityQuery>::success(std::move(query));
}

core::Result<CurveRouteVisibilityResult> evaluate_curve_route_visibility(
    const render::RenderScene& scene,
    const geometry::CurvePath& curve,
    const CurveRouteVisibilityQuery& query,
    core::Revision current_project_revision,
    const RouteVisibilityOptions& options) {
    if (auto valid = query.validate(curve, current_project_revision, options); !valid) {
        return core::Result<CurveRouteVisibilityResult>::failure(valid.error());
    }
    auto visibility = evaluate_route_visibility(scene, query.route, options);
    if (!visibility) {
        return core::Result<CurveRouteVisibilityResult>::failure(visibility.error());
    }
    CurveRouteVisibilityResult result{query, std::move(visibility.value())};
    if (auto valid = result.validate(curve, current_project_revision, options); !valid) {
        return core::Result<CurveRouteVisibilityResult>::failure(valid.error());
    }
    return core::Result<CurveRouteVisibilityResult>::success(std::move(result));
}

core::Result<void> LandmarkViewshedOptions::validate() const {
    if (auto valid = sightline.validate(); !valid) return valid;
    if (max_cells < 4U || max_cells > 65'536U ||
        max_total_triangles == 0U || max_total_triangles > 16'000'000U) {
        return core::Result<void>::failure(invalid(
            "landmark viewshed options exceed bounded limits"));
    }
    return core::Result<void>::success();
}

core::Result<void> LandmarkViewshedResult::validate(
    const LandmarkViewshedQuery& query,
    const LandmarkViewshedOptions& options,
    core::Revision current_source_revision) const {
    if (auto valid = options.validate(); !valid) return valid;
    const auto cell_count = validate_viewshed_query(query, options);
    if (!cell_count) return core::Result<void>::failure(cell_count.error());
    if (current_source_revision != source_revision ||
        current_source_revision != query.source_revision) {
        return core::Result<void>::failure(stale(
            "landmark viewshed result belongs to a stale project snapshot"));
    }
    if (algorithm != kAlgorithm || source_revision != query.source_revision ||
        !same_point(observer, query.observer) || width != query.width ||
        height != query.height || cells.size() != cell_count.value() ||
        triangles_tested > options.max_total_triangles ||
        visible_cells > cell_count.value()) {
        return core::Result<void>::failure(validation(
            "landmark viewshed receipt does not match its query or aggregate bounds"));
    }

    std::uint64_t measured_triangles = 0U;
    std::uint32_t measured_visible = 0U;
    for (std::size_t index = 0U; index < cells.size(); ++index) {
        const PointSightlineQuery point_query{
            query.observer, query.target_positions[index], query.source_revision,
            query.observer_object, std::nullopt};
        if (!cells[index].validate(
                point_query, options.sightline, current_source_revision)) {
            return core::Result<void>::failure(validation(
                "landmark viewshed cell is stale or detached from its grid point"));
        }
        if (cells[index].triangles_tested >
            options.max_total_triangles - measured_triangles) {
            return core::Result<void>::failure(validation(
                "landmark viewshed cell totals exceed their aggregate budget"));
        }
        measured_triangles += cells[index].triangles_tested;
        if (cells[index].visible) ++measured_visible;
    }
    if (measured_triangles != triangles_tested || measured_visible != visible_cells) {
        return core::Result<void>::failure(validation(
            "landmark viewshed aggregate receipt is inconsistent"));
    }
    return core::Result<void>::success();
}

core::Result<LandmarkViewshedResult> evaluate_landmark_viewshed(
    const render::RenderScene& scene,
    const LandmarkViewshedQuery& query,
    const LandmarkViewshedOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<LandmarkViewshedResult>::failure(valid.error());
    }
    const auto cell_count = validate_viewshed_query(query, options);
    if (!cell_count) {
        return core::Result<LandmarkViewshedResult>::failure(cell_count.error());
    }

    LandmarkViewshedResult result;
    result.source_revision = query.source_revision;
    result.observer = query.observer;
    result.width = query.width;
    result.height = query.height;
    result.cells.reserve(cell_count.value());
    for (const auto target : query.target_positions) {
        const PointSightlineQuery point_query{
            query.observer, target, query.source_revision,
            query.observer_object, std::nullopt};
        auto cell = evaluate_point_sightline(scene, point_query, options.sightline);
        if (!cell) return core::Result<LandmarkViewshedResult>::failure(cell.error());
        if (cell.value().triangles_tested >
            options.max_total_triangles - result.triangles_tested) {
            return core::Result<LandmarkViewshedResult>::failure(validation(
                "landmark viewshed exceeds its aggregate triangle budget"));
        }
        result.triangles_tested += cell.value().triangles_tested;
        if (cell.value().visible) ++result.visible_cells;
        result.cells.push_back(std::move(cell.value()));
    }
    if (auto valid = result.validate(query, options, query.source_revision); !valid) {
        return core::Result<LandmarkViewshedResult>::failure(valid.error());
    }
    return core::Result<LandmarkViewshedResult>::success(std::move(result));
}

namespace {

struct ObservationBasis final {
    core::Vec3d forward{};
    core::Vec3d right{};
    core::Vec3d up{};
};

struct ProjectedTarget final {
    double distance = 0.0;
    double horizontal_bearing = 0.0;
    double vertical_bearing = 0.0;
    std::optional<ObservationDisposition> disposition;
};

struct ValidatedObservationQuery final {
    ObservationBasis basis;
    std::vector<ProjectedTarget> targets;
};

core::Result<core::Vec3d> normalized_observation_axis(core::Vec3d axis) {
    const double length = axis.length();
    if (!axis.finite() || !finite_positive(length) || length > 1.0e6) {
        return core::Result<core::Vec3d>::failure(invalid(
            "authoring observation camera basis is degenerate or out of bounds"));
    }
    const core::Vec3d normalized = axis * (1.0 / length);
    if (!normalized.finite()) {
        return core::Result<core::Vec3d>::failure(invalid(
            "authoring observation camera basis could not be normalized"));
    }
    return core::Result<core::Vec3d>::success(normalized);
}

core::Result<ValidatedObservationQuery> validate_observation_query(
    const AuthoringObservationQuery& query,
    const AuthoringObservationOptions& options) {
    if (!query.observer.finite() ||
        (query.observer_object.has_value() && !*query.observer_object) ||
        !std::isfinite(query.horizontal_fov_radians) ||
        !std::isfinite(query.vertical_fov_radians) ||
        query.horizontal_fov_radians <= 0.0 ||
        query.horizontal_fov_radians >= 3.141592653589793 ||
        query.vertical_fov_radians <= 0.0 ||
        query.vertical_fov_radians >= 3.141592653589793 ||
        query.targets.empty() || query.targets.size() > options.max_targets) {
        return core::Result<ValidatedObservationQuery>::failure(invalid(
            "authoring observation query has an invalid camera, cone, or target count"));
    }

    const auto forward = normalized_observation_axis(query.forward);
    const auto supplied_up = normalized_observation_axis(query.up);
    if (!forward || !supplied_up) {
        return core::Result<ValidatedObservationQuery>::failure(
            !forward ? forward.error() : supplied_up.error());
    }
    const auto right = core::cross(forward.value(), supplied_up.value()).normalized();
    if (!right.finite() || right.length_squared() < 0.5) {
        return core::Result<ValidatedObservationQuery>::failure(invalid(
            "authoring observation forward and up axes must not be parallel"));
    }
    const auto corrected_up = core::cross(right, forward.value()).normalized();
    if (!corrected_up.finite()) {
        return core::Result<ValidatedObservationQuery>::failure(invalid(
            "authoring observation camera basis is invalid"));
    }

    ValidatedObservationQuery validated;
    validated.basis = {forward.value(), right, corrected_up};
    validated.targets.reserve(query.targets.size());
    std::vector<std::uint64_t> seen_identities;
    seen_identities.reserve(query.targets.size());
    for (const auto& target : query.targets) {
        const std::uint32_t kind_bit = observation_target_kind_bit(target.kind);
        if (target.identity == 0U || !target.object || !target.position.finite() ||
            kind_bit == 0U ||
            (query.observer_object.has_value() &&
                target.object == *query.observer_object) ||
            std::find(seen_identities.begin(), seen_identities.end(), target.identity) !=
                seen_identities.end()) {
            return core::Result<ValidatedObservationQuery>::failure(invalid(
                "authoring observation target has an invalid or duplicate identity"));
        }
        seen_identities.push_back(target.identity);

        const core::Vec3d delta = target.position - query.observer;
        const double distance = delta.length();
        if (!delta.finite() || !finite_positive(distance)) {
            return core::Result<ValidatedObservationQuery>::failure(invalid(
                "authoring observation target is coincident with the observer or non-finite"));
        }
        const core::Vec3d direction = delta * (1.0 / distance);
        const double forward_component = core::dot(direction, validated.basis.forward);
        const double right_component = core::dot(direction, validated.basis.right);
        const double up_component = core::dot(direction, validated.basis.up);
        const double horizontal = std::atan2(right_component, forward_component);
        const double vertical = std::atan2(
            up_component, std::hypot(forward_component, right_component));
        if (!direction.finite() || !std::isfinite(horizontal) ||
            !std::isfinite(vertical)) {
            return core::Result<ValidatedObservationQuery>::failure(invalid(
                "authoring observation target projection is non-finite"));
        }

        ProjectedTarget projected;
        projected.distance = distance;
        projected.horizontal_bearing = horizontal;
        projected.vertical_bearing = vertical;
        if ((options.included_target_kinds & kind_bit) == 0U) {
            projected.disposition = ObservationDisposition::filtered_by_kind;
        } else if (distance > options.max_distance) {
            projected.disposition = ObservationDisposition::outside_range;
        } else if (forward_component <= 0.0 ||
            std::abs(horizontal) > query.horizontal_fov_radians * 0.5 ||
            std::abs(vertical) > query.vertical_fov_radians * 0.5) {
            projected.disposition = ObservationDisposition::outside_view_cone;
        }
        validated.targets.push_back(projected);
    }
    return core::Result<ValidatedObservationQuery>::success(std::move(validated));
}

} // namespace

core::Result<void> AuthoringObservationOptions::validate() const {
    if (auto valid = sightline.validate(); !valid) return valid;
    if (!finite_positive(max_distance) || max_distance > 1.0e6 ||
        max_distance > sightline.max_distance ||
        included_target_kinds == 0U ||
        (included_target_kinds & ~kAllObservationTargetKinds) != 0U ||
        max_targets == 0U || max_targets > 4096U ||
        max_total_triangles == 0U || max_total_triangles > 16'000'000U) {
        return core::Result<void>::failure(invalid(
            "authoring observation options exceed bounded limits"));
    }
    return core::Result<void>::success();
}

core::Result<void> AuthoringObservationResult::validate(
    const AuthoringObservationQuery& query,
    const AuthoringObservationOptions& options,
    core::Revision current_source_revision) const {
    if (auto valid = options.validate(); !valid) return valid;
    const auto projected = validate_observation_query(query, options);
    if (!projected) return core::Result<void>::failure(projected.error());
    if (current_source_revision != source_revision ||
        current_source_revision != query.source_revision) {
        return core::Result<void>::failure(stale(
            "authoring observation report belongs to a stale project snapshot"));
    }
    if (algorithm != kAlgorithm || source_revision != query.source_revision ||
        !same_point(observer, query.observer) ||
        !same_point(forward, projected.value().basis.forward) ||
        !same_point(right, projected.value().basis.right) ||
        !same_point(up, projected.value().basis.up) ||
        horizontal_fov_radians != query.horizontal_fov_radians ||
        vertical_fov_radians != query.vertical_fov_radians ||
        targets.size() != query.targets.size() ||
        triangles_tested > options.max_total_triangles) {
        return core::Result<void>::failure(validation(
            "authoring observation report does not match its source query or bounds"));
    }

    std::uint32_t measured_visible = 0U;
    std::uint32_t measured_occluded = 0U;
    std::uint32_t measured_filtered = 0U;
    std::uint32_t measured_outside_range = 0U;
    std::uint32_t measured_outside_cone = 0U;
    std::uint64_t measured_triangles = 0U;
    for (std::size_t index = 0U; index < targets.size(); ++index) {
        const auto& sample = targets[index];
        const auto& source = query.targets[index];
        const auto& geometry = projected.value().targets[index];
        if (sample.target_identity != source.identity || sample.object != source.object ||
            !same_point(sample.position, source.position) || sample.kind != source.kind ||
            sample.distance != geometry.distance ||
            sample.horizontal_bearing_radians != geometry.horizontal_bearing ||
            sample.vertical_bearing_radians != geometry.vertical_bearing) {
            return core::Result<void>::failure(validation(
                "authoring observation target receipt is detached from its source"));
        }
        if (geometry.disposition.has_value()) {
            if (sample.disposition != *geometry.disposition || sample.sightline.has_value()) {
                return core::Result<void>::failure(validation(
                    "filtered or out-of-cone observation target has an invalid receipt"));
            }
            switch (sample.disposition) {
            case ObservationDisposition::filtered_by_kind: ++measured_filtered; break;
            case ObservationDisposition::outside_range: ++measured_outside_range; break;
            case ObservationDisposition::outside_view_cone: ++measured_outside_cone; break;
            case ObservationDisposition::visible:
            case ObservationDisposition::occluded:
                return core::Result<void>::failure(validation(
                    "observation target disposition contradicts its query filter"));
            }
            continue;
        }

        if (!sample.sightline.has_value()) {
            return core::Result<void>::failure(validation(
                "in-cone observation target is missing its sightline receipt"));
        }
        const PointSightlineQuery point_query{
            query.observer, source.position, query.source_revision,
            query.observer_object, source.object};
        if (auto valid = sample.sightline->validate(
                point_query, options.sightline, current_source_revision); !valid) {
            return core::Result<void>::failure(validation(
                "authoring observation target contains a stale sightline receipt"));
        }
        if (sample.sightline->visible) {
            if (sample.disposition != ObservationDisposition::visible) {
                return core::Result<void>::failure(validation(
                    "visible sightline has a conflicting observation disposition"));
            }
            ++measured_visible;
        } else {
            if (sample.disposition != ObservationDisposition::occluded) {
                return core::Result<void>::failure(validation(
                    "blocked sightline has a conflicting observation disposition"));
            }
            ++measured_occluded;
        }
        if (sample.sightline->triangles_tested >
            options.max_total_triangles - measured_triangles) {
            return core::Result<void>::failure(validation(
                "authoring observation sightline totals exceed the aggregate budget"));
        }
        measured_triangles += sample.sightline->triangles_tested;
    }
    if (measured_visible != visible_targets || measured_occluded != occluded_targets ||
        measured_filtered != filtered_targets ||
        measured_outside_range != outside_range_targets ||
        measured_outside_cone != outside_view_cone_targets ||
        measured_triangles != triangles_tested) {
        return core::Result<void>::failure(validation(
            "authoring observation aggregate receipts are inconsistent"));
    }
    return core::Result<void>::success();
}

core::Result<AuthoringObservationResult> evaluate_authoring_observation(
    const render::RenderScene& scene,
    const AuthoringObservationQuery& query,
    const AuthoringObservationOptions& options) {
    if (auto valid = options.validate(); !valid) {
        return core::Result<AuthoringObservationResult>::failure(valid.error());
    }
    const auto projected = validate_observation_query(query, options);
    if (!projected) {
        return core::Result<AuthoringObservationResult>::failure(projected.error());
    }

    AuthoringObservationResult result;
    result.source_revision = query.source_revision;
    result.observer = query.observer;
    result.forward = projected.value().basis.forward;
    result.right = projected.value().basis.right;
    result.up = projected.value().basis.up;
    result.horizontal_fov_radians = query.horizontal_fov_radians;
    result.vertical_fov_radians = query.vertical_fov_radians;
    result.targets.reserve(query.targets.size());
    for (std::size_t index = 0U; index < query.targets.size(); ++index) {
        const auto& target = query.targets[index];
        const auto& geometry = projected.value().targets[index];
        ObservationProjectionSample sample;
        sample.target_identity = target.identity;
        sample.object = target.object;
        sample.position = target.position;
        sample.kind = target.kind;
        sample.disposition = geometry.disposition.value_or(
            ObservationDisposition::visible);
        sample.distance = geometry.distance;
        sample.horizontal_bearing_radians = geometry.horizontal_bearing;
        sample.vertical_bearing_radians = geometry.vertical_bearing;

        if (!geometry.disposition.has_value()) {
            const PointSightlineQuery point_query{
                query.observer, target.position, query.source_revision,
                query.observer_object, target.object};
            auto sightline = evaluate_point_sightline(scene, point_query, options.sightline);
            if (!sightline) {
                return core::Result<AuthoringObservationResult>::failure(sightline.error());
            }
            if (sightline.value().triangles_tested >
                options.max_total_triangles - result.triangles_tested) {
                return core::Result<AuthoringObservationResult>::failure(validation(
                    "authoring observation exceeds its aggregate triangle budget"));
            }
            result.triangles_tested += sightline.value().triangles_tested;
            sample.disposition = sightline.value().visible
                ? ObservationDisposition::visible : ObservationDisposition::occluded;
            sample.sightline = std::move(sightline.value());
        }

        switch (sample.disposition) {
        case ObservationDisposition::filtered_by_kind: ++result.filtered_targets; break;
        case ObservationDisposition::outside_range: ++result.outside_range_targets; break;
        case ObservationDisposition::outside_view_cone:
            ++result.outside_view_cone_targets;
            break;
        case ObservationDisposition::visible: ++result.visible_targets; break;
        case ObservationDisposition::occluded: ++result.occluded_targets; break;
        }
        result.targets.push_back(std::move(sample));
    }
    if (auto valid = result.validate(query, options, query.source_revision); !valid) {
        return core::Result<AuthoringObservationResult>::failure(valid.error());
    }
    return core::Result<AuthoringObservationResult>::success(std::move(result));
}

} // namespace carto::editor
