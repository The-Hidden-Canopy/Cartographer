#include <carto/geometry/sweep.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

geometry::CurvePath line_path(
    std::string identity, core::Vec3d start, core::Vec3d end,
    core::Revision revision = core::Revision{1U}) {
    const auto delta = end - start;
    geometry::CurvePath path;
    path.source_identity = std::move(identity);
    path.source_revision = revision;
    path.segments.push_back({start, start + delta * (1.0 / 3.0),
        start + delta * (2.0 / 3.0), end});
    return path;
}

std::vector<core::Vec2d> regular_profile(std::size_t count, double radius) {
    std::vector<core::Vec2d> points;
    points.reserve(count);
    constexpr double kTau = 6.283185307179586476925286766559;
    for (std::size_t index = 0U; index < count; ++index) {
        const double angle = kTau * static_cast<double>(index) /
            static_cast<double>(count);
        points.push_back({radius * std::cos(angle), radius * std::sin(angle)});
    }
    return points;
}

geometry::SweepSource make_source(
    const geometry::CurvePath& path,
    std::string identity,
    std::vector<core::Vec2d> profile,
    double start_scale = 1.0,
    double end_scale = 1.0) {
    geometry::SweepSource source;
    source.source_identity = std::move(identity);
    source.source_revision = core::Revision{1U};
    source.centerline_identity = path.source_identity;
    source.centerline_revision = path.source_revision;
    source.profile = std::move(profile);
    source.reference_up = {0.0, 0.0, 1.0};
    source.samples_per_segment = 4U;
    source.start_scale = start_scale;
    source.end_scale = end_scale;
    source.cap_policy = geometry::SweepCapPolicy::both;
    return source;
}

bool same_mesh(const geometry::EditableMesh& left, const geometry::EditableMesh& right) {
    const auto left_vertices = left.vertices_sorted();
    const auto right_vertices = right.vertices_sorted();
    const auto left_faces = left.faces_sorted();
    const auto right_faces = right.faces_sorted();
    if (left_vertices.size() != right_vertices.size() || left_faces.size() != right_faces.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left_vertices.size(); ++index) {
        if (left_vertices[index].id != right_vertices[index].id ||
            left_vertices[index].position.x != right_vertices[index].position.x ||
            left_vertices[index].position.y != right_vertices[index].position.y ||
            left_vertices[index].position.z != right_vertices[index].position.z) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < left_faces.size(); ++index) {
        if (left_faces[index].id != right_faces[index].id ||
            left_faces[index].vertices != right_faces[index].vertices) {
            return false;
        }
    }
    return true;
}

void sweep_source_is_canonical_and_round_trips() {
    const auto path = line_path("pipe.centerline", {0.0, 0.0, 0.0}, {0.0, 4.0, 0.0});
    const auto source = make_source(path, "pipe.source", regular_profile(12U, 0.15));
    require(static_cast<bool>(source.validate()),
        "valid sweep authoring source passes bounded validation");
    const auto encoded = source.serialize();
    const auto reopened = geometry::SweepSource::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "sweep source round-trips to its canonical serialized form");
    require(!geometry::SweepSource::deserialize(encoded + "UNEXPECTED\n"),
        "sweep source rejects trailing data instead of ignoring appended payload");
    require(!geometry::SweepSource::deserialize(
        "CARTOGRAPHER_SWEEP_SOURCE 99\n"),
        "sweep source rejects unknown future schemas");
}

void pipe_geometry_is_deterministic_and_revision_bound() {
    const auto path = line_path("pipe.centerline", {0.0, 0.0, 0.0}, {0.0, 4.0, 0.0});
    const auto source = make_source(path, "pipe.source", regular_profile(12U, 0.15));
    const auto first = geometry::build_sweep_mesh(source, path);
    const auto second = geometry::build_sweep_mesh(source, path);
    require(first && second && first.value().mesh.validate(),
        "pipe source generates a valid closed editable mesh");
    require(first.value().mesh.vertex_count() == 60U &&
            first.value().mesh.face_count() == 50U,
        "four centerline intervals generate five stable 12-point rings and two caps");
    require(same_mesh(first.value().mesh, second.value().mesh),
        "identical source inputs allocate identical local topology and positions");
    require(first.value().source_identity == source.source_identity &&
            first.value().source_revision == source.source_revision &&
            first.value().centerline_identity == path.source_identity &&
            first.value().centerline_revision == path.source_revision,
        "derived mesh product retains exact source and centerline lineage");

    auto stale_path = path;
    stale_path.source_revision = core::Revision{2U};
    require(!geometry::build_sweep_mesh(source, stale_path),
        "a changed centerline revision cannot silently reuse a sweep source");
    stale_path = path;
    stale_path.source_identity = "other.centerline";
    require(!geometry::build_sweep_mesh(source, stale_path),
        "a same-shaped but differently identified centerline is rejected");
}

void pipe_railing_and_branch_authoring_fixtures_generate_natively() {
    const auto pipe_path = line_path(
        "fixture.pipe.path", {0.0, 0.0, 0.0}, {0.0, 2.0, 0.0});
    const auto pipe = geometry::build_sweep_mesh(make_source(
        pipe_path, "fixture.pipe.source", regular_profile(10U, 0.08)), pipe_path);
    require(pipe && pipe.value().mesh.validate(),
        "generic circular profile sweep builds the pipe fixture");

    const std::vector<core::Vec2d> rail_profile = {
        {-0.04, -0.04}, {0.04, -0.04}, {0.04, 0.04}, {-0.04, 0.04}};
    const auto top_path = line_path(
        "fixture.railing.top.path", {0.0, 1.0, 0.0}, {3.0, 1.0, 0.0});
    const auto bottom_path = line_path(
        "fixture.railing.bottom.path", {0.0, 0.1, 0.0}, {3.0, 0.1, 0.0});
    const auto post_left_path = line_path(
        "fixture.railing.post-left.path", {0.0, 0.1, 0.0}, {0.0, 1.0, 0.0});
    const auto post_right_path = line_path(
        "fixture.railing.post-right.path", {3.0, 0.1, 0.0}, {3.0, 1.0, 0.0});
    const auto top = geometry::build_sweep_mesh(make_source(
        top_path, "fixture.railing.top", rail_profile), top_path);
    const auto bottom = geometry::build_sweep_mesh(make_source(
        bottom_path, "fixture.railing.bottom", rail_profile), bottom_path);
    auto post_source = make_source(post_left_path, "fixture.railing.post-left", rail_profile);
    post_source.reference_up = {0.0, 0.0, 1.0};
    const auto left_post = geometry::build_sweep_mesh(post_source, post_left_path);
    post_source.source_identity = "fixture.railing.post-right";
    post_source.centerline_identity = post_right_path.source_identity;
    const auto right_post = geometry::build_sweep_mesh(post_source, post_right_path);
    require(top && bottom && left_post && right_post &&
            top.value().mesh.validate() && bottom.value().mesh.validate() &&
            left_post.value().mesh.validate() && right_post.value().mesh.validate(),
        "railing fixture builds two rails and two posts as native source-derived parts");

    geometry::CurvePath branch_path;
    branch_path.source_identity = "fixture.branch.path";
    branch_path.source_revision = core::Revision{1U};
    branch_path.segments.push_back({
        {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.4, 2.0, 0.4}, {1.0, 3.0, 0.8}});
    auto branch_source = make_source(
        branch_path, "fixture.branch.source", regular_profile(12U, 0.12), 1.0, 0.3);
    const auto branch = geometry::build_sweep_mesh(branch_source, branch_path);
    require(branch && branch.value().mesh.validate(),
        "curved tapered branch fixture is generated as a validated mesh");
    const auto branch_vertices = branch.value().mesh.vertices_sorted();
    const auto profile_count = branch_source.profile.size();
    const double start_radius = (branch_vertices.front().position -
        branch_path.segments.front().p0).length();
    const double end_radius = (branch_vertices[branch_vertices.size() - profile_count].position -
        branch_path.segments.front().p3).length();
    require(start_radius > end_radius * 2.0,
        "tapered branch fixture preserves the authored endpoint scale change");
}

void unsupported_frames_profiles_and_joints_fail_closed() {
    const auto path = line_path("tube.path", {0.0, 0.0, 0.0}, {0.0, 3.0, 0.0});
    auto source = make_source(path, "tube.source", regular_profile(8U, 0.1));
    source.reference_up = {0.0, 1.0, 0.0};
    require(!geometry::build_sweep_mesh(source, path),
        "frame parallel to the path is rejected instead of choosing an arbitrary roll");

    source.reference_up = {0.0, 0.0, 1.0};
    source.profile = {{-1.0, -1.0}, {1.0, -1.0}, {0.0, 0.0},
        {1.0, 1.0}, {-1.0, 1.0}};
    require(!source.validate(), "concave profile is rejected before topology generation");

    source.profile = regular_profile(5U, 1.0);
    const auto star = std::vector<core::Vec2d>{
        source.profile[0U], source.profile[2U], source.profile[4U],
        source.profile[1U], source.profile[3U]};
    source.profile = star;
    require(!source.validate(), "self-intersecting star profile is rejected");

    geometry::CurvePath kinked;
    kinked.source_identity = "kinked.path";
    kinked.source_revision = core::Revision{1U};
    kinked.segments = {
        {{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 2.0, 0.0}, {0.0, 3.0, 0.0}},
        {{0.0, 3.0, 0.0}, {1.0, 3.0, 0.0}, {2.0, 3.0, 0.0}, {3.0, 3.0, 0.0}},
    };
    auto kinked_source = make_source(kinked, "kinked.sweep", regular_profile(8U, 0.1));
    require(!geometry::build_sweep_mesh(kinked_source, kinked),
        "tangent-discontinuous segment joints are not hidden by frame transport");

    auto oversized = make_source(path, "oversized.sweep", regular_profile(8U, 0.1));
    oversized.samples_per_segment = 0U;
    require(!oversized.validate(), "zero sample density is rejected at source admission");
}

} // namespace

int main() {
    sweep_source_is_canonical_and_round_trips();
    pipe_geometry_is_deterministic_and_revision_bound();
    pipe_railing_and_branch_authoring_fixtures_generate_natively();
    unsupported_frames_profiles_and_joints_fail_closed();
    std::cout << "All procedural sweep tests passed.\n";
    return EXIT_SUCCESS;
}
