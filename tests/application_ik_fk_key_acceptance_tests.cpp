#include <carto/application/application.hpp>
#include <carto/character/rig_record.hpp>
#include <carto/journal/journal.hpp>
#include <carto/pose/ik_fk_mode_key_record.hpp>
#include <carto/ui/ui.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

using namespace carto;
static_assert(!std::is_same_v<application::HistoricalAutorigView, application::AcceptedAutorigView>);
namespace {
struct CommaPunctuation final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
};
void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
character::AutorigConstraintProvenance human(std::string id) {
    return {std::move(id), "artist", "reviewed", "", character::AutorigConstraintOrigin::human_authored};
}
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("carto-app-autorig-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    const auto project_path = path / "rig.carto";
    application::ApplicationSession session;
    geometry::EditableMesh mesh;
    auto root = mesh.add_vertex({0, 0, 0});
    auto mid = mesh.add_vertex({0, 1, 0});
    auto child = mesh.add_vertex({0.35, 2, 0});
    auto side = mesh.add_vertex({1, 0, 0});
    require(root && mid && child && side && mesh.add_face({root.value(), mid.value(), side.value()}) &&
        mesh.add_face({mid.value(), child.value(), side.value()}), "mesh fixture");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(
        session, application::CreateMeshObjectAction{"source", mesh})), "create source mesh");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(
        session, application::SaveProjectAction{project_path})), "save baseline");
    auto snapshot = session.snapshot();
    require(snapshot.objects.size() == 1 && snapshot.objects.front().object.mesh_asset.has_value(), "source identity");
    const auto mesh_id = *snapshot.objects.front().object.mesh_asset;
    application::AcceptAutorigRequest request;
    request.expected_project_generation = snapshot.project_generation;
    request.expected_project_revision = snapshot.project_revision;
    request.identity = "body";
    request.analysis = {{mesh_id, hash("caller source")}, mesh.revision(), character::AutorigAxis::z, 0, 1e-7, {}, {}};
    const auto analysis = character::analyze_autorig_features(mesh, request.analysis);
    require(static_cast<bool>(analysis), "analyze source");
    character::AutorigConstraintSet constraints{request.analysis.source, mesh.revision(), analysis.value().analysis_digest,
        {{"root", "root", std::nullopt, character::AutorigPartSide::center},
         {"mid", "mid", std::string("root"), character::AutorigPartSide::center},
         {"child", "child", std::string("mid"), character::AutorigPartSide::center}},
        {{"a", "root", "root", character::AutorigPartSide::center, root.value(), human("root")},
         {"m", "mid", "mid", character::AutorigPartSide::center, mid.value(), human("mid")},
         {"b", "child", "child", character::AutorigPartSide::center, child.value(), human("child")}}, {}, {}};
    auto opened = character::AutorigCandidateReview::begin(mesh, request.analysis, constraints);
    require(static_cast<bool>(opened), "open review");
    auto review = std::move(opened).value();
    request.candidates = review.candidates();
    const auto& candidate = request.candidates.ranked_candidates.front();
    require(static_cast<bool>(review.select_candidate(candidate.digest)), "select candidate");
    auto acceptance = review.accept_review(human("decision"));
    require(static_cast<bool>(acceptance), "review acceptance");
    request.acceptance = acceptance.value();
    std::uint64_t joint_id = 100;
    for (const auto& joint : candidate.joints) {
        character::AutorigFinalNode node;
        node.preview_joint = joint.id; node.durable_joint = {joint_id++};
        node.local_rest.translation = joint.parent ? core::Vec3d{joint_id == 103 ? 0.35 : 0.0, 1, 0} : core::Vec3d{};
        node.kind = kinematic::JointKind::ball;
        node.node_class = kinematic::NodeClass::skinned; node.semantic_type = joint.semantic_role;
        request.nodes.push_back(node);
    }
    auto mismatched_nodes = request.nodes;
    mismatched_nodes.back().local_rest.translation.x += 0.1;
    require(!character::prepare_accepted_autorig_graph(
        mesh, request.analysis, request.candidates, request.acceptance, mismatched_nodes),
        "rest graph must reconstruct the reviewed source anchors");
    auto graph = character::prepare_accepted_autorig_graph(
        mesh, request.analysis, request.candidates, request.acceptance, request.nodes);
    if (!graph) std::cerr << "DIAGNOSTIC graph: " << graph.error().message << "\n";
    require(static_cast<bool>(graph), "prepare graph");
    request.rig.id = character::RigId{7};
    request.rig.kinematic_graph_digest = graph.value().graph_digest;
    request.rig.joint_limits.digest = request.limits.canonical_digest().value();
    for (std::uint64_t i = 1; i <= 6; ++i) {
        character::ControlDefinition control;
        control.id = {i};
        if (i <= 3) control.affected_joints.push_back(request.nodes[i - 1].durable_joint);
        if (i == 4 || i == 5) {
            control.allowed_axes = character::kAxisTranslateX | character::kAxisTranslateY | character::kAxisTranslateZ;
            for (std::size_t a = 0; a < 3; ++a) { control.axis_minimum[a] = -100; control.axis_maximum[a] = 100; }
        }
        if (i == 6) {
            control.allowed_axes = character::kAxisTranslateX;
            control.axis_minimum[0] = 0; control.axis_maximum[0] = 1;
        }
        require(static_cast<bool>(request.rig.control_graph.insert(control)), "insert rig control");
    }
    request.source_binding.mesh_asset = 77;
    request.source_binding.mesh_digest = hash("caller source binding");
    request.source_binding.graph_digest = graph.value().graph_digest;
    request.source_binding.influences.emplace(0, std::vector<kinematic::JointInfluence>{{request.nodes.front().durable_joint, 1.0}});
    request.source_layer.layer_id = character::RigLayerId{1};
    request.source_layer.mesh_topology_digest = request.source_binding.mesh_digest;
    request.source_layer.joint_graph_digest = request.source_binding.graph_digest;
    request.source_layer.influenced_vertex_count = 1;
    request.source_layer.total_influence_count = 1;
    request.source_layer.provenance_digest = hash("authored");
    require(request.correspondence.map(0, {{0,1}}) && request.correspondence.map(1, {{0,1}}) &&
        request.correspondence.map(2, {{0,1}}) && request.correspondence.map(3, {{0,1}}), "correspondence");
    request.transfer_options.output_layer_id = character::RigLayerId{2};
    request.transfer_options.output_mesh_asset = mesh_id;
    request.transfer_options.output_mesh_digest = request.analysis.source.content_digest;
    require(static_cast<bool>(application::HumanApplicationAccess::commit_autorig(session, request)), "accept rig");
    snapshot = session.snapshot();
    pose::IkFkAuthoring authored;
    authored.rig = request.rig.id;
    authored.rig_digest = character::rig_definition_digest(request.rig).value();
    authored.graph_digest = graph.value().graph_digest;
    authored.limits_digest = request.rig.joint_limits.digest;
    authored.chain = {request.nodes[0].durable_joint, request.nodes[1].durable_joint, request.nodes[2].durable_joint};
    authored.fk_root = {1}; authored.fk_mid = {2}; authored.fk_effector = {3};
    authored.ik_target = {4}; authored.ik_pole = character::ControlId{5}; authored.mode_control = {6};
    application::ConfigureAutorigIkFkRequest configure;
    configure.expected_project_generation = snapshot.project_generation;
    configure.expected_project_revision = snapshot.project_revision;
    configure.identity = "body"; configure.expected_record_revision = core::Revision{1};
    configure.configurations.push_back(authored);
    require(static_cast<bool>(application::HumanApplicationAccess::configure_autorig_ik_fk(session, configure)),
        "configure explicit IK/FK authoring");
    snapshot = session.snapshot();
    auto configured = session.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision);
    require(configured && configured.value().record_revision == core::Revision{2} &&
        configured.value().bundle.ik_fk.size() == 1,
        "configured IK/FK record is persisted");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(
        session, application::SaveProjectAction{project_path})), "save configured baseline");
    auto saved = project::ProjectDocument::load(project_path);
    require(static_cast<bool>(saved), "load baseline rig digest");
    snapshot = session.snapshot();
    const auto rig_digest = saved.value().accepted_autorigs().at("body").digest;
    ui::UiController ui(session);
    auto current_rig = ui.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision);
    require(current_rig && current_rig.value().record_digest == rig_digest, "UI exposes current rig pin");
    workspace::CharacterSourceBundle source;
    source.definition.id = character::CharacterId{7};
    source.definition.project_namespace = snapshot.source_namespace;
    source.definition.source_revision = request.acceptance.mesh_revision;
    source.definition.source_digest = request.acceptance.mesh_geometry_digest;
    source.definition.root = snapshot.objects.front().object.id;
    source.definition.rig.rig = request.rig.id;
    source.definition.rig.graph_digest = authored.graph_digest;
    source.definition.control_graph.owner = request.rig.id;
    source.definition.control_graph.digest = request.rig.control_graph.canonical_digest().value();
    source.definition.coordinate_policy_digest = hash("policy assertion");
    source.graph = graph.value().graph;
    source.rig = request.rig;
    for (std::uint64_t id : {4U, 5U, 6U}) {
        const std::uint8_t count = id == 6U ? 1U : 3U;
        for (std::uint8_t axis = 0; axis < count; ++axis) {
            workspace::ControlAxisTrack track;
            track.control = character::ControlId{id}; track.axis = axis;
            track.role = id == 4U ? workspace::ControlTrackRole::ik_target :
                id == 5U ? workspace::ControlTrackRole::ik_pole : workspace::ControlTrackRole::ik_fk_mode;
            track.source_namespace = snapshot.source_namespace;
            track.source_revision = request.acceptance.mesh_revision;
            track.reference_frame = "graph-world";
            track.rig_digest = authored.rig_digest; track.graph_digest = authored.graph_digest;
            track.curve.id = motion::MotionCurveId{id * 10U + axis + 1U};
            track.curve.timebase_ticks_per_second = 30U;
            track.curve.interpolation = motion::InterpolationKind::step;
            track.curve.keys = {{{0, 30}, 0.0, 0.0, 0.0}};
            track.curve_digest = track.curve.canonical_digest().value();
            source.control_axis_tracks.push_back(std::move(track));
        }
    }
    workspace::CharacterSourceStore detached;
    require(static_cast<bool>(detached.insert(source)), "valid detached control workspace");
    const auto detached_bytes = detached.serialize();
    application::AdoptControlWorkspaceRequest adopt;
    adopt.expected_project_generation = snapshot.project_generation;
    adopt.expected_project_revision = snapshot.project_revision;
    adopt.rig_identity = "body"; adopt.expected_rig_revision = configured.value().record_revision;
    adopt.expected_rig_digest = rig_digest; adopt.workspace_bytes = detached_bytes;
    adopt.justification = "adopt this control workspace into project history";
    auto stale_adopt = adopt; stale_adopt.expected_rig_digest = hash("different rig");
    require(!application::HumanApplicationAccess::adopt_control_workspace(session, stale_adopt), "stale rig refused");
    auto wrong_frame_store = detached;
    wrong_frame_store.find({7})->control_axis_tracks.front().reference_frame = "other-frame";
    auto wrong_frame = adopt; wrong_frame.workspace_bytes = wrong_frame_store.serialize();
    require(!application::HumanApplicationAccess::adopt_control_workspace(session, wrong_frame), "unbound frame refused");
    auto legacy_store = detached;
    temporal::TemporalChannel legacy_channel;
    legacy_channel.id = {55}; legacy_channel.channel_name = "legacy";
    legacy_channel.target = {temporal::TargetBindingKind::skinned_mesh, "source"};
    legacy_channel.duration = 1.0; legacy_channel.scalar_keyframes = {{0.0, 0.0}};
    legacy_store.find({7})->channel = legacy_channel;
    auto legacy = adopt; legacy.workspace_bytes = legacy_store.serialize();
    require(!application::HumanApplicationAccess::adopt_control_workspace(session, legacy),
        "legacy temporal channel is quarantined rather than rebound");
    require(static_cast<bool>(ui.adopt_control_workspace(adopt)), "adopt workspace");
    snapshot = session.snapshot();
    auto before = session.control_workspace_source(snapshot.project_generation, snapshot.project_revision);
    require(before && before.value().source.serialize() == detached_bytes &&
        before.value().source_namespace == snapshot.source_namespace && before.value().rig_identity == "body" &&
        before.value().rig_revision == configured.value().record_revision && before.value().rig_digest == rig_digest,
        "owned source and exact rig binding are readable");
    auto pose_before = ui.evaluate_control_workspace_pose("body", {7}, {29, 30},
        snapshot.project_generation, snapshot.project_revision);
    require(pose_before && pose_before.value().mode == pose::IkFkMode::fk, "owned tracks evaluate source FK pose");
    require(detached.serialize() == detached_bytes, "detached source was not mutated");
    static_assert(!std::is_copy_constructible_v<application::IkFkKeyReview>);
    auto stale_review = session.review_ik_fk_key("body", character::CharacterId{7}, {30, 30},
        snapshot.project_generation, snapshot.project_revision);
    require(static_cast<bool>(stale_review), "review before unrelated project edit");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(
        session, application::CreateMeshObjectAction{"other", mesh})), "unrelated edit");
    require(!application::HumanApplicationAccess::accept_ik_fk_key(session, stale_review.value(), "stale"),
        "stale reviewed source is refused");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})),
        "restore source after unrelated edit");
    snapshot = session.snapshot();
    require(!application::HumanApplicationAccess::accept_ik_fk_key(session, stale_review.value(), "restored"),
        "restored bytes do not revive old review identity");
    core::Transform moved;
    moved.translation.x = 1.0;
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session,
        application::SetObjectTransformAction{source.definition.root, moved, snapshot.project_revision})),
        "move scene root");
    snapshot = session.snapshot();
    require(!session.review_ik_fk_key("body", {7}, {30, 30}, snapshot.project_generation, snapshot.project_revision),
        "nonidentity scene frame is refused");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})),
        "restore supported scene root");
    snapshot = session.snapshot();
    const auto old_locale = std::locale();
    std::locale::global(std::locale(old_locale, new CommaPunctuation));
    require(!session.review_ik_fk_key("body", {7}, {30, 30}, snapshot.project_generation, snapshot.project_revision),
        "nonclassic legacy graph locale is quarantined");
    std::locale::global(old_locale);
    auto reviewed = ui.review_ik_fk_key("body", character::CharacterId{7}, {30, 30},
        snapshot.project_generation, snapshot.project_revision);
    require(reviewed && reviewed.value().active() && !reviewed.value().proposal_digest().is_zero(), "review exact match");
    require(!session.review_ik_fk_key("body", {7}, {0, 30}, snapshot.project_generation, snapshot.project_revision),
        "switch without a preceding tick refused");
    require(before.value().source.serialize() == detached_bytes, "review did not mutate source");
    application::ApplicationSession other;
    require(!application::HumanApplicationAccess::accept_ik_fk_key(other, reviewed.value(), "wrong session"),
        "review cannot cross sessions");
    require(!application::HumanApplicationAccess::accept_ik_fk_key(session, reviewed.value(), " "), "reason required");
    const auto journal_path = std::filesystem::path(project_path.string() + ".journal");
    const auto journal_backup = path / "saved-journal";
    std::error_code file_error;
    std::filesystem::rename(journal_path, journal_backup, file_error);
    require(!file_error && std::filesystem::create_directory(journal_path), "block journal");
    const auto before_blocked = session.snapshot();
    require(!application::HumanApplicationAccess::accept_ik_fk_key(session, reviewed.value(), "blocked append") &&
        reviewed.value().active(), "failed journal append retains review");
    const auto after_blocked = session.snapshot();
    auto blocked_source = session.control_workspace_source(after_blocked.project_generation, after_blocked.project_revision);
    require(after_blocked.project_revision == before_blocked.project_revision &&
        after_blocked.undo_count == before_blocked.undo_count && blocked_source &&
        blocked_source.value().source.serialize() == detached_bytes, "failed append restores source and history");
    require(std::filesystem::remove(journal_path), "remove blocked journal");
    std::filesystem::rename(journal_backup, journal_path, file_error);
    require(!file_error, "restore journal");
    require(static_cast<bool>(ui.accept_ik_fk_key(reviewed.value(), "accept matched key")),
        "accept exact reviewed keys");
    require(!reviewed.value().active() && !application::HumanApplicationAccess::accept_ik_fk_key(session, reviewed.value(), "reuse"),
        "review is consumed exactly once");
    snapshot = session.snapshot();
    auto after = session.control_workspace_source(snapshot.project_generation, snapshot.project_revision);
    require(after && after.value().source.serialize() != detached_bytes, "accepted keys are project-owned");
    auto pose_after = ui.evaluate_control_workspace_pose("body", {7}, {30, 30},
        snapshot.project_generation, snapshot.project_revision);
    require(pose_after && pose_after.value().mode == pose::IkFkMode::ik, "accepted tracks evaluate as IK");
    for (auto joint : {authored.chain.root, authored.chain.mid, authored.chain.effector}) {
        const auto& a = pose_before.value().pose.world.at(joint);
        const auto& b = pose_after.value().pose.world.at(joint);
        require((a.translation - b.translation).length() <= authored.max_position_jump,
            "reviewed source and accepted result stay within position bound");
        const auto qa = a.rotation.normalized(); const auto qb = b.rotation.normalized();
        const double q = std::abs(qa.x*qb.x + qa.y*qb.y + qa.z*qb.z + qa.w*qb.w);
        require(2.0 * std::acos(std::clamp(q, 0.0, 1.0)) <= 1e-6, "joint orientation remains continuous");
    }
    const auto* keyed = after.value().source.find(character::CharacterId{7});
    require(keyed && keyed->control_axis_tracks.size() == 7U, "seven keyed components remain present");
    for (const auto& track : keyed->control_axis_tracks)
        require(track.curve.keys.size() == 2U && track.curve.keys.back().time.tick == 30 &&
            (track.control != authored.mode_control || track.curve.keys.back().value == authored.ik_mode_value),
            "each component has the exact new tick and mode");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})), "undo key acceptance");
    snapshot = session.snapshot();
    auto undone = session.control_workspace_source(snapshot.project_generation, snapshot.project_revision);
    require(undone && undone.value().source.serialize() == detached_bytes, "undo restores exact source");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::RedoAction{})), "redo key acceptance");
    snapshot = session.snapshot();
    auto redone = session.control_workspace_source(snapshot.project_generation, snapshot.project_revision);
    require(redone && redone.value().source.serialize() == after.value().source.serialize(), "redo restores exact result");
    require(!session.review_ik_fk_key("body", {7}, {30, 30}, snapshot.project_generation, snapshot.project_revision),
        "existing tick cannot be overwritten");
    auto later = session.review_ik_fk_key("body", {7}, {60, 30}, snapshot.project_generation, snapshot.project_revision);
    require(!later, "mode must still be FK before a supported switch");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::SaveProjectAction{project_path})),
        "save accepted keys");
    auto persisted = project::ProjectDocument::load(project_path);
    require(persisted && persisted.value().control_workspace(), "persisted workspace record");
    const auto& saved_workspace = *persisted.value().control_workspace();
    auto tampered = persisted.value().serialize();
    const std::string prefix = "CONTROL_WORKSPACE_SNAPSHOT " + std::to_string(saved_workspace.revision.value()) + " ";
    const auto position = tampered.find(prefix);
    require(position != std::string::npos, "find workspace envelope");
    tampered.replace(position, prefix.size(), "CONTROL_WORKSPACE_SNAPSHOT 99 ");
    require(!project::ProjectDocument::deserialize(tampered), "workspace revision tamper is detected");
    tampered = persisted.value().serialize();
    const auto line = tampered.find("CONTROL_WORKSPACE_SNAPSHOT ");
    const auto rig_name = tampered.find("\"body\"", line);
    require(line != std::string::npos && rig_name != std::string::npos, "find bound rig identity");
    tampered.replace(rig_name, std::string("\"body\"").size(), "\"b0dy\"");
    require(!project::ProjectDocument::deserialize(tampered), "workspace rig binding tamper is detected");
    application::ApplicationSession reopened;
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(reopened, application::OpenProjectAction{project_path})),
        "reopen accepted keys");
    const auto opened_snapshot = reopened.snapshot();
    auto reopened_source = reopened.control_workspace_source(opened_snapshot.project_generation, opened_snapshot.project_revision);
    require(reopened_source && reopened_source.value().source.serialize() == redone.value().source.serialize(),
        "project reopen retains authored keys");
    auto reopened_pose = reopened.evaluate_control_workspace_pose("body", {7}, {30, 30},
        opened_snapshot.project_generation, opened_snapshot.project_revision);
    require(reopened_pose && reopened_pose.value().mode == pose::IkFkMode::ik,
        "reopened authored keys evaluate through the same consumer");
    auto revised_config = configure;
    revised_config.expected_project_generation = opened_snapshot.project_generation;
    revised_config.expected_project_revision = opened_snapshot.project_revision;
    revised_config.configurations.front().max_position_jump *= 2.0;
    require(!application::HumanApplicationAccess::configure_autorig_ik_fk(reopened, revised_config) &&
        reopened.snapshot().project_revision == opened_snapshot.project_revision &&
        reopened.snapshot().undo_count == opened_snapshot.undo_count,
        "stale copied rig revision refuses without changing history");
    const auto reopened_rig = reopened.accepted_autorig("body",
        opened_snapshot.project_generation, opened_snapshot.project_revision);
    require(static_cast<bool>(reopened_rig), "read current reopened rig revision");
    revised_config.expected_record_revision = reopened_rig.value().record_revision;
    require(static_cast<bool>(application::HumanApplicationAccess::configure_autorig_ik_fk(reopened, revised_config)),
        "revise configured rig without deleting workspace");
    auto changed_rig = reopened.snapshot();
    auto historical_workspace = reopened.control_workspace_source(changed_rig.project_generation, changed_rig.project_revision);
    require(historical_workspace && historical_workspace.value().source.serialize() == redone.value().source.serialize(),
        "rig change preserves historical authored keys");
    require(!reopened.evaluate_control_workspace_pose("body", {7}, {30, 30},
        changed_rig.project_generation, changed_rig.project_revision), "changed rig is not silently rebound");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(reopened, application::UndoAction{})),
        "undo rig revision");
    changed_rig = reopened.snapshot();
    require(static_cast<bool>(reopened.evaluate_control_workspace_pose("body", {7}, {30, 30},
        changed_rig.project_generation, changed_rig.project_revision)), "restored rig evaluates preserved keys");
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::cout << "application IK/FK key acceptance tests passed\n";
}
