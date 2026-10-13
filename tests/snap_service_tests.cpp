#include <carto/editor/snap_service.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using carto::core::Revision;
using carto::core::Vec3d;
using carto::editor::SnapCandidate;
using carto::editor::SnapKind;
using carto::editor::SnapProfile;
using carto::editor::SnapSession;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-snap-service-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

SnapCandidate vertex(std::uint64_t id, Vec3d position, Revision revision) {
    SnapCandidate candidate;
    candidate.kind = SnapKind::vertex;
    candidate.vertex = carto::geometry::VertexId{id};
    candidate.position = position;
    candidate.source_revision = revision;
    return candidate;
}

SnapCandidate edge(std::uint64_t id, Vec3d position, Revision revision) {
    SnapCandidate candidate;
    candidate.kind = SnapKind::edge_midpoint;
    candidate.edge = carto::geometry::EdgeId{id};
    candidate.position = position;
    candidate.source_revision = revision;
    return candidate;
}

void profile_prefers_feature_order_and_recomputes_distance() {
    const Revision revision{4U};
    const std::vector<SnapCandidate> candidates{
        edge(2U, {0.01, 0.0, 0.0}, revision),
        vertex(1U, {0.20, 0.0, 0.0}, revision),
    };
    SnapProfile profile;
    SnapSession session;
    const auto result = carto::editor::acquire_snap(
        candidates, {}, revision, profile, session);
    require(result && result.value().candidate.has_value(),
        "snap service acquires a candidate within radius");
    require(result.value().candidate->kind == SnapKind::vertex &&
            result.value().candidate->vertex == carto::geometry::VertexId{1U} &&
            result.value().candidate->distance == 0.20,
        "feature priority wins and distance is recomputed from origin");
}

void hysteresis_retains_and_switches_deterministically() {
    const Revision revision{7U};
    SnapProfile profile;
    profile.acquire_radius = 0.30;
    profile.retain_radius = 0.40;
    profile.break_radius = 0.80;
    profile.switch_margin = 0.05;
    profile.priority = {
        SnapKind::edge_midpoint, SnapKind::vertex, SnapKind::face_center};
    SnapSession session;

    const std::vector<SnapCandidate> initial{
        vertex(1U, {0.10, 0.0, 0.0}, revision)};
    const auto first = carto::editor::acquire_snap(
        initial, {}, revision, profile, session);
    require(first && first.value().candidate.has_value() && !first.value().retained,
        "first candidate is acquired rather than retained");

    const std::vector<SnapCandidate> near_competitor{
        vertex(1U, {0.20, 0.0, 0.0}, revision),
        edge(2U, {0.18, 0.0, 0.0}, revision)};
    const auto held = carto::editor::acquire_snap(
        near_competitor, {}, revision, profile, session);
    require(held && held.value().candidate.has_value() && held.value().retained &&
            held.value().candidate->vertex == carto::geometry::VertexId{1U},
        "held candidate survives a near-equal competitor");

    const std::vector<SnapCandidate> better_competitor{
        vertex(1U, {0.30, 0.0, 0.0}, revision),
        edge(2U, {0.01, 0.0, 0.0}, revision)};
    const auto switched = carto::editor::acquire_snap(
        better_competitor, {}, revision, profile, session);
    require(switched && switched.value().candidate.has_value() &&
            switched.value().candidate->edge == carto::geometry::EdgeId{2U} &&
            switched.value().switched && !switched.value().retained,
        "a materially better candidate breaks hysteresis");

    const std::vector<SnapCandidate> broken_candidate{
        edge(2U, {2.0, 0.0, 0.0}, revision)};
    const auto broken = carto::editor::acquire_snap(
        broken_candidate, {}, revision, profile, session);
    require(broken && !broken.value().candidate.has_value() && !session.locked(),
        "a candidate beyond the break radius clears the transient lock");
}

void stale_and_malformed_inputs_fail_closed() {
    const Revision revision{9U};
    SnapProfile profile;
    SnapSession session;
    const std::vector<SnapCandidate> stale_candidate{
        vertex(1U, {}, Revision{8U})};
    const auto stale = carto::editor::acquire_snap(
        stale_candidate, {}, revision, profile, session);
    require(!stale && stale.error().code == carto::core::ErrorCode::stale_data,
        "stale candidates are rejected");

    auto malformed = vertex(1U, {}, revision);
    malformed.edge = carto::geometry::EdgeId{2U};
    const std::vector<SnapCandidate> ambiguous{malformed};
    const auto invalid = carto::editor::acquire_snap(
        ambiguous, {}, revision, profile, session);
    require(!invalid, "ambiguous candidate identity is rejected");

    auto non_finite = vertex(1U, {}, revision);
    non_finite.position.x = std::numeric_limits<double>::quiet_NaN();
    const std::vector<SnapCandidate> invalid_position{non_finite};
    const auto invalid_result = carto::editor::acquire_snap(
        invalid_position, {}, revision, profile, session);
    require(!invalid_result, "non-finite candidate position is rejected");

    SnapProfile invalid_profile = profile;
    invalid_profile.break_radius = 0.1;
    const auto invalid_policy = carto::editor::acquire_snap(
        {}, {}, revision, invalid_profile, session);
    require(!invalid_policy, "inverted hysteresis bounds are rejected");
}

void disabled_profile_does_not_leave_a_lock() {
    const Revision revision{12U};
    SnapProfile profile;
    profile.enabled = false;
    SnapSession session;
    const std::vector<SnapCandidate> candidates{
        vertex(1U, {}, revision)};
    const auto result = carto::editor::acquire_snap(
        candidates, {}, revision, profile, session);
    require(result && !result.value().candidate.has_value() && !session.locked(),
        "disabled snap profile clears transient state without authoring mutation");
}

void scene_query_is_world_space_instance_aware_and_revision_bound() {
    TempDirectory directory;
    carto::journal::Journal journal(directory.path() / "snap-events.log");
    carto::project::ProjectDocument project;
    auto staged = carto::project::ProjectTransaction::begin(
        project, journal, "snap-service-tests");
    require(static_cast<bool>(staged), "fixture transaction starts at the journal tail");

    carto::geometry::EditableMesh mesh;
    const auto first_vertex = mesh.add_vertex({0.0, 0.0, 0.0});
    const auto second_vertex = mesh.add_vertex({1.0, 0.0, 0.0});
    const auto third_vertex = mesh.add_vertex({0.0, 1.0, 0.0});
    require(first_vertex && second_vertex && third_vertex,
        "mesh fixture vertices are created");
    require(static_cast<bool>(mesh.add_face({first_vertex.value(), second_vertex.value(),
        third_vertex.value()})), "mesh fixture face is created");
    const auto asset = staged.value().add_mesh(std::move(mesh));
    require(static_cast<bool>(asset), "mesh fixture is stored in the project");

    carto::core::Transform first_transform;
    first_transform.translation = {0.0, 0.0, 0.0};
    carto::core::Transform second_transform;
    second_transform.translation = {0.2, 0.0, 0.0};
    const auto first_object = staged.value().create_object("first instance", first_transform);
    const auto second_object = staged.value().create_object("second instance", second_transform);
    require(first_object && second_object, "scene instances are created");
    require(static_cast<bool>(staged.value().attach_mesh(first_object.value(), asset.value())) &&
            static_cast<bool>(staged.value().attach_mesh(second_object.value(), asset.value())),
        "both scene instances reference the same mesh asset");

    carto::scene::SceneObject hidden;
    hidden.id = carto::scene::ObjectId{100U};
    hidden.name = "hidden instance";
    hidden.local_transform.translation = {0.18, 0.0, 0.0};
    hidden.mesh_asset = asset.value();
    hidden.visible = false;
    require(static_cast<bool>(staged.value().insert_object(hidden)),
        "hidden scene instance is inserted");
    carto::scene::SceneObject hidden_parent;
    hidden_parent.id = carto::scene::ObjectId{101U};
    hidden_parent.name = "hidden parent";
    hidden_parent.visible = false;
    require(static_cast<bool>(staged.value().insert_object(hidden_parent)),
        "hidden parent is inserted");
    carto::scene::SceneObject child_of_hidden;
    child_of_hidden.id = carto::scene::ObjectId{102U};
    child_of_hidden.name = "visible child of hidden parent";
    child_of_hidden.parent = hidden_parent.id;
    child_of_hidden.mesh_asset = asset.value();
    require(static_cast<bool>(staged.value().insert_object(child_of_hidden)),
        "visible mesh child of hidden parent is inserted");
    require(static_cast<bool>(staged.value().commit("snap.fixture.create")),
        "fixture scene is published through a journaled transaction");

    carto::editor::SnapSettings settings;
    settings.enabled = true;
    settings.kind = SnapKind::vertex;
    const Vec3d origin{0.18, 0.0, 0.0};
    const auto queried = carto::editor::find_snap_candidates(
        project, origin, settings, 0.25);
    require(queried && queried.value().size() == 2U,
        "query returns visible candidates from each mesh instance only");
    require(queried.value()[0].object == first_object.value() &&
            queried.value()[1].object == second_object.value(),
        "candidate ownership is deterministic and distinguishes instance identity");
    require(queried.value()[0].mesh_asset == asset.value() &&
            queried.value()[1].mesh_asset == asset.value(),
        "candidate receipts retain their mesh asset identity");
    require(queried.value()[0].source_revision == project.revision() &&
            queried.value()[1].source_revision == project.revision(),
        "scene candidates bind to the exact project revision");
    require(queried.value()[0].mesh_revision == queried.value()[1].mesh_revision &&
            std::abs(queried.value()[1].position.x - 0.2) < 1e-12,
        "world-space candidate positions preserve shared mesh revision provenance");

    SnapProfile profile;
    profile.acquire_radius = 0.25;
    profile.retain_radius = 0.30;
    profile.break_radius = 0.40;
    SnapSession session;
    const auto acquired = carto::editor::acquire_snap(
        queried.value(), origin, project.revision(), profile, session);
    require(acquired && acquired.value().candidate.has_value() &&
            acquired.value().candidate->object == second_object.value(),
        "snap arbitration selects the nearest world-space scene instance");

    auto transform_update = carto::project::ProjectTransaction::begin(
        project, journal, "snap-service-tests");
    require(transform_update && transform_update.value().set_object_transform(
        first_object.value(), carto::core::Transform::identity()) &&
        transform_update.value().commit("snap.fixture.transform"),
        "project transform mutation advances revision through the journal");
    const auto stale = carto::editor::acquire_snap(
        queried.value(), origin, project.revision(), profile, session);
    require(!stale && stale.error().code == carto::core::ErrorCode::stale_data,
        "scene candidates cannot survive a later transform revision");
}

} // namespace

int main() {
    profile_prefers_feature_order_and_recomputes_distance();
    hysteresis_retains_and_switches_deterministically();
    stale_and_malformed_inputs_fail_closed();
    disabled_profile_does_not_leave_a_lock();
    scene_query_is_world_space_instance_aware_and_revision_bound();
    std::cout << "snap service tests passed\n";
    return EXIT_SUCCESS;
}
