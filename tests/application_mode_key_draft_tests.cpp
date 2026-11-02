#include <carto/application/application.hpp>
#include <carto/character/rig_record.hpp>
#include <carto/journal/journal.hpp>
#include <carto/pose/ik_fk_mode_key_record.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

using namespace carto;
static_assert(!std::is_same_v<application::HistoricalAutorigView, application::AcceptedAutorigView>);
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
    application::StoreUnadmittedModeKeyDraftRequest draft;
    draft.expected_project_generation = snapshot.project_generation;
    draft.expected_source_namespace = snapshot.source_namespace;
    draft.expected_project_revision = snapshot.project_revision;
    draft.rig_identity = "body";
    draft.expected_rig_record_revision = configured.value().record_revision;
    draft.expected_rig_record_digest = rig_digest;
    draft.draft_identity = "mode-draft-1";
    draft.acknowledge_unadmitted_assertions = true;
    draft.justification = "review this asserted future mode key";
    draft.channel.id = {7}; draft.channel.channel_name = "mode-reference";
    draft.channel.target = {temporal::TargetBindingKind::skinned_mesh, "character.body"};
    draft.channel.duration = 10; draft.channel.scalar_keyframes = {{0, 0}};
    draft.clip.id = {8}; draft.clip.clip_name = "clip"; draft.clip.duration = 10;
    draft.clip.channels = {{draft.channel.id, draft.channel.canonical_digest().value()}};
    draft.document.id = {9}; draft.document.timebase_ticks_per_second = 30;
    draft.document.channels = {{draft.channel.id, draft.channel.canonical_digest().value()}};
    draft.document.clips = {{draft.clip.id, draft.clip.canonical_digest().value()}};
    draft.document.targets = {{{}, {6}}};
    draft.layer.id = {10}; draft.layer.kind = motion::LayerKind::base;
    draft.layer.window_start = {0, 30}; draft.layer.window_end = {120, 30};
    motion::LayerCurve curve; curve.target.control = {6}; curve.curve.id = {12};
    curve.curve.timebase_ticks_per_second = 30; curve.curve.interpolation = motion::InterpolationKind::step;
    curve.curve.keys = {{{0, 30}, 0, 0, 0}};
    draft.layer.curves.push_back(curve);
    draft.crosswalk = {draft.channel.target, draft.channel.id, draft.channel.canonical_digest().value(),
        draft.layer.id, curve.curve.id, curve.curve.canonical_digest().value(), {6}, 0};
    auto& context = draft.context;
    context.source_namespace = "caller.motion.source";
    context.source_revision = core::Revision{3}; context.source_digest = hash("asserted source");
    context.motion_document_digest = hash("asserted motion document");
    context.rig = authored.rig; context.rig_digest = authored.rig_digest;
    context.rest_graph_digest = authored.graph_digest; context.limits_digest = authored.limits_digest;
    context.source_pose_digest = hash("asserted pose");
    context.source_pose_assertion = pose::SourcePoseAssertion::predicted_alternative;
    context.authoring_digest = authored.canonical_digest().value();
    context.reference_frame = "caller asserted graph-world frame";
    context.solver_input_frame = pose::SolverInputFrameAssertion::graph_world;
    context.lineage = {"scenario", "branch", "session", "restore"};
    context.clip_tick = {30, 30}; context.clip_start = {0, 30}; context.clip_end = {120, 30};
    context.source_tick = {48000, 48000}; context.source_start = {0, 48000}; context.source_end = {96000, 48000};
    context.channel_tick = {60, 60}; context.channel_start = {0, 60}; context.channel_end = {240, 60};
    draft.source_mode = pose::IkFkMode::fk; draft.target_mode = pose::IkFkMode::ik;
    core::Transform root_pose; root_pose.rotation = {0, 0, std::sin(0.2), std::cos(0.2)};
    core::Transform mid_pose; mid_pose.translation = {0, 1, 0};
    mid_pose.rotation = {0, 0, std::sin(-0.45), std::cos(-0.45)};
    draft.source_locals.emplace(request.nodes[0].durable_joint, root_pose);
    draft.source_locals.emplace(request.nodes[1].durable_joint, mid_pose);
    auto proposal = pose::propose_ik_fk_mode_key_intent(draft.document, draft.clip, draft.channel,
        draft.layer, context, draft.crosswalk, authored, request.rig, graph.value().graph, request.limits,
        draft.source_mode, draft.target_mode, draft.source_locals, draft.source_ik);
    require(static_cast<bool>(proposal), "make caller-side proposal");
    auto record = pose::make_ik_fk_mode_key_record(proposal.value());
    require(static_cast<bool>(record), "encode expected record");
    draft.expected_record = record.value();
    const auto original_record = record.value().serialize();
    const auto original_curve = draft.layer.curves.front().curve.serialize();
    const auto original_channel = draft.channel.serialize();
    const auto original_clip = draft.clip.serialize();
    const auto store = [&](const application::StoreUnadmittedModeKeyDraftRequest& input) {
        return application::HumanApplicationAccess::store_unadmitted_mode_key_draft(session, input);
    };
    const auto read = [&]() {
        const auto state = session.snapshot();
        return session.unadmitted_mode_key_draft(draft.draft_identity, state.project_generation,
            state.source_namespace, state.project_revision);
    };
    const auto assert_refused = [&](const application::StoreUnadmittedModeKeyDraftRequest& input,
                                    const char* message) {
        const auto before = session.snapshot();
        require(!store(input), message);
        const auto after = session.snapshot();
        require(before.project_revision == after.project_revision && before.undo_count == after.undo_count &&
            before.redo_count == after.redo_count && !read(), "refusal leaves project, history and draft unchanged");
    };
    auto bad = draft; bad.acknowledge_unadmitted_assertions = false;
    assert_refused(bad, "unadmitted assertion acknowledgment required");
    bad = draft; bad.justification = "   "; assert_refused(bad, "blank rationale refused");
    bad = draft; bad.justification = std::string(5000, 'x'); assert_refused(bad, "unbounded rationale refused");
    bad = draft; ++bad.expected_project_generation; assert_refused(bad, "stale generation refused");
    bad = draft; bad.expected_source_namespace = "other-project"; assert_refused(bad, "wrong project namespace refused");
    bad = draft; bad.expected_project_revision = core::Revision{}; assert_refused(bad, "stale project revision refused");
    bad = draft; bad.expected_rig_record_revision = core::Revision{1}; assert_refused(bad, "stale rig revision refused");
    bad = draft; bad.expected_rig_record_digest = hash("wrong rig row"); assert_refused(bad, "wrong rig row digest refused");
    bad = draft; bad.context.authoring_digest = hash("noncurrent configuration");
    assert_refused(bad, "noncurrent authoring configuration refused");
    bad = draft; bad.expected_record.context.source_pose_digest = hash("forged proposal record");
    assert_refused(bad, "caller record must match recomputed proposal");
    bad = draft; bad.expected_draft_revision = core::Revision{1}; assert_refused(bad, "draft insert CAS refused");
    application::ApplicationSession unsaved;
    const auto unsaved_result = application::HumanApplicationAccess::store_unadmitted_mode_key_draft(unsaved, draft);
    require(!unsaved_result && unsaved_result.error().message.find("durable journal") != std::string::npos,
        "unsaved session cannot commit durable draft");
    const auto journal_path = std::filesystem::path(project_path.string() + ".journal");
    const auto journal_backup = path / "saved-journal";
    std::error_code file_error;
    std::filesystem::rename(journal_path, journal_backup, file_error);
    require(!file_error && std::filesystem::create_directory(journal_path), "block journal");
    const auto before_blocked = session.snapshot();
    const auto blocked = store(draft);
    const auto after_blocked = session.snapshot();
    std::cerr << "DIAGNOSTIC blocked=" << static_cast<bool>(blocked) << " message=" << (blocked ? "success" : blocked.error().message)
        << " before_revision=" << before_blocked.project_revision.value() << " after_revision=" << after_blocked.project_revision.value()
        << " before_undo=" << before_blocked.undo_count << " after_undo=" << after_blocked.undo_count
        << " before_redo=" << before_blocked.redo_count << " after_redo=" << after_blocked.redo_count << " draft_present=" << static_cast<bool>(read()) << "\n";
    require(!blocked && blocked.error().message.find("rolled back") != std::string::npos &&
        before_blocked.project_revision == after_blocked.project_revision &&
        before_blocked.undo_count == after_blocked.undo_count &&
        before_blocked.redo_count == after_blocked.redo_count && !read(),
        "journal append failure rolls back draft and history");
    require(std::filesystem::remove(journal_path), "remove blocked journal");
    std::filesystem::rename(journal_backup, journal_path, file_error);
    require(!file_error, "restore journal");
    const auto before_insert = session.snapshot();
    require(static_cast<bool>(store(draft)), "store explicit unadmitted draft");
    snapshot = session.snapshot();
    auto result = read();
    require(result && result.value().record_revision == core::Revision{1} &&
        result.value().checked_project_namespace == draft.expected_source_namespace &&
        result.value().checked_project_revision == draft.expected_project_revision &&
        result.value().checked_rig_identity == "body" &&
        result.value().checked_rig_revision == configured.value().record_revision &&
        result.value().checked_rig_digest == rig_digest &&
        !result.value().envelope_digest.is_zero() &&
        result.value().record.serialize() == original_record &&
        result.value().record.context.source_pose_assertion == pose::SourcePoseAssertion::predicted_alternative &&
        result.value().record.context.source_namespace == "caller.motion.source" &&
        result.value().record.context.reference_frame == "caller asserted graph-world frame" &&
        result.value().record.context.lineage.restore == std::optional<std::string>{"restore"} &&
        result.value().record.context.clip_tick.tick == 30 &&
        result.value().record.context.source_tick.tick == 48000 &&
        result.value().record.context.channel_tick.tick == 60 &&
        result.value().digest == record.value().canonical_digest().value(),
        "typed unadmitted read preserves exact caller assertions and clocks");
    require(snapshot.undo_count == before_insert.undo_count + 1U &&
        draft.layer.curves.front().curve.serialize() == original_curve && draft.channel.serialize() == original_channel &&
        draft.clip.serialize() == original_clip, "draft adds history without mutating input motion");
    journal::Journal journal(journal_path);
    const auto entries = journal.read_all();
    require(entries && !entries.value().empty(), "read draft journal entry");
    const auto& payload = entries.value().back().payload;
    const std::string payload_text(payload.begin(), payload.end());
    constexpr char digits[] = "0123456789abcdef";
    std::string reason_hex;
    for (const unsigned char byte : draft.justification) {
        reason_hex.push_back(digits[byte >> 4U]); reason_hex.push_back(digits[byte & 15U]);
    }
    require(payload_text.find("operation=Store Unadmitted Mode-Key Draft acknowledge_unadmitted_assertions=1 justification_hex=" +
            reason_hex) != std::string::npos &&
        payload_text.find(" rig_revision=2 rig_digest=" + rig_digest.hex() +
            " draft_digest=" + record.value().canonical_digest().value().hex()) != std::string::npos,
        "journal binds explicit unadmitted acknowledgment, rationale and row/draft digests");
    auto still_rig = session.accepted_autorig("body", snapshot.project_generation, snapshot.project_revision);
    require(still_rig && still_rig.value().record_revision == configured.value().record_revision &&
        still_rig.value().bundle.ik_fk.front().serialize() == authored.serialize(),
        "draft does not mutate rig or mode authoring");
    bad = draft; bad.expected_project_generation = snapshot.project_generation;
    bad.expected_project_revision = snapshot.project_revision; bad.expected_draft_revision = {};
    const auto duplicate_before = session.snapshot();
    require(!store(bad) && session.snapshot().project_revision == duplicate_before.project_revision &&
        session.snapshot().undo_count == duplicate_before.undo_count && read().value().record.serialize() == original_record,
        "duplicate identity cannot replace draft");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})), "undo draft");
    require(!read(), "undo removes authoring draft");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::RedoAction{})), "redo draft");
    require(read() && read().value().record.serialize() == original_record, "redo restores same asserted draft");
    // Undo permits identity/revision reuse. A different asserted source gives
    // a different digest; an old project-state pin cannot address the new row.
    const auto old_pin = session.snapshot();
    const auto old_digest = read().value().digest;
    const auto old_envelope_digest = read().value().envelope_digest;
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})), "undo before identity reuse");
    auto alternative = draft;
    auto reuse_state = session.snapshot();
    alternative.expected_project_generation = reuse_state.project_generation;
    alternative.expected_source_namespace = reuse_state.source_namespace;
    alternative.expected_project_revision = reuse_state.project_revision;
    alternative.context.motion_document_digest = hash("different caller asserted document");
    auto alternative_proposal = pose::propose_ik_fk_mode_key_intent(alternative.document, alternative.clip,
        alternative.channel, alternative.layer, alternative.context, alternative.crosswalk, authored,
        request.rig, graph.value().graph, request.limits, alternative.source_mode, alternative.target_mode,
        alternative.source_locals, alternative.source_ik);
    require(static_cast<bool>(alternative_proposal), "make alternative asserted proposal");
    auto alternative_record = pose::make_ik_fk_mode_key_record(alternative_proposal.value());
    require(static_cast<bool>(alternative_record), "make alternative record");
    alternative.expected_record = alternative_record.value();
    require(static_cast<bool>(store(alternative)), "reuse draft identity after undo");
    auto reused = read();
    require(reused && reused.value().record_revision == core::Revision{1} && reused.value().digest != old_digest &&
        reused.value().envelope_digest != old_envelope_digest &&
        !session.unadmitted_mode_key_draft(draft.draft_identity, old_pin.project_generation,
            old_pin.source_namespace, old_pin.project_revision),
        "reused row requires fresh project pin and distinct digest");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::UndoAction{})), "undo alternative draft");
    reuse_state = session.snapshot();
    draft.expected_project_generation = reuse_state.project_generation;
    draft.expected_source_namespace = reuse_state.source_namespace;
    draft.expected_project_revision = reuse_state.project_revision;
    require(static_cast<bool>(store(draft)), "restore original as a fresh draft insertion");
    const auto restored_envelope_digest = read().value().envelope_digest;
    require(!restored_envelope_digest.is_zero() && restored_envelope_digest != old_envelope_digest &&
        read().value().digest == old_digest,
        "same record reinserted at a new project revision has a distinct envelope digest");
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(session, application::SaveProjectAction{project_path})),
        "save draft");
    application::ApplicationSession reopened;
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(reopened, application::OpenProjectAction{project_path})),
        "reopen draft");
    auto reopened_state = reopened.snapshot();
    auto reopened_draft = reopened.unadmitted_mode_key_draft(draft.draft_identity, reopened_state.project_generation,
        reopened_state.source_namespace, reopened_state.project_revision);
    require(reopened_draft && reopened_draft.value().record.serialize() == original_record &&
        reopened_draft.value().checked_project_namespace == draft.expected_source_namespace &&
        reopened_draft.value().checked_project_revision == draft.expected_project_revision &&
        reopened_draft.value().checked_rig_identity == "body" &&
        reopened_draft.value().checked_rig_revision == configured.value().record_revision &&
        reopened_draft.value().checked_rig_digest == rig_digest &&
        reopened_draft.value().envelope_digest == restored_envelope_digest,
        "reopened unadmitted draft preserves assertions");
    // Change one field of the valid serialized row at a time, without
    // recomputing either digest. All replacement values are syntactically
    // valid so cold-load integrity, not just token parsing, is exercised.
    auto persisted = project::ProjectDocument::load(project_path);
    require(static_cast<bool>(persisted), "load populated v38 draft for tamper tests");
    const auto& entry = persisted.value().unadmitted_mode_key_drafts().at(draft.draft_identity);
    const auto row = [&](const std::string& identity, const std::string& checked_namespace,
                         core::Revision checked_revision, const std::string& rig_identity,
                         core::Revision rig_revision, const assets::Sha256Digest& checked_rig_digest,
                         const assets::Sha256Digest& envelope_digest, const std::string& bytes) {
        std::ostringstream out;
        out << "UNADMITTED_MODE_KEY_DRAFT " << std::quoted(identity) << ' ' << entry.revision.value()
            << ' ' << entry.digest.hex() << ' ' << std::quoted(checked_namespace) << ' '
            << checked_revision.value() << ' ' << std::quoted(rig_identity) << ' ' << rig_revision.value()
            << ' ' << checked_rig_digest.hex() << ' ' << envelope_digest.hex() << ' ' << std::quoted(bytes) << '\n';
        return out.str();
    };
    const auto original_row = row(draft.draft_identity, entry.checked_project_namespace,
        entry.checked_project_revision, entry.checked_rig_identity, entry.checked_rig_revision,
        entry.checked_rig_digest, entry.envelope_digest, entry.bytes);
    const auto serialized = persisted.value().serialize();
    const auto row_position = serialized.find(original_row);
    require(row_position != std::string::npos && serialized.find(original_row, row_position + 1U) == std::string::npos &&
        static_cast<bool>(project::ProjectDocument::deserialize(serialized)), "valid populated v38 row round trips");
    const auto reject_tamper = [&](const std::string& replacement, const char* message) {
        auto modified = serialized;
        modified.replace(row_position, original_row.size(), replacement);
        require(!project::ProjectDocument::deserialize(modified), message);
    };
    const auto changed_project_revision = core::Revision{entry.checked_project_revision.value() + 1U};
    const auto changed_rig_revision = core::Revision{entry.checked_rig_revision.value() + 1U};
    reject_tamper(row("another-mode-draft", entry.checked_project_namespace, entry.checked_project_revision,
        entry.checked_rig_identity, entry.checked_rig_revision, entry.checked_rig_digest, entry.envelope_digest, entry.bytes),
        "draft identity tamper rejected");
    reject_tamper(row(draft.draft_identity, "another-project", entry.checked_project_revision,
        entry.checked_rig_identity, entry.checked_rig_revision, entry.checked_rig_digest, entry.envelope_digest, entry.bytes),
        "checked project namespace tamper rejected");
    reject_tamper(row(draft.draft_identity, entry.checked_project_namespace, changed_project_revision,
        entry.checked_rig_identity, entry.checked_rig_revision, entry.checked_rig_digest, entry.envelope_digest, entry.bytes),
        "checked project revision tamper rejected");
    reject_tamper(row(draft.draft_identity, entry.checked_project_namespace, entry.checked_project_revision,
        "another-rig", entry.checked_rig_revision, entry.checked_rig_digest, entry.envelope_digest, entry.bytes),
        "checked rig identity tamper rejected");
    reject_tamper(row(draft.draft_identity, entry.checked_project_namespace, entry.checked_project_revision,
        entry.checked_rig_identity, changed_rig_revision, entry.checked_rig_digest, entry.envelope_digest, entry.bytes),
        "checked rig revision tamper rejected");
    reject_tamper(row(draft.draft_identity, entry.checked_project_namespace, entry.checked_project_revision,
        entry.checked_rig_identity, entry.checked_rig_revision, hash("another rig digest"), entry.envelope_digest, entry.bytes),
        "checked rig digest tamper rejected");
    reject_tamper(row(draft.draft_identity, entry.checked_project_namespace, entry.checked_project_revision,
        entry.checked_rig_identity, entry.checked_rig_revision, entry.checked_rig_digest, hash("another envelope digest"), entry.bytes),
        "envelope digest tamper rejected");
    reject_tamper(row(draft.draft_identity, entry.checked_project_namespace, entry.checked_project_revision,
        entry.checked_rig_identity, entry.checked_rig_revision, entry.checked_rig_digest, entry.envelope_digest,
        alternative_record.value().serialize()), "canonical draft bytes tamper rejected");
    require(!reopened.unadmitted_mode_key_draft(draft.draft_identity, reopened_state.project_generation,
        "other-project", reopened_state.project_revision), "draft read refuses wrong project namespace");
    const auto object = reopened_state.objects.front().object.id;
    require(application::HumanApplicationAccess::dispatch(reopened,
        application::SetSelectionModeAction{editor::SelectionMode::vertex}) &&
        application::HumanApplicationAccess::dispatch(reopened,
            application::SelectVertexAction{object, root.value()}), "select source vertex");
    editor::ToolArguments edit; edit.position = {0.1, 0, 0};
    require(static_cast<bool>(application::HumanApplicationAccess::dispatch(reopened,
        application::InvokeToolAction{"mesh.set-vertex-position", edit})), "make accepted rig stale");
    reopened_state = reopened.snapshot();
    auto stale_source = draft;
    stale_source.draft_identity = "stale-rig-draft";
    stale_source.expected_project_generation = reopened_state.project_generation;
    stale_source.expected_source_namespace = reopened_state.source_namespace;
    stale_source.expected_project_revision = reopened_state.project_revision;
    require(!application::HumanApplicationAccess::store_unadmitted_mode_key_draft(reopened, stale_source),
        "current readiness refuses changed source mesh");
    application::ClearHistoricalAutorigIkFkRequest clear;
    clear.expected_project_generation = reopened_state.project_generation;
    clear.expected_project_revision = reopened_state.project_revision;
    clear.identity = "body"; clear.expected_record_revision = configured.value().record_revision;
    clear.acknowledge_configuration_loss = true; clear.justification = "clear stale rig authoring";
    require(static_cast<bool>(application::HumanApplicationAccess::clear_historical_autorig_ik_fk(reopened, clear)),
        "explicitly clear historical rig configuration");
    reopened_state = reopened.snapshot();
    reopened_draft = reopened.unadmitted_mode_key_draft(draft.draft_identity, reopened_state.project_generation,
        reopened_state.source_namespace, reopened_state.project_revision);
    require(reopened_draft && reopened_draft.value().record.serialize() == original_record &&
        reopened_draft.value().checked_project_revision == draft.expected_project_revision &&
        reopened_draft.value().checked_rig_identity == "body" &&
        reopened_draft.value().checked_rig_revision == configured.value().record_revision &&
        reopened_draft.value().checked_rig_digest == rig_digest &&
        reopened_draft.value().envelope_digest == restored_envelope_digest,
        "historical rig clear preserves independent unadmitted draft");
    require(static_cast<bool>(journal.verify()), "draft journal verifies");
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}
