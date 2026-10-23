#include <carto/assets/blob_store.hpp>
#include <carto/collision/benchmark.hpp>
#include <carto/collision/compound.hpp>
#include <carto/collision/convex.hpp>
#include <carto/collision/family.hpp>
#include <carto/collision/metrics.hpp>
#include <carto/collision/primitive.hpp>
#include <carto/collision/triangle.hpp>
#include <carto/core/math.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/web_geometry/types.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1e-9) {
    return std::abs(left - right) <= epsilon;
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void add_cube(
    carto::geometry::CompiledMesh& mesh,
    const carto::core::Vec3d& center,
    std::uint64_t first_vertex_id,
    std::uint64_t first_face_id) {
    using namespace carto;
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.positions.size());
    const core::Vec3d corners[8] = {
        {center.x - 0.5, center.y - 0.5, center.z - 0.5},
        {center.x + 0.5, center.y - 0.5, center.z - 0.5},
        {center.x + 0.5, center.y + 0.5, center.z - 0.5},
        {center.x - 0.5, center.y + 0.5, center.z - 0.5},
        {center.x - 0.5, center.y - 0.5, center.z + 0.5},
        {center.x + 0.5, center.y - 0.5, center.z + 0.5},
        {center.x + 0.5, center.y + 0.5, center.z + 0.5},
        {center.x - 0.5, center.y + 0.5, center.z + 0.5},
    };
    for (std::size_t index = 0U; index < 8U; ++index) {
        mesh.positions.push_back(corners[index]);
        const core::Vec3d normal = (corners[index] - center) * 2.0;
        mesh.normals.push_back(normal.normalized());
        mesh.vertex_ids.push_back(geometry::VertexId{first_vertex_id + index});
        mesh.bounds.include(corners[index]);
    }
    const std::uint32_t faces[12][3] = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {3, 6, 2}, {3, 7, 6},
        {0, 4, 7}, {0, 7, 3},
        {1, 2, 6}, {1, 6, 5},
    };
    for (std::size_t index = 0U; index < 12U; ++index) {
        mesh.indices.push_back(base + faces[index][0]);
        mesh.indices.push_back(base + faces[index][1]);
        mesh.indices.push_back(base + faces[index][2]);
        mesh.triangle_faces.push_back(geometry::FaceId{first_face_id + index});
        mesh.triangle_edges.emplace_back();
    }
}

carto::geometry::CompiledMesh unit_cube_mesh() {
    carto::geometry::CompiledMesh mesh;
    mesh.source_revision = carto::core::Revision{1U};
    add_cube(mesh, {0.0, 0.0, 0.0}, 1U, 1U);
    return mesh;
}

carto::geometry::CompiledMesh two_cubes_mesh() {
    carto::geometry::CompiledMesh mesh;
    mesh.source_revision = carto::core::Revision{1U};
    add_cube(mesh, {0.0, 0.0, 0.0}, 1U, 1U);
    add_cube(mesh, {10.0, 0.0, 0.0}, 9U, 13U);
    return mesh;
}

carto::geometry::CompiledMesh octahedron_mesh() {
    using namespace carto;
    geometry::CompiledMesh mesh;
    mesh.source_revision = core::Revision{1U};
    const core::Vec3d poles[6] = {
        {1.0, 0.0, 0.0}, {-1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0}, {0.0, -1.0, 0.0},
        {0.0, 0.0, 1.0}, {0.0, 0.0, -1.0},
    };
    for (std::size_t index = 0U; index < 6U; ++index) {
        mesh.positions.push_back(poles[index]);
        mesh.normals.push_back(poles[index]);
        mesh.vertex_ids.push_back(geometry::VertexId{index + 1U});
        mesh.bounds.include(poles[index]);
    }
    const std::uint32_t faces[8][3] = {
        {4, 0, 2}, {4, 2, 1}, {4, 1, 3}, {4, 3, 0},
        {5, 2, 0}, {5, 1, 2}, {5, 3, 1}, {5, 0, 3},
    };
    for (std::size_t index = 0U; index < 8U; ++index) {
        mesh.indices.push_back(faces[index][0]);
        mesh.indices.push_back(faces[index][1]);
        mesh.indices.push_back(faces[index][2]);
        mesh.triangle_faces.push_back(geometry::FaceId{index + 1U});
        mesh.triangle_edges.emplace_back();
    }
    return mesh;
}

carto::collision::PrimitiveCollisionCandidate sphere_proxy(
    carto::core::Vec3d center,
    double radius) {
    carto::collision::PrimitiveCollisionCandidate candidate;
    candidate.kind = carto::collision::PrimitiveCollisionKind::sphere;
    candidate.center = center;
    candidate.radius = radius;
    candidate.fitted_vertex_count = 1U;
    require(static_cast<bool>(candidate.refresh_digest()),
        "sphere proxy refreshes its digest");
    return candidate;
}

carto::collision::PrimitiveCollisionCandidate box_proxy(
    carto::core::Vec3d center,
    double half_extent) {
    carto::collision::PrimitiveCollisionCandidate candidate;
    candidate.kind = carto::collision::PrimitiveCollisionKind::box;
    candidate.center = center;
    candidate.half_extents = {half_extent, half_extent, half_extent};
    candidate.fitted_vertex_count = 1U;
    require(static_cast<bool>(candidate.refresh_digest()),
        "box proxy refreshes its digest");
    return candidate;
}

carto::web_geometry::ClusterHierarchy donor_hierarchy() {
    using namespace carto;
    web_geometry::ClusterHierarchy hierarchy;
    hierarchy.source_revision = core::Revision{3U};
    hierarchy.source_faces = {geometry::FaceId{1U}, geometry::FaceId{2U}};
    hierarchy.triangle_indices = {0U, 1U};

    auto make_bounds = [](double reach) {
        web_geometry::ClusterBounds bounds;
        bounds.aabb.include({-reach, -reach, -reach});
        bounds.aabb.include({reach, reach, reach});
        bounds.sphere_center = {};
        bounds.sphere_radius = reach * std::sqrt(3.0);
        return bounds;
    };

    web_geometry::ClusterRecord root;
    root.id = 0U;
    root.first_child = 0U;
    root.child_count = 2U;
    root.bounds = make_bounds(4.0);
    root.geometric_error = 0.5;
    root.content_digest = 1U;
    web_geometry::ClusterRecord first;
    first.id = 1U;
    first.parent = 0U;
    first.first_triangle = 0U;
    first.triangle_count = 1U;
    first.bounds = make_bounds(2.0);
    first.geometric_error = 0.25;
    first.page = 7U;
    first.content_digest = 2U;
    web_geometry::ClusterRecord second;
    second.id = 2U;
    second.parent = 0U;
    second.first_triangle = 1U;
    second.triangle_count = 1U;
    second.bounds = make_bounds(1.0);
    second.geometric_error = 0.1;
    second.page = 3U;
    second.content_digest = 3U;

    hierarchy.child_ids = {1U, 2U};
    hierarchy.clusters = {root, first, second};
    // The donor digest binds its own canonical form, exactly as a real
    // compiled donor would.
    hierarchy.digest = {};
    const std::string canonical = hierarchy.canonical();
    hierarchy.digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    return hierarchy;
}

void box_fit_on_unit_cube_is_exact() {
    using namespace carto;
    const geometry::CompiledMesh mesh = unit_cube_mesh();
    const auto fit = collision::fit_box_candidate(mesh);
    require(static_cast<bool>(fit), "box fit succeeds on the unit cube");
    const collision::PrimitiveCollisionCandidate& candidate = fit.value();
    require(candidate.kind == collision::PrimitiveCollisionKind::box,
        "candidate kind is box");
    require(near(candidate.center.x, 0.0) && near(candidate.center.y, 0.0) &&
            near(candidate.center.z, 0.0),
        "box fit center is the cube center");
    require(near(candidate.half_extents.x, 0.5) && near(candidate.half_extents.y, 0.5) &&
            near(candidate.half_extents.z, 0.5),
        "box fit half extents match the unit cube");
    require(near(candidate.fit.max_outside_distance, 0.0),
        "every cube vertex lies inside the fitted box");
    require(near(candidate.fit.volume_residual, 0.0, 1e-12),
        "box volume residual is zero for the closed cube");
    require(static_cast<bool>(candidate.validate()),
        "fitted box candidate validates");
    require(!candidate.candidate_digest.is_zero(), "box candidate is digest-bound");
}

void sphere_fit_matches_known_answers() {
    using namespace carto;
    const geometry::CompiledMesh octahedron = octahedron_mesh();
    const auto fit = collision::fit_sphere_candidate(octahedron);
    require(static_cast<bool>(fit), "sphere fit succeeds on the octahedron");
    require(near(fit.value().radius, 1.0, 1e-12),
        "octahedron vertices lie exactly on the unit sphere");
    require(near(fit.value().center.x, 0.0) && near(fit.value().center.y, 0.0) &&
            near(fit.value().center.z, 0.0),
        "sphere fit center is the octahedron center");
    require(near(fit.value().fit.max_outside_distance, 0.0),
        "octahedron vertices lie inside the fitted sphere");
    require(fit.value().fit.volume_residual > 2.8 &&
            fit.value().fit.volume_residual < 2.9,
        "sphere volume residual matches |4/3 - 4*pi/3|");

    const geometry::CompiledMesh cube = unit_cube_mesh();
    const auto cube_sphere = collision::fit_sphere_candidate(cube);
    require(static_cast<bool>(cube_sphere), "sphere fit succeeds on the cube");
    require(near(cube_sphere.value().radius, std::sqrt(3.0) * 0.5, 1e-12),
        "cube sphere radius is half the space diagonal");

    const auto capsule = collision::fit_capsule_candidate(cube);
    require(static_cast<bool>(capsule) &&
            static_cast<bool>(capsule.value().validate()),
        "capsule fit on the cube validates and is digest-bound");
    const auto cylinder = collision::fit_cylinder_candidate(cube);
    require(static_cast<bool>(cylinder) &&
            static_cast<bool>(cylinder.value().validate()),
        "cylinder fit on the cube validates and is digest-bound");
}

void candidate_generation_is_deterministic() {
    using namespace carto;
    const geometry::CompiledMesh cube = unit_cube_mesh();
    const auto first = collision::generate_primitive_candidates(cube);
    const auto second = collision::generate_primitive_candidates(cube);
    require(first && second && first.value().size() == 4U &&
            second.value().size() == 4U,
        "candidate generation emits all four primitive kinds");
    for (std::size_t index = 0U; index < first.value().size(); ++index) {
        require(first.value()[index].canonical() == second.value()[index].canonical(),
            "repeated generation is byte-stable");
        require(first.value()[index].candidate_digest ==
                second.value()[index].candidate_digest,
            "candidate digests are reproducible");
    }
    require(first.value()[0].kind == collision::PrimitiveCollisionKind::sphere &&
            first.value()[1].kind == collision::PrimitiveCollisionKind::box &&
            first.value()[2].kind == collision::PrimitiveCollisionKind::capsule &&
            first.value()[3].kind == collision::PrimitiveCollisionKind::cylinder,
        "candidate generation follows the fixed kind order");
}

void error_metrics_match_analytic_cases() {
    using namespace carto;
    constexpr double kPi = 3.14159265358979323846;
    require(near(collision::sphere_sphere_overlap_volume({0, 0, 0}, 1.0, {1, 0, 0}, 1.0),
            5.0 * kPi / 12.0, 1e-12),
        "unit spheres one unit apart overlap by 5*pi/12");
    require(near(collision::box_box_overlap_volume(
            {0, 0, 0}, {1, 1, 1}, {1, 0, 0}, {1, 1, 1}), 4.0, 1e-12),
        "offset unit boxes overlap by exactly four cubic units");
    require(near(collision::sphere_box_overlap_volume(
            {2, 0, 0}, 1.0, {0, 0, 0}, {1, 1, 1}), 0.0, 1e-12),
        "separated sphere and box do not overlap");
    require(near(collision::sphere_box_overlap_volume(
            {1.5, 0, 0}, 1.0, {0, 0, 0}, {1, 1, 1}), 5.0 * kPi / 24.0, 1e-12),
        "half-buried sphere contributes an exact spherical cap");

    const std::vector<core::Vec3d> from{{0, 0, 0}, {2, 0, 0}};
    const std::vector<core::Vec3d> to{{1, 0, 0}};
    require(near(collision::one_sided_hausdorff(from, to), 1.0, 1e-12),
        "one-sided Hausdorff picks the farthest source point");

    const collision::PrimitiveCollisionCandidate first = sphere_proxy({0, 0, 0}, 1.0);
    const collision::PrimitiveCollisionCandidate second = sphere_proxy({1, 0, 0}, 1.0);
    auto penetration = collision::primitive_pair_max_penetration(first, second);
    require(static_cast<bool>(penetration) && near(penetration.value(), 1.0, 1e-12),
        "unit spheres one unit apart interpenetrate by exactly one unit");
    const collision::PrimitiveCollisionCandidate box_a = box_proxy({0, 0, 0}, 1.0);
    const collision::PrimitiveCollisionCandidate box_b = box_proxy({1, 0, 0}, 1.0);
    auto box_penetration = collision::primitive_pair_max_penetration(box_a, box_b);
    require(static_cast<bool>(box_penetration) &&
            near(box_penetration.value(), 1.0, 1e-12),
        "offset unit boxes penetrate by their minimal translation depth");

    const collision::PrimitiveCollisionCandidate far_a = sphere_proxy({0, 0, 0}, 1.0);
    const collision::PrimitiveCollisionCandidate far_b = sphere_proxy({5, 0, 0}, 1.0);
    require(near(collision::clearance_violation(far_a, far_b, 2.0), 0.0, 1e-12),
        "satisfied clearance never reports a violation");
    require(near(collision::clearance_violation(far_a, far_b, 4.0), 1.0, 1e-12),
        "clearance violation is the shortfall below the requirement");

    auto metrics = collision::compute_pair_metrics(far_a, far_b, 3.5);
    require(static_cast<bool>(metrics) &&
            near(metrics.value().volume_overlap_residual, 0.0, 1e-12) &&
            near(metrics.value().max_penetration_depth, 0.0, 1e-12) &&
            near(metrics.value().clearance_violation, 0.5, 1e-12),
        "pair metrics fill every measured term");
}

void threshold_policy_refuses_exceeded_error() {
    using namespace carto;
    collision::CollisionErrorThresholdPolicy policy;
    policy.max_one_sided_hausdorff = 1.0;
    require(static_cast<bool>(policy.validate()), "threshold policy validates");

    collision::CollisionErrorMetrics within;
    within.one_sided_hausdorff = 0.5;
    require(static_cast<bool>(collision::evaluate_collision_error(within, policy)),
        "metrics within budget pass the threshold policy");

    collision::CollisionErrorMetrics exceeded;
    exceeded.one_sided_hausdorff = 1.5;
    const auto refused = collision::evaluate_collision_error(exceeded, policy);
    require(!refused &&
            refused.error().code == core::ErrorCode::validation_failed,
        "exceeded error refuses with validation_failed");

    const auto first_digest = policy.canonical_digest();
    const auto second_digest = policy.canonical_digest();
    require(first_digest && second_digest &&
            first_digest.value() == second_digest.value(),
        "threshold policy digest is deterministic");
}

void compound_search_partitions_disjoint_clusters() {
    using namespace carto;
    const geometry::CompiledMesh mesh = two_cubes_mesh();
    collision::CompoundSearchOptions options;
    options.target_vertices_per_part = 8U;
    const auto search = collision::search_compound_candidate(mesh, options);
    require(static_cast<bool>(search), "compound search succeeds on two cubes");
    const collision::CompoundCollisionCandidate& candidate = search.value();
    require(candidate.parts.size() == 2U,
        "two disjoint cubes partition into two parts");
    require(static_cast<bool>(candidate.validate()),
        "compound candidate validates");
    require(near(candidate.aggregate_error, 0.0, 1e-9),
        "exact box parts leave no aggregate error");
    require(near(candidate.total_volume, 2.0, 1e-9),
        "compound volume sums the part volumes");
    require(candidate.fitted_vertex_count == 16U,
        "compound candidate names its fitted vertex count");

    const auto repeat = collision::search_compound_candidate(mesh, options);
    require(repeat && repeat.value().candidate_digest == candidate.candidate_digest,
        "compound search digests are reproducible");

    collision::CompoundSearchOptions oversized;
    oversized.max_parts = collision::CompoundSearchOptions::kHardMaxParts + 1U;
    require(!collision::search_compound_candidate(mesh, oversized),
        "compound search refuses options outside their bounds");
}

void convex_proxy_admission_is_capability_gated() {
    using namespace carto;
    const geometry::CompiledMesh octahedron = octahedron_mesh();
    const std::vector<core::Vec3d> hull = octahedron.positions;

    const auto refused = collision::admit_convex_proxy_candidate(
        "public.octahedron", core::Revision{1U}, digest("octahedron-source"),
        hull, octahedron, collision::ConvexProxyCapability::unsupported);
    require(!refused && refused.error().code == core::ErrorCode::unsupported,
        "unsupported capability refuses convex admission explicitly");

    const auto admitted = collision::admit_convex_proxy_candidate(
        "public.octahedron", core::Revision{1U}, digest("octahedron-source"),
        hull, octahedron, collision::ConvexProxyCapability::cpu_reference);
    require(static_cast<bool>(admitted), "reference capability admits convex proxy");
    require(admitted.value().status ==
            collision::ConvexProxyStatus::admitted_reference_evidence,
        "admitted convex proxy labels its evidence tier");
    require(near(admitted.value().one_sided_hausdorff_evidence, 0.0),
        "donor hull vertices cover the octahedron vertices exactly");
    require(static_cast<bool>(admitted.value().validate()),
        "convex proxy candidate validates and is digest-bound");

    const auto empty = collision::admit_convex_proxy_candidate(
        "public.octahedron", core::Revision{1U}, digest("octahedron-source"),
        {}, octahedron, collision::ConvexProxyCapability::cpu_reference);
    require(!empty && empty.error().code == core::ErrorCode::validation_failed,
        "empty donor hull refuses with validation_failed");
}

void triangle_candidate_binds_donor_pages() {
    using namespace carto;
    const web_geometry::ClusterHierarchy hierarchy = donor_hierarchy();
    const auto bound = collision::bind_triangle_collision_candidate(
        "public.terrain", core::Revision{3U}, hierarchy);
    require(static_cast<bool>(bound), "triangle candidate binds the valid donor");
    const collision::TriangleCollisionCandidate& candidate = bound.value();
    require(candidate.bound_pages.size() == 2U && candidate.bound_pages[0] == 3U &&
            candidate.bound_pages[1] == 7U,
        "bound pages are sorted and unique");
    require(candidate.triangle_count == 2U,
        "triangle count sums the assigned cluster triangles");
    require(near(candidate.geometric_error_evidence, 0.25),
        "geometric error evidence is the worst assigned cluster error");
    require(candidate.hierarchy_digest == hierarchy.digest,
        "candidate binds the donor hierarchy digest");
    require(static_cast<bool>(candidate.validate()),
        "triangle candidate validates");

    web_geometry::ClusterHierarchy unassigned = hierarchy;
    for (web_geometry::ClusterRecord& cluster : unassigned.clusters) {
        cluster.page = web_geometry::kUnassignedRuntimePage;
    }
    unassigned.digest = {};
    const std::string canonical = unassigned.canonical();
    unassigned.digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
    require(!collision::bind_triangle_collision_candidate(
            "public.terrain", core::Revision{3U}, unassigned),
        "donor hierarchy without assigned pages fails closed");

    const auto repeat = collision::bind_triangle_collision_candidate(
        "public.terrain", core::Revision{3U}, hierarchy);
    require(repeat && repeat.value().candidate_digest == candidate.candidate_digest,
        "triangle candidate digests are reproducible");
}

} // namespace

int main() {
    box_fit_on_unit_cube_is_exact();
    sphere_fit_matches_known_answers();
    candidate_generation_is_deterministic();
    error_metrics_match_analytic_cases();
    threshold_policy_refuses_exceeded_error();
    compound_search_partitions_disjoint_clusters();
    convex_proxy_admission_is_capability_gated();
    triangle_candidate_binds_donor_pages();
    std::cout << "collision candidate tests passed\n";
    return EXIT_SUCCESS;
}
