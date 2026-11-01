#include <carto/application/application.hpp>
#include <carto/character/rig_record.hpp>
#include <carto/journal/journal.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>

using namespace carto;
namespace {
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
    auto child = mesh.add_vertex({0, 2, 0});
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
    request.analysis = {{mesh_id, hash("caller source")}, mesh.revision(), character::AutorigAxis::x, 0, 1e-7, {}, {}};
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
        node.local_rest.translation = joint.parent ? core::Vec3d{0, 1, 0} : core::Vec3d{};
        node.kind = kinematic::JointKind::ball;
        node.node_class = kinematic::NodeClass::skinned; node.semantic_type = joint.semantic_role;
        request.nodes.push_back(node);
    }
    auto graph = character::prepare_accepted_autorig_graph(
        mesh, request.analysis, request.candidates, request.acceptance, request.nodes);
    require(static_cast<bool>(graph), "prepare graph");
    request.rig.id = character::RigId{7};
    request.rig.kinematic_graph_digest = graph.value().graph_digest;
    request.rig.joint_limits.digest = request.limits.canonical_digest().value();
    for (std::uint64_t i = 1; i <= 6; ++i) {
        character::ControlDefinition control;
        control.id = {i};
        if (i <= 3) control.affected_joints.push_back(request.nodes[i - 1].durable_joint);
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
    const auto journal_path = std::filesystem::path(project_path.string() + ".journal");
    const auto journal_backup = path / "saved-journal";
    std::error_code file_error;
    std::filesystem::rename(journal_path, journal_backup, file_error);
    require(!file_error && std::filesystem::create_directory(journal_path), "block test journal path");
    const auto before_failure = session.snapshot();
    require(!application::HumanApplicationAccess::commit_autorig(session, request), "journal failure refuses acceptance");
    const auto after_failure = session.snapshot();
    require(after_failure.project_revision == before_failure.project_revision &&
        after_failure.undo_count == before_failure.undo_count && after_failure.redo_count == before_failure.redo_count &&
        !session.accepted_autorig("body", after_failure.project_generation, after_failure.project_revision),
        "journal failure restores project and history");
    require(std::filesystem::remove(journal_path), "remove blocked test journal path");
    std::filesystem::rename(journal_backup, journal_path, file_error);
    require(!file_error, "restore test journal");
    auto accepted = application::HumanApplicationAccess::commit_autorig(session, request);
    require(accepted && accepted.value().document_changed, "accept as application command");
    snapshot = session.snapshot();
    const auto accepted_undo_count = snapshot.undo_count;
    auto loaded = session.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision);
    require(loaded && loaded.value().record_revision == core::Revision{1} &&
        loaded.value().bundle.weights.output.binding.influences.size() == 4, "read current committed binding and revision");
    require(!session.accepted_autorig(std::string(129, 'x'), snapshot.project_generation,
        snapshot.project_revision), "oversized identity refused at read boundary");
    auto unchanged = request;
    unchanged.expected_project_revision = snapshot.project_revision;
    unchanged.expected_record_revision = loaded.value().record_revision;
    auto no_op = application::HumanApplicationAccess::commit_autorig(session, unchanged);
    require(no_op && !no_op.value().document_changed && session.snapshot().undo_count == accepted_undo_count,
        "identical acceptance does not create history");
    require(!application::HumanApplicationAccess::commit_autorig(session, request) &&
        session.snapshot().undo_count == accepted_undo_count, "stale project refused without changing history");
    auto stale_record = unchanged;
    stale_record.expected_record_revision = {};
    require(!application::HumanApplicationAccess::commit_autorig(session, stale_record) &&
        session.snapshot().undo_count == accepted_undo_count, "stale acceptance CAS leaves history unchanged");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})), "undo acceptance");
    snapshot = session.snapshot();
    require(!session.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision), "undo removes acceptance");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::RedoAction{})), "redo acceptance");
    snapshot = session.snapshot();
    require(static_cast<bool>(session.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision)),
        "redo restores acceptance");
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
    const auto undo_count = snapshot.undo_count;
    auto stale_configure = configure;
    stale_configure.expected_project_revision = snapshot.project_revision;
    require(!application::HumanApplicationAccess::configure_autorig_ik_fk(session, stale_configure) &&
        session.snapshot().undo_count == undo_count, "stale configuration CAS leaves history unchanged");
    auto invalid_configure = configure;
    invalid_configure.expected_project_revision = snapshot.project_revision;
    invalid_configure.expected_record_revision = core::Revision{2};
    invalid_configure.configurations[0].mode_control = {99};
    require(!application::HumanApplicationAccess::configure_autorig_ik_fk(session, invalid_configure) &&
        session.snapshot().undo_count == undo_count, "invalid configuration leaves history unchanged");
    std::filesystem::rename(journal_path, journal_backup, file_error);
    require(!file_error && std::filesystem::create_directory(journal_path), "block journal before undo");
    const auto before_failed_undo = session.snapshot();
    require(!application::HumanApplicationAccess::dispatch(session, application::UndoAction{}),
        "journal failure refuses undo");
    const auto after_failed_undo = session.snapshot();
    auto still_configured = session.accepted_autorig("body", after_failed_undo.project_generation,
        after_failed_undo.project_revision);
    require(after_failed_undo.project_revision == before_failed_undo.project_revision &&
        after_failed_undo.undo_count == before_failed_undo.undo_count &&
        after_failed_undo.redo_count == before_failed_undo.redo_count &&
        still_configured && still_configured.value().bundle.ik_fk.size() == 1,
        "failed undo restores configuration and history");
    require(std::filesystem::remove(journal_path), "remove blocked undo journal path");
    std::filesystem::rename(journal_backup, journal_path, file_error);
    require(!file_error, "restore journal after failed undo");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})),
        "undo IK/FK configuration");
    snapshot = session.snapshot();
    auto unconfigured = session.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision);
    require(unconfigured && unconfigured.value().record_revision == core::Revision{1} &&
        unconfigured.value().bundle.ik_fk.empty(),
        "undo preserves accepted bundle and removes IK/FK configuration");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::RedoAction{})),
        "redo IK/FK configuration");
    snapshot = session.snapshot();
    pose::LocalPose bent;
    core::Transform root_pose;
    root_pose.rotation = {0, 0, std::sin(0.2), std::cos(0.2)};
    core::Transform mid_pose;
    mid_pose.translation = {0, 1, 0};
    mid_pose.rotation = {0, 0, std::sin(-0.45), std::cos(-0.45)};
    bent.emplace(request.nodes[0].durable_joint, root_pose);
    bent.emplace(request.nodes[1].durable_joint, mid_pose);
    auto switch_proposal = session.propose_autorig_ik_fk_switch("body", snapshot.project_generation,
        snapshot.project_revision, 0, pose::IkFkMode::ik, bent);
    require(switch_proposal && switch_proposal.value().mode_control == character::ControlId{6} &&
        switch_proposal.value().mode_value == 1.0, "current rig supplies a non-mutating IK/FK switch proposal");
    auto fk_proposal = session.propose_autorig_ik_fk_switch("body", snapshot.project_generation,
        snapshot.project_revision, 0, pose::IkFkMode::fk, {}, switch_proposal.value().match.ik);
    require(fk_proposal && fk_proposal.value().mode_value == 0.0, "IK-to-FK proposal succeeds");
    require(session.snapshot().project_revision == snapshot.project_revision, "switch proposal does not mutate project");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(
        session, application::SaveProjectAction{project_path})), "save committed acceptance");
    application::ApplicationSession reopened;
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(
        reopened, application::OpenProjectAction{project_path})), "reopen committed acceptance");
    snapshot = reopened.snapshot();
    auto reopened_bundle = reopened.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision);
    require(reopened_bundle && reopened_bundle.value().record_revision == core::Revision{2} &&
        reopened_bundle.value().bundle.ik_fk.size() == 1,
        "reopened bundle is available and current");
    journal::Journal journal(std::filesystem::path(project_path.string() + ".journal"));
    require(static_cast<bool>(journal.verify()), "application journal verifies");
    const auto object = snapshot.objects.front().object.id;
    require(application::HumanApplicationAccess::dispatch(reopened,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}) &&
        application::HumanApplicationAccess::dispatch(reopened,
            application::SelectVertexAction{object, root.value()}), "select source vertex");
    editor::ToolArguments edit;
    edit.position = {0.1, 0, 0};
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(reopened,
        application::InvokeToolAction{"mesh.set-vertex-position", edit})), "edit source mesh");
    snapshot = reopened.snapshot();
    require(!reopened.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision),
        "changed source refuses current bundle read");
    require(!reopened.propose_autorig_ik_fk_switch("body", snapshot.project_generation,
        snapshot.project_revision, 0, pose::IkFkMode::ik, {}), "changed source refuses switch proposal");
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}
