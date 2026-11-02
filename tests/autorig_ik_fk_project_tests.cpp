#include <carto/character/rig_record.hpp>
#include <carto/project/accepted_autorig.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace carto;
using namespace carto::character;

namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
AutorigConstraintProvenance human(std::string id) {
    return {std::move(id), "artist", "reviewed", "", AutorigConstraintOrigin::human_authored};
}
core::Transform rotation(double angle, double x = 0.0) {
    core::Transform result;
    result.translation.x = x;
    result.rotation = {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
    return result;
}
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("carto-autorig-ik-fk-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    project::ProjectDocument document;
    journal::Journal journal(path / "events.log");
    geometry::EditableMesh mesh;
    const auto root = mesh.add_vertex({0, 0, 0});
    const auto mid = mesh.add_vertex({1, 0, 0});
    const auto tip = mesh.add_vertex({2, 0, 0});
    const auto extension = mesh.add_vertex({3, 0, 0});
    const auto side = mesh.add_vertex({0, 1, 0});
    require(root && mid && tip && extension && side && mesh.add_face({root.value(), mid.value(), side.value()}) &&
        mesh.add_face({mid.value(), tip.value(), side.value()}) &&
        mesh.add_face({tip.value(), extension.value(), side.value()}), "source mesh");
    auto setup = project::ProjectTransaction::begin(document, journal, "artist");
    require(setup && setup.value().insert_mesh(51, mesh) && setup.value().commit("insert.mesh"), "publish source mesh");
    AutorigAnalysisRequest analysis_request{{51, hash("source")}, mesh.revision(), AutorigAxis::x, 0, 1e-7, {}, {}};
    const auto analysis = analyze_autorig_features(mesh, analysis_request);
    require(static_cast<bool>(analysis), "analyze source");
    AutorigConstraintSet constraints{analysis_request.source, mesh.revision(), analysis.value().analysis_digest,
        {{"root", "root", std::nullopt, AutorigPartSide::center},
         {"mid", "mid", std::string("root"), AutorigPartSide::center},
         {"tip", "tip", std::string("mid"), AutorigPartSide::center},
         {"extension", "extension", std::string("tip"), AutorigPartSide::center}},
        {{"a", "root", "root", AutorigPartSide::center, root.value(), human("root")},
         {"b", "mid", "mid", AutorigPartSide::center, mid.value(), human("mid")},
         {"c", "tip", "tip", AutorigPartSide::center, tip.value(), human("tip")},
         {"d", "extension", "extension", AutorigPartSide::center, extension.value(), human("extension")}}, {}, {}};
    auto opened = AutorigCandidateReview::begin(mesh, analysis_request, constraints);
    require(static_cast<bool>(opened), "open review");
    auto review = std::move(opened).value();
    const auto candidates = review.candidates();
    require(static_cast<bool>(review.select_candidate(candidates.ranked_candidates.front().digest)), "select candidate");
    const auto acceptance = review.accept_review(human("decision"));
    require(static_cast<bool>(acceptance), "accept candidate");
    std::vector<AutorigFinalNode> nodes;
    for (const auto& joint : candidates.ranked_candidates.front().joints) {
        AutorigFinalNode node;
        node.preview_joint = joint.id;
        node.durable_joint = {joint.part_id == "root" ? 1U : joint.part_id == "mid" ? 2U : joint.part_id == "tip" ? 3U : 4U};
        node.local_rest.translation = joint.parent ? core::Vec3d{1, 0, 0} : core::Vec3d{};
        node.kind = kinematic::JointKind::ball;
        node.node_class = kinematic::NodeClass::skinned;
        node.semantic_type = joint.semantic_role;
        nodes.push_back(node);
    }
    const auto graph = prepare_accepted_autorig_graph(mesh, analysis_request, candidates, acceptance.value(), nodes);
    require(static_cast<bool>(graph), "prepare reviewed graph");
    kinematic::JointLimitSet limits;
    RigDefinition rig;
    rig.id = {7};
    rig.kinematic_graph_digest = graph.value().graph_digest;
    rig.joint_limits.digest = limits.canonical_digest().value();
    for (std::uint64_t id = 1; id <= 12; ++id) {
        ControlDefinition control;
        control.id = {id};
        if (id <= 3) control.affected_joints.push_back({id});
        if (id >= 7 && id <= 9) control.affected_joints.push_back({id - 5});
        if (id == 6 || id == 12) {
            control.allowed_axes = kAxisTranslateX;
            control.axis_minimum[0] = 0;
            control.axis_maximum[0] = 1;
        }
        require(static_cast<bool>(rig.control_graph.insert(control)), "insert control");
    }
    kinematic::SkinBinding source;
    source.mesh_asset = 4;
    source.mesh_digest = hash("source-weights");
    source.graph_digest = graph.value().graph_digest;
    source.influences.emplace(0, std::vector<kinematic::JointInfluence>{{{1}, 1.0}});
    SkinWeightLayer source_layer;
    source_layer.layer_id = {1};
    source_layer.mesh_topology_digest = source.mesh_digest;
    source_layer.joint_graph_digest = source.graph_digest;
    source_layer.influenced_vertex_count = 1;
    source_layer.total_influence_count = 1;
    source_layer.provenance_digest = hash("authored");
    weights::WeightCorrespondence correspondence;
    for (std::uint32_t vertex = 0; vertex < 5; ++vertex)
        require(static_cast<bool>(correspondence.map(vertex, {{0, 1.0}})), "map target vertex");
    weights::WeightTransferOptions options;
    options.output_layer_id = {2};
    options.output_mesh_asset = analysis_request.source.asset_id;
    options.output_mesh_digest = analysis_request.source.content_digest;
    auto accept = project::ProjectTransaction::begin(document, journal, "artist");
    require(accept && accept.value().accept_autorig("body", document.source_namespace(), document.revision(), {}, analysis_request, candidates,
        acceptance.value(), nodes, rig, limits, source, source_layer, correspondence, {}, options) &&
        accept.value().commit("accept.autorig"), "persist accepted rig with transferred binding");
    const auto initial = document.accepted_autorigs().at("body");
    auto old = project::decode_accepted_autorig_bundle(initial.bytes);
    require(initial.bytes.starts_with("CARTO_ACCEPTED_AUTORIG_BUNDLE 1\n") && old && old.value().ik_fk.empty(),
        "v1 bundle decodes with no IK/FK configurations");

    pose::IkFkAuthoring config;
    config.rig = rig.id;
    config.rig_digest = rig_definition_digest(rig).value();
    config.graph_digest = rig.kinematic_graph_digest;
    config.limits_digest = rig.joint_limits.digest;
    config.chain = {{1}, {2}, {3}};
    config.fk_root = {1}; config.fk_mid = {2}; config.fk_effector = {3};
    config.ik_target = {4}; config.ik_pole = ControlId{5}; config.mode_control = {6};
    auto configure = project::ProjectTransaction::begin(document, journal, "artist");
    require(static_cast<bool>(configure), "begin configuration transaction");
    const auto revision = configure.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), initial.revision, {config});
    require(revision && revision.value() == core::Revision{2} &&
        document.accepted_autorigs().at("body") == initial, "configuration stages atomically");
    require(static_cast<bool>(configure.value().commit("configure.ik-fk")), "journal configuration");
    const auto current = document.accepted_autorigs().at("body");
    auto decoded = project::decode_accepted_autorig_bundle(current.bytes);
    require(current.bytes.starts_with("CARTO_ACCEPTED_AUTORIG_BUNDLE 2\n") && decoded &&
        decoded.value().ik_fk.size() == 1 && decoded.value().ik_fk.front().serialize() == config.serialize() &&
        decoded.value().weights.output.binding.serialize() == old.value().weights.output.binding.serialize(),
        "v2 retains authored configuration and full transferred binding");
    auto mislabeled_v36 = document.serialize();
    const auto mislabeled_v36_drafts = mislabeled_v36.find("UNADMITTED_MODE_KEY_DRAFTS 0\n");
    require(mislabeled_v36_drafts != std::string::npos, "fixture has the v38 draft table");
    mislabeled_v36.erase(mislabeled_v36_drafts, std::string("UNADMITTED_MODE_KEY_DRAFTS 0\n").size());
    require(mislabeled_v36.starts_with("CARTOGRAPHER_PROJECT 39\n"), "current v38 header");
    if (const auto control_workspace = mislabeled_v36.find("CONTROL_WORKSPACE 0\n"); control_workspace != std::string::npos)
        mislabeled_v36.erase(control_workspace, std::string("CONTROL_WORKSPACE 0\n").size());
    mislabeled_v36.replace(0U, std::string("CARTOGRAPHER_PROJECT 39\n").size(),
                           "CARTOGRAPHER_PROJECT 36\n");
    require(!project::ProjectDocument::deserialize(mislabeled_v36),
        "v36 refuses a v2 accepted-autorig bundle");
    auto invalid_v2 = current.bytes;
    const auto count_at = invalid_v2.find("IK_FK 1\n");
    require(count_at != std::string::npos, "v2 count fixture");
    invalid_v2.replace(count_at, std::string("IK_FK 1\n").size(), "IK_FK 0\n");
    require(!project::decode_accepted_autorig_bundle(invalid_v2), "empty v2 list refused");
    invalid_v2 = current.bytes;
    invalid_v2.replace(count_at, std::string("IK_FK 1\n").size(), "IK_FK 129\n");
    require(!project::decode_accepted_autorig_bundle(invalid_v2), "over-budget v2 list refused");
    require(!project::decode_accepted_autorig_bundle(current.bytes + "trailing"), "trailing v2 bytes refused");
    require(static_cast<bool>(project::validate_accepted_autorig_current(document, decoded.value())), "current bundle readiness");
    const auto historical_clear = project::prepare_historical_autorig_ik_fk_clear(decoded.value());
    require(historical_clear && historical_clear.value() == initial.bytes,
        "historical clear preserves all non-IK/FK canonical bytes");
    auto corrupt_historical = decoded.value();
    corrupt_historical.weights.output.receipt.output_binding_digest = hash("bad-binding");
    require(!project::prepare_historical_autorig_ik_fk_clear(corrupt_historical),
        "historical clear refuses internally corrupted binding provenance");
    auto reaccept = project::ProjectTransaction::begin(document, journal, "artist");
    require(static_cast<bool>(reaccept), "begin configured reacceptance");
    const auto before_reaccept = reaccept.value().staged_document().serialize();
    const project::ProjectDocument other_document;
    require(other_document.source_namespace() != document.source_namespace(), "different project namespace fixture");
    require(!reaccept.value().accept_autorig("body", other_document.source_namespace(), document.revision(), current.revision,
        analysis_request, candidates, acceptance.value(), nodes, rig, limits, source, source_layer, correspondence, {}, options) &&
        reaccept.value().staged_document().serialize() == before_reaccept,
        "matching numeric revisions cannot substitute a different project namespace");
    require(!reaccept.value().accept_autorig("body", document.source_namespace(), core::Revision{}, current.revision, analysis_request,
        candidates, acceptance.value(), nodes, rig, limits, source, source_layer, correspondence, {}, options) &&
        reaccept.value().staged_document().serialize() == before_reaccept,
        "matching row revision does not revive stale project authority for acceptance");
    const auto same = reaccept.value().accept_autorig("body", document.source_namespace(), document.revision(), current.revision, analysis_request,
        candidates, acceptance.value(), nodes, rig, limits, source, source_layer, correspondence, {}, options);
    require(same && same.value() == current.revision &&
        reaccept.value().staged_document().serialize() == before_reaccept,
        "identical reacceptance preserves authored IK/FK without a new revision");
    auto changed_options = options;
    changed_options.output_layer_id = {88};
    require(!reaccept.value().accept_autorig("body", document.source_namespace(), document.revision(), current.revision, analysis_request,
        candidates, acceptance.value(), nodes, rig, limits, source, source_layer, correspondence, {}, changed_options) &&
        reaccept.value().staged_document().serialize() == before_reaccept,
        "changed reacceptance refuses to discard authored IK/FK");
    require(!reaccept.value().accept_autorig("body", document.source_namespace(), document.revision(), initial.revision, analysis_request,
        candidates, acceptance.value(), nodes, rig, limits, source, source_layer, correspondence, {}, options),
        "configured reacceptance retains CAS refusal");
    require(static_cast<bool>(reaccept.value().rollback()), "rollback reacceptance checks");
    const auto observed_project_revision = document.revision();
    auto older = project::ProjectTransaction::begin(document, journal, "artist");
    auto newer = project::ProjectTransaction::begin(document, journal, "artist");
    require(older && newer && newer.value().insert_mesh(52, mesh) && newer.value().commit("insert.other.mesh"),
        "interleaving fixture publishes a newer project revision");
    const auto older_before = older.value().staged_document().serialize();
    require(!older.value().accept_autorig("body", document.source_namespace(), observed_project_revision,
        current.revision, analysis_request, candidates, acceptance.value(), nodes, rig, limits,
        source, source_layer, correspondence, {}, options) &&
        older.value().staged_document().serialize() == older_before,
        "interleaved identical acceptance refuses stale live project without staged mutation");
    require(!older.value().configure_autorig_ik_fk("body", document.source_namespace(),
        observed_project_revision, current.revision, {config}) &&
        older.value().staged_document().serialize() == older_before,
        "interleaved identical configuration refuses stale live project without staged mutation");
    require(static_cast<bool>(older.value().rollback()), "rollback interleaved transaction");
    const auto decoded_graph = kinematic::KinematicGraph::deserialize(decoded.value().reviewed.graph_bytes);
    require(static_cast<bool>(decoded_graph), "decode stored graph");
    pose::LocalPose fk;
    fk[{1}] = rotation(0.4);
    fk[{2}] = rotation(-0.9, 1.0);
    const auto& restored_config = decoded.value().ik_fk.front();
    auto to_ik = pose::propose_ik_fk_switch(restored_config, decoded.value().rig, decoded_graph.value(),
        decoded.value().limits, pose::IkFkMode::ik, fk);
    require(to_ik && to_ik.value().mode_value == config.ik_mode_value &&
        to_ik.value().match.position_jump <= config.max_position_jump, "stored FK-to-IK proposal");
    auto to_fk = pose::propose_ik_fk_switch(restored_config, decoded.value().rig, decoded_graph.value(),
        decoded.value().limits, pose::IkFkMode::fk, {}, to_ik.value().match.ik);
    require(to_fk && to_fk.value().mode_value == config.fk_mode_value &&
        to_fk.value().match.position_jump <= config.max_position_jump, "stored IK-to-FK proposal");

    auto rejected = project::ProjectTransaction::begin(document, journal, "artist");
    require(static_cast<bool>(rejected), "begin rejection checks");
    const auto before_refusals = rejected.value().staged_document().serialize();
    require(!rejected.value().configure_autorig_ik_fk("body", other_document.source_namespace(), document.revision(), current.revision, {config}),
        "configuration refuses a different project namespace with matching numeric revisions");
    require(!rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), core::Revision{}, current.revision, {config}),
        "caller project revision pin is required even when the record revision matches");
    require(!rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), initial.revision, {config}), "stale CAS refused");
    auto bad = config; bad.rig_digest = hash("different rig");
    require(!rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), current.revision, {bad}), "wrong rig pin refused");
    bad = config; bad.ik_target = {99};
    require(!rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), current.revision, {bad}), "dangling control refused");
    require(!rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), current.revision, {config, config}), "duplicate chain refused");
    require(!rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), current.revision, {}),
        "ordinary configuration cannot silently clear authored chains");
    auto second = config;
    second.chain = {{2}, {3}, {4}};
    second.fk_root = {7}; second.fk_mid = {8}; second.fk_effector = {9};
    second.ik_target = {10}; second.ik_pole = ControlId{11};
    require(static_cast<bool>(second.validate_against(rig, decoded_graph.value(), limits)),
        "second chain is individually valid with an intentionally shared mode control");
    const auto shared = rejected.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), current.revision, {config, second});
    require(!shared && shared.error().message.find("cannot share controls") != std::string::npos,
        "individually valid distinct chains cannot share controls without arbitration");
    second.mode_control = {12};
    require(static_cast<bool>(second.validate_against(rig, decoded_graph.value(), limits)) &&
        static_cast<bool>(project::prepare_autorig_ik_fk_configuration(document, decoded.value(), {config, second})),
        "distinct chains with disjoint controls are admitted");
    auto two_chains = decoded.value();
    two_chains.ik_fk = {config, second};
    require(!project::prepare_autorig_ik_fk_configuration(document, two_chains, {config}),
        "ordinary configuration cannot silently drop a chain");
    require(rejected.value().staged_document().serialize() == before_refusals &&
        document.accepted_autorigs().at("body") == current, "refusals leave staged and published state unchanged");
    require(static_cast<bool>(rejected.value().rollback()), "roll back refusal checks");

    // This checks the early repeated-validation work fence. The repeated
    // records would also be duplicates, so verify the explicit budget error;
    // a single individually valid configuration on this same rig is admitted.
    auto large_bundle = decoded.value();
    large_bundle.ik_fk.clear();
    for (std::uint64_t id = 13; id <= ControlGraph::kMaxControls; ++id) {
        ControlDefinition control;
        control.id = {id};
        control.control_set = std::string(ControlGraph::kMaxControlSetBytes, 'x');
        require(static_cast<bool>(large_bundle.rig.control_graph.insert(control)), "insert bounded work fixture control");
    }
    const auto large_rig_bytes = serialize_rig_definition(large_bundle.rig);
    require(large_rig_bytes && large_rig_bytes.value().size() >
        project::AcceptedAutorigBundle::kMaxIkFkValidationBytes / project::AcceptedAutorigBundle::kMaxIkFkChains,
        "fixture reaches the repeated-validation byte budget");
    auto large_config = config;
    large_config.rig_digest = rig_definition_digest(large_bundle.rig).value();
    require(static_cast<bool>(large_config.validate_against(large_bundle.rig, decoded_graph.value(), large_bundle.limits)) &&
        static_cast<bool>(project::prepare_autorig_ik_fk_configuration(document, large_bundle, {large_config})),
        "one configuration on the large rig remains valid");
    const std::vector<pose::IkFkAuthoring> repeated(project::AcceptedAutorigBundle::kMaxIkFkChains, large_config);
    const auto over_budget = project::prepare_autorig_ik_fk_configuration(document, large_bundle, repeated);
    require(!over_budget && over_budget.error().message.find("validation work exceeds") != std::string::npos &&
        document.accepted_autorigs().at("body") == current, "validation work budget refuses without mutation");
    const auto saved = document.save_atomic(path / "project.carto");
    const auto reopened = project::ProjectDocument::load(path / "project.carto");
    require(saved && reopened && reopened.value().accepted_autorigs().at("body") == current,
        "v2 survives save and reopen");
    const auto reopened_bundle = project::decode_accepted_autorig_bundle(reopened.value().accepted_autorigs().at("body").bytes);
    require(reopened_bundle && reopened_bundle.value().ik_fk.size() == 1 &&
        reopened_bundle.value().ik_fk.front().serialize() == config.serialize(), "reopened configuration is exact");
    const auto reopened_graph = kinematic::KinematicGraph::deserialize(reopened_bundle.value().reviewed.graph_bytes);
    require(static_cast<bool>(reopened_graph) &&
        static_cast<bool>(project::validate_accepted_autorig_current(reopened.value(), reopened_bundle.value())),
        "reopened graph and current mesh are ready");
    const auto reopened_ik = pose::propose_ik_fk_switch(reopened_bundle.value().ik_fk.front(),
        reopened_bundle.value().rig, reopened_graph.value(), reopened_bundle.value().limits, pose::IkFkMode::ik, fk);
    require(static_cast<bool>(reopened_ik), "reopened FK-to-IK proposal");
    const auto reopened_fk = pose::propose_ik_fk_switch(reopened_bundle.value().ik_fk.front(),
        reopened_bundle.value().rig, reopened_graph.value(), reopened_bundle.value().limits,
        pose::IkFkMode::fk, {}, reopened_ik.value().match.ik);
    require(reopened_fk && reopened_fk.value().match.position_jump <= config.max_position_jump,
        "reopened IK-to-FK proposal");
    auto changed_mesh = mesh;
    require(static_cast<bool>(changed_mesh.set_vertex_position(side.value(), {0, 1.2, 0})), "change source vertex");
    auto change = project::ProjectTransaction::begin(document, journal, "artist");
    require(change && change.value().replace_mesh(51, changed_mesh) && change.value().commit("change.mesh"),
        "publish changed source mesh");
    auto stale_mesh = project::ProjectTransaction::begin(document, journal, "artist");
    const auto removed_snapshot = document.serialize();
    require(stale_mesh && !stale_mesh.value().configure_autorig_ik_fk("body", document.source_namespace(), document.revision(), current.revision, {config}) &&
        stale_mesh.value().staged_document().serialize() == removed_snapshot && document.serialize() == removed_snapshot,
        "changed source mesh refuses configuration without mutation");
    require(static_cast<bool>(stale_mesh.value().rollback()), "rollback stale source check");
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    std::cout << "autorig IK/FK project tests passed\n";
}
