#include "authoring_justification.hpp"
#include <carto/application/application.hpp>
#include <carto/character/rig_record.hpp>
#include <carto/workspace/commands.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <span>
#include <sstream>
#include <utility>

namespace carto::application {
namespace {
core::Diagnostic invalid(std::string message) {
    return {core::ErrorCode::validation_failed, std::move(message)};
}
core::Diagnostic stale(std::string message) {
    return {core::ErrorCode::stale_data, std::move(message)};
}
std::string hex_text(std::string_view text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() * 2U);
    for (unsigned char c : text) { result.push_back(digits[c >> 4U]); result.push_back(digits[c & 15U]); }
    return result;
}
assets::Sha256Digest digest(std::string_view bytes) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

// A detached workspace is explicitly adopted; no other workspace or journal
// is written. Legacy temporal references are quarantined because their v1
// locale-dependent digests cannot be safely rebound here.
core::Result<void> validate_workspace(const workspace::CharacterSourceStore& store,
    const project::AcceptedAutorigBundle& accepted, const project::ProjectDocument& document) {
    if (std::locale() != std::locale::classic())
        return core::Result<void>::failure(invalid("legacy graph codecs require the classic C++ locale"));
    if (auto r = store.validate(); !r) return r;
    if (store.bundles().size() != 1U)
        return core::Result<void>::failure(invalid("control workspace must contain exactly one character"));
    const auto& bundle = store.bundles().begin()->second;
    const auto& source = accepted.reviewed.acceptance;
    const auto* root = document.scene().find(bundle.definition.root);
    const auto world = document.scene().world_transform(bundle.definition.root);
    if (bundle.definition.project_namespace != document.source_namespace() ||
        bundle.definition.source_revision != source.mesh_revision ||
        bundle.definition.source_digest != source.mesh_geometry_digest || !root ||
        !root->mesh_asset || *root->mesh_asset != source.source.asset_id || !world)
        return core::Result<void>::failure(stale("workspace character is not bound to the current scene source"));
    if (root->locked)
        return core::Result<void>::failure(invalid("locked scene source cannot receive authored control keys"));
    const auto& transform = world.value();
    if (transform.translation.x != 0.0 || transform.translation.y != 0.0 || transform.translation.z != 0.0 ||
        transform.rotation.x != 0.0 || transform.rotation.y != 0.0 || transform.rotation.z != 0.0 ||
        transform.rotation.w != 1.0 || transform.scale.x != 1.0 || transform.scale.y != 1.0 || transform.scale.z != 1.0)
        return core::Result<void>::failure(invalid("supported control frame requires an identity scene root transform"));
    if (bundle.motion || bundle.channel || accepted.ik_fk.size() != 1U)
        return core::Result<void>::failure(invalid("legacy temporal channels and multiple chains are unsupported"));
    auto rig = character::rig_definition_digest(bundle.rig);
    auto graph = bundle.graph.canonical_digest();
    auto controls = bundle.rig.control_graph.canonical_digest();
    const auto& config = accepted.ik_fk.front();
    if (!rig || !graph || !controls || bundle.graph.nodes().size() != 3U ||
        rig.value() != config.rig_digest || graph.value() != config.graph_digest ||
        bundle.definition.rig.rig != config.rig || bundle.definition.rig.graph_digest != config.graph_digest ||
        bundle.definition.control_graph.owner != config.rig || bundle.definition.control_graph.digest != controls.value() ||
        bundle.rig.id != config.rig || bundle.control_axis_tracks.size() != 7U)
        return core::Result<void>::failure(stale("workspace rig, graph or component tracks do not match the accepted rig"));
    if (!config.ik_pole)
        return core::Result<void>::failure(invalid("supported switch requires an explicit pole direction control"));
    for (auto id : {config.ik_target, *config.ik_pole, config.mode_control}) {
        const auto* control = bundle.rig.control_graph.find(id);
        if (!control) return core::Result<void>::failure(invalid("required control is absent"));
        const auto& t = control->transform;
        if (t.translation.x != 0.0 || t.translation.y != 0.0 || t.translation.z != 0.0 ||
            t.rotation.x != 0.0 || t.rotation.y != 0.0 || t.rotation.z != 0.0 || t.rotation.w != 1.0 ||
            t.scale.x != 1.0 || t.scale.y != 1.0 || t.scale.z != 1.0)
            return core::Result<void>::failure(invalid("supported controls require identity local display transforms"));
    }
    for (const auto& track : bundle.control_axis_tracks) {
        if (track.source_namespace != document.source_namespace() ||
            track.source_revision != accepted.reviewed.acceptance.mesh_revision ||
            track.rig_digest != config.rig_digest || track.graph_digest != config.graph_digest ||
            track.reference_frame != "graph-world")
            return core::Result<void>::failure(stale("control track source, rest graph or frame pin is stale"));
        const bool target = track.control == config.ik_target && track.axis < 3U &&
            track.role == workspace::ControlTrackRole::ik_target;
        const bool pole = track.control == *config.ik_pole && track.axis < 3U &&
            track.role == workspace::ControlTrackRole::ik_pole;
        const bool mode = track.control == config.mode_control && track.axis == config.mode_axis &&
            track.role == workspace::ControlTrackRole::ik_fk_mode;
        if (!(target || pole || mode))
            return core::Result<void>::failure(invalid("unsupported control track role or axis"));
    }
    const auto* mode = bundle.rig.control_graph.find(config.mode_control);
    if (!mode || mode->follow_space || !mode->affected_joints.empty())
        return core::Result<void>::failure(invalid("mode control must be isolated"));
    for (const auto& [id, node] : bundle.rig.control_graph.nodes()) {
        (void)id;
        if (node.follow_space && *node.follow_space == config.mode_control)
            return core::Result<void>::failure(invalid("mode control has unsupported followers"));
    }
    return core::Result<void>::success();
}

struct PreparedKey {
    std::string result_bytes;
    assets::Sha256Digest proposal_digest;
    pose::FkToIkControlFrameProposal proposal;
};

core::Result<ControlWorkspacePoseView> evaluate_owned_pose(
    const workspace::CharacterSourceStore& store, const project::AcceptedAutorigBundle& accepted,
    const project::ProjectDocument& document, character::CharacterId character, character::RationalTime tick) {
    using Result = core::Result<ControlWorkspacePoseView>;
    if (auto r = validate_workspace(store, accepted, document); !r) return Result::failure(r.error());
    if (!tick.validate()) return Result::failure(invalid("invalid exact control tick"));
    const auto* bundle = store.find(character);
    if (!bundle) return Result::failure(stale("workspace character identity changed"));
    const auto& config = accepted.ik_fk.front();
    pose::ControlValues values;
    for (const auto& track : bundle->control_axis_tracks) {
        if (track.curve.timebase_ticks_per_second != tick.ticks_per_second ||
            track.curve.keys.front().time.tick > tick.tick)
            return Result::failure(invalid("control track has no authored value at the exact tick"));
        auto sampled = track.curve.sample(tick);
        if (!sampled) return Result::failure(sampled.error());
        values[track.control][track.axis] = sampled.value();
    }
    const double mode = values[config.mode_control][config.mode_axis];
    auto source_digest = store.canonical_digest();
    if (!source_digest) return Result::failure(source_digest.error());
    if (mode == config.fk_mode_value) {
        auto fk = pose::evaluate_fk(bundle->graph);
        if (!fk) return Result::failure(fk.error());
        return Result::success({pose::IkFkMode::fk, std::move(fk.value()), source_digest.value()});
    }
    if (mode != config.ik_mode_value)
        return Result::failure(invalid("control mode is neither authored FK nor IK"));
    pose::IkControlRigidFrame frame;
    auto authoring = config.canonical_digest();
    if (!authoring) return Result::failure(authoring.error());
    frame.authoring_digest = authoring.value();
    frame.frame_identity = "graph-world";
    frame.scene_unit_identity = "rig-default-scene-unit";
    frame.max_point_round_trip_error = 1e-9;
    frame.max_direction_round_trip_error = 1e-9;
    auto ik = pose::decode_ik_control_frame(frame, config, bundle->rig, bundle->graph, accepted.limits, values);
    if (!ik) return Result::failure(ik.error());
    pose::TwoBoneIkRequest request;
    request.root = config.chain.root; request.mid = config.chain.mid; request.effector = config.chain.effector;
    request.target = ik.value().target; request.pole_vector = ik.value().pole_vector; request.budget = config.budget;
    auto solved = pose::solve_two_bone(bundle->graph, {}, &accepted.limits, request);
    if (!solved || !solved.value().converged)
        return Result::failure(invalid("authored IK control tracks do not converge"));
    auto fk = pose::evaluate_fk(bundle->graph, solved.value().solved_locals);
    if (!fk) return Result::failure(fk.error());
    return Result::success({pose::IkFkMode::ik, std::move(fk.value()), source_digest.value()});
}

core::Result<PreparedKey> prepare_key(workspace::CharacterSourceStore store,
    const project::AcceptedAutorigBundle& accepted, const project::ProjectDocument& document,
    character::CharacterId character, character::RationalTime tick) {
    using Result = core::Result<PreparedKey>;
    if (auto r = validate_workspace(store, accepted, document); !r) return Result::failure(r.error());
    if (!tick.validate() || tick.tick <= 0 || store.revision().exhausted() || store.revision().next().exhausted())
        return Result::failure(invalid("switch requires a positive exact tick and available workspace revision"));
    auto* bundle = store.find(character);
    if (!bundle) return Result::failure(stale("workspace character identity changed"));
    const auto& config = accepted.ik_fk.front();
    const character::RationalTime previous{tick.tick - 1, tick.ticks_per_second};
    pose::ControlValues current;
    for (const auto& track : bundle->control_axis_tracks) {
        if (track.curve.timebase_ticks_per_second != tick.ticks_per_second ||
            track.curve.keys.front().time.tick > previous.tick ||
            std::any_of(track.curve.keys.begin(), track.curve.keys.end(), [tick](const auto& key) {
                return key.time.tick == tick.tick;
            }))
            return Result::failure(invalid("each track requires a preceding key and no key at the switch tick"));
        auto sampled = track.curve.sample(previous);
        if (!sampled) return Result::failure(sampled.error());
        current[track.control][track.axis] = sampled.value();
    }
    if (current[config.mode_control][config.mode_axis] != config.fk_mode_value)
        return Result::failure(stale("source mode is not FK at the preceding tick"));
    pose::IkControlRigidFrame frame;
    auto authoring = config.canonical_digest();
    if (!authoring) return Result::failure(authoring.error());
    frame.authoring_digest = authoring.value();
    frame.frame_identity = "graph-world";
    frame.scene_unit_identity = "rig-default-scene-unit";
    frame.max_point_round_trip_error = 1e-9;
    frame.max_direction_round_trip_error = 1e-9;
    // No FK tracks are present in this supported case. The current FK pose is
    // the exact accepted graph rest pose, not an external pose assertion.
    auto proposal = pose::propose_fk_to_ik_control_frame(frame, config, bundle->rig,
        bundle->graph, accepted.limits, {}, current);
    if (!proposal) return Result::failure(proposal.error());
    auto before = pose::evaluate_fk(bundle->graph);
    auto after = pose::evaluate_fk(bundle->graph, proposal.value().decoded_solve.solved_locals);
    if (!before || !after) return Result::failure(invalid("cannot verify matched joint orientations"));
    for (auto joint : {config.chain.root, config.chain.mid, config.chain.effector}) {
        const auto left = before.value().world.find(joint);
        const auto right = after.value().world.find(joint);
        if (left == before.value().world.end() || right == after.value().world.end())
            return Result::failure(invalid("matched joint is absent"));
        const auto a = left->second.rotation.normalized();
        const auto b = right->second.rotation.normalized();
        const double d = std::abs(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
        if (!std::isfinite(d) || 2.0 * std::acos(std::clamp(d, 0.0, 1.0)) > 1e-6)
            return Result::failure(invalid("matched orientation exceeds supported continuity bound"));
    }
    auto source_digest = store.canonical_digest();
    if (!source_digest) return Result::failure(source_digest.error());
    workspace::AnimateEditControlTracks edit;
    edit.expected_source_revision = store.revision();
    edit.expected_store_digest = source_digest.value();
    for (const auto& track : bundle->control_axis_tracks) {
        const double value = track.control == config.mode_control ? config.ik_mode_value :
            proposal.value().candidate_values.at(track.control)[track.axis];
        edit.edits.push_back({track.control, track.axis, track.curve_digest, {tick, value, 0.0, 0.0}, true});
    }
    auto applied = workspace::apply_control_track_edits(*bundle, edit);
    if (!applied) return Result::failure(applied.error());
    store.advance_revision();
    if (auto r = store.validate(); !r) return Result::failure(r.error());
    auto evaluated = evaluate_owned_pose(store, accepted, document, character, tick);
    if (!evaluated || evaluated.value().mode != pose::IkFkMode::ik)
        return Result::failure(invalid("newly authored control tracks do not evaluate as IK"));
    for (auto joint : {config.chain.root, config.chain.mid, config.chain.effector}) {
        const auto& a = after.value().world.at(joint);
        const auto& b = evaluated.value().pose.world.at(joint);
        const double position = (a.translation - b.translation).length();
        const auto qa = a.rotation.normalized(); const auto qb = b.rotation.normalized();
        const double q = std::abs(qa.x*qb.x + qa.y*qb.y + qa.z*qb.z + qa.w*qb.w);
        if (!std::isfinite(position) || position > config.max_position_jump || !std::isfinite(q) ||
            2.0 * std::acos(std::clamp(q, 0.0, 1.0)) > 1e-6)
            return Result::failure(invalid("stored keys do not reproduce the reviewed pose"));
    }
    auto result_bytes = store.serialize();
    std::ostringstream evidence;
    evidence.imbue(std::locale::classic());
    evidence << std::setprecision(17) << "CARTO_REST_IK_KEY_REVIEW_1\n" << source_digest.value().hex()
        << '\n' << digest(result_bytes).hex() << '\n' << authoring.value().hex() << '\n'
        << character.value << ' ' << tick.tick << ' ' << tick.ticks_per_second << '\n'
        << proposal.value().decoded_position_jump << ' ' << proposal.value().decoded_solve.residual << '\n';
    for (const auto& [joint, local] : proposal.value().decoded_solve.solved_locals)
        evidence << joint.value << ' ' << local.translation.x << ' ' << local.translation.y << ' '
            << local.translation.z << ' ' << local.rotation.x << ' ' << local.rotation.y << ' '
            << local.rotation.z << ' ' << local.rotation.w << ' ' << local.scale.x << ' '
            << local.scale.y << ' ' << local.scale.z << '\n';
    return Result::success({std::move(result_bytes), digest(evidence.str()), std::move(proposal.value())});
}
} // namespace

core::Result<DispatchReceipt> HumanApplicationAccess::adopt_control_workspace(
    ApplicationSession& session, const AdoptControlWorkspaceRequest& request) {
    return session.adopt_control_workspace(HumanActionAdmission{}, request);
}

core::Result<DispatchReceipt> HumanApplicationAccess::accept_ik_fk_key(
    ApplicationSession& session, IkFkKeyReview& review, std::string justification) {
    return session.accept_ik_fk_key(HumanActionAdmission{}, review, std::move(justification));
}

core::Result<DispatchReceipt> ApplicationSession::adopt_control_workspace(
    HumanActionAdmission, const AdoptControlWorkspaceRequest& request) {
    if (std::locale() != std::locale::classic() || active_source_ != OperationSource::human ||
        !detail::justification_valid(request.justification) || !journal_)
        return core::Result<DispatchReceipt>::failure(invalid("workspace adoption requires a saved project and explicit human justification"));
    if (document_.control_workspace())
        return core::Result<DispatchReceipt>::failure(stale("project already owns a control workspace"));
    auto rig = accepted_autorig(request.rig_identity, request.expected_project_generation, request.expected_project_revision);
    if (!rig) return core::Result<DispatchReceipt>::failure(rig.error());
    const auto row = document_.accepted_autorigs().find(request.rig_identity);
    if (rig.value().record_revision != request.expected_rig_revision || row->second.digest != request.expected_rig_digest)
        return core::Result<DispatchReceipt>::failure(stale("accepted rig changed before workspace adoption"));
    auto store = workspace::CharacterSourceStore::deserialize(request.workspace_bytes);
    if (!store || store.value().serialize() != request.workspace_bytes)
        return core::Result<DispatchReceipt>::failure(invalid("workspace must use the current canonical codec"));
    if (auto r = validate_workspace(store.value(), rig.value().bundle, document_); !r)
        return core::Result<DispatchReceipt>::failure(r.error());
    project::ProjectDocument candidate = document_;
    auto staged = candidate.set_control_workspace({}, document_.source_namespace(), request.rig_identity,
        request.expected_rig_revision, request.expected_rig_digest, request.workspace_bytes, digest(request.workspace_bytes));
    if (!staged) return core::Result<DispatchReceipt>::failure(staged.error());
    if (candidate.serialize().size() > 128ULL * 1024ULL * 1024ULL)
        return core::Result<DispatchReceipt>::failure(invalid("adopted project would exceed its readable bound"));
    editor::ProjectCommandAdmission admission;
    return execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(
        admission, document_, std::move(candidate), "Adopt Control Workspace"),
        "Adopt Control Workspace source=" + digest(request.workspace_bytes).hex() +
        " rig=" + request.expected_rig_digest.hex() + " reason_hex=" + hex_text(request.justification));
}

core::Result<IkFkKeyReview> ApplicationSession::review_ik_fk_key(
    std::string_view rig_identity, character::CharacterId character, character::RationalTime tick,
    std::uint64_t expected_generation, core::Revision expected_project_revision) const {
    using Result = core::Result<IkFkKeyReview>;
    if (std::locale() != std::locale::classic() || !ik_fk_review_identity_ || !document_.control_workspace())
        return Result::failure(invalid("project has no adopted control workspace"));
    auto rig = accepted_autorig(rig_identity, expected_generation, expected_project_revision);
    if (!rig) return Result::failure(rig.error());
    const auto& snapshot = *document_.control_workspace();
    const auto row = document_.accepted_autorigs().find(std::string(rig_identity));
    if (snapshot.source_namespace != document_.source_namespace() || snapshot.rig_identity != rig_identity ||
        snapshot.rig_revision != rig.value().record_revision || row == document_.accepted_autorigs().end() ||
        snapshot.rig_digest != row->second.digest)
        return Result::failure(stale("control workspace is bound to a different project or rig record"));
    auto store = workspace::CharacterSourceStore::deserialize(snapshot.bytes);
    if (!store || store.value().serialize() != snapshot.bytes || digest(snapshot.bytes) != snapshot.digest)
        return Result::failure(invalid("project control workspace failed integrity validation"));
    auto prepared = prepare_key(std::move(store.value()), rig.value().bundle, document_, character, tick);
    if (!prepared) return Result::failure(prepared.error());
    IkFkKeyReview review;
    review.session_identity_ = ik_fk_review_identity_;
    review.generation_ = project_generation_;
    review.project_revision_ = document_.revision();
    review.workspace_revision_ = snapshot.revision;
    review.workspace_digest_ = snapshot.digest;
    review.rig_identity_ = std::string(rig_identity);
    review.rig_revision_ = rig.value().record_revision;
    review.rig_digest_ = row->second.digest;
    review.character_ = character;
    review.tick_ = tick;
    std::ostringstream identity;
    identity.imbue(std::locale::classic());
    identity << "CARTO_PROJECT_IK_KEY_REVIEW_1\n" << std::quoted(document_.source_namespace()) << ' '
        << project_generation_ << ' ' << document_.revision().value() << ' ' << document_.scene().revision().value()
        << '\n' << std::quoted(review.rig_identity_) << ' ' << review.rig_revision_.value() << ' '
        << review.rig_digest_.hex() << '\n' << review.workspace_revision_.value() << ' '
        << review.workspace_digest_.hex() << '\n' << prepared.value().proposal_digest.hex() << '\n';
    review.proposal_digest_ = digest(identity.str());
    review.result_workspace_ = std::move(prepared.value().result_bytes);
    review.proposal_ = std::move(prepared.value().proposal);
    return Result::success(std::move(review));
}

core::Result<ControlWorkspaceView> ApplicationSession::control_workspace_source(
    std::uint64_t expected_generation, core::Revision expected_project_revision) const {
    using Result = core::Result<ControlWorkspaceView>;
    if (std::locale() != std::locale::classic())
        return Result::failure(invalid("control workspace requires the classic C++ locale"));
    if (expected_generation != project_generation_ || expected_project_revision != document_.revision())
        return Result::failure(stale("control workspace read belongs to another project state"));
    if (!document_.control_workspace()) return Result::failure(invalid("project has no control workspace"));
    const auto& snapshot = *document_.control_workspace();
    auto store = workspace::CharacterSourceStore::deserialize(snapshot.bytes);
    if (!store || store.value().serialize() != snapshot.bytes || digest(snapshot.bytes) != snapshot.digest)
        return Result::failure(invalid("project control workspace failed integrity validation"));
    return Result::success({snapshot.revision, snapshot.digest, snapshot.source_namespace, snapshot.rig_identity,
        snapshot.rig_revision, snapshot.rig_digest, std::move(store.value())});
}

core::Result<ControlWorkspacePoseView> ApplicationSession::evaluate_control_workspace_pose(
    std::string_view rig_identity, character::CharacterId character, character::RationalTime tick,
    std::uint64_t expected_generation, core::Revision expected_project_revision) const {
    using Result = core::Result<ControlWorkspacePoseView>;
    auto rig = accepted_autorig(rig_identity, expected_generation, expected_project_revision);
    if (!rig) return Result::failure(rig.error());
    if (!document_.control_workspace()) return Result::failure(invalid("project has no control workspace"));
    const auto& binding = *document_.control_workspace();
    const auto row = document_.accepted_autorigs().find(std::string(rig_identity));
    if (binding.source_namespace != document_.source_namespace() || binding.rig_identity != rig_identity ||
        binding.rig_revision != rig.value().record_revision || row == document_.accepted_autorigs().end() ||
        binding.rig_digest != row->second.digest)
        return Result::failure(stale("control workspace is bound to a different project or rig record"));
    auto source = control_workspace_source(expected_generation, expected_project_revision);
    if (!source) return Result::failure(source.error());
    return evaluate_owned_pose(source.value().source, rig.value().bundle, document_, character, tick);
}

core::Result<DispatchReceipt> ApplicationSession::accept_ik_fk_key(
    HumanActionAdmission, IkFkKeyReview& review, std::string justification) {
    const auto session = review.session_identity_.lock();
    if (std::locale() != std::locale::classic() || !review.active() || !session || session != ik_fk_review_identity_ ||
        active_source_ != OperationSource::human || !journal_ || !detail::justification_valid(justification))
        return core::Result<DispatchReceipt>::failure(invalid("key acceptance requires an active same-session human review and saved journal"));
    if (review.generation_ != project_generation_ || review.project_revision_ != document_.revision() ||
        !document_.control_workspace() || document_.control_workspace()->revision != review.workspace_revision_ ||
        document_.control_workspace()->digest != review.workspace_digest_)
        return core::Result<DispatchReceipt>::failure(stale("reviewed project or workspace has changed"));
    auto checked = review_ik_fk_key(review.rig_identity_, review.character_, review.tick_,
        review.generation_, review.project_revision_);
    if (!checked) return core::Result<DispatchReceipt>::failure(checked.error());
    if (checked.value().rig_revision_ != review.rig_revision_ || checked.value().rig_digest_ != review.rig_digest_ ||
        checked.value().proposal_digest_ != review.proposal_digest_ || checked.value().result_workspace_ != review.result_workspace_)
        return core::Result<DispatchReceipt>::failure(stale("reviewed key proposal no longer reproduces"));
    project::ProjectDocument candidate = document_;
    auto staged = candidate.set_control_workspace(review.workspace_revision_, document_.source_namespace(),
        review.rig_identity_, review.rig_revision_, review.rig_digest_, review.result_workspace_, digest(review.result_workspace_));
    if (!staged) return core::Result<DispatchReceipt>::failure(staged.error());
    if (candidate.serialize().size() > 128ULL * 1024ULL * 1024ULL)
        return core::Result<DispatchReceipt>::failure(invalid("keyed project would exceed its readable bound"));
    editor::ProjectCommandAdmission admission;
    auto result = execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(
        admission, document_, std::move(candidate), "Accept IK/FK Keys"),
        "Accept IK/FK Keys proposal=" + review.proposal_digest_.hex() + " workspace=" +
        review.workspace_digest_.hex() + " result=" + digest(review.result_workspace_).hex() +
        " reason_hex=" + hex_text(justification));
    if (result) review.cancel();
    return result;
}
} // namespace carto::application
