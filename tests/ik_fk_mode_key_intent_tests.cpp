#include <carto/character/rig_record.hpp>
#include <carto/pose/ik_fk_mode_key_intent.hpp>
#include <carto/pose/ik_fk_mode_key_record.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>

using namespace carto;
namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
assets::Sha256Digest hash(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
core::Transform rotation(double angle, double x = 0.0) {
    core::Transform result;
    result.translation.x = x;
    result.rotation = {0, 0, std::sin(angle / 2), std::cos(angle / 2)};
    return result;
}
struct Fixture {
    kinematic::KinematicGraph graph;
    kinematic::JointLimitSet limits;
    character::RigDefinition rig;
    pose::IkFkAuthoring authoring;
    temporal::TemporalChannel channel;
    temporal::MotionClip clip;
    character::MotionDocument document;
    motion::AnimationLayer layer;
    pose::ModeKeyIntentContext context;
    pose::ModeIntentCrosswalk crosswalk;
    pose::LocalPose fk;

    Fixture() {
        for (std::uint64_t i = 1; i <= 3; ++i) {
            kinematic::KinematicNode node;
            node.id = {i};
            if (i > 1) { node.parent = kinematic::JointId{i - 1}; node.rest.translation.x = 1; }
            node.kind = kinematic::JointKind::ball;
            node.node_class = kinematic::NodeClass::skinned;
            require(static_cast<bool>(graph.insert_node(node)), "graph fixture");
        }
        rig.id = {11};
        rig.kinematic_graph_digest = graph.canonical_digest().value();
        rig.joint_limits.digest = limits.canonical_digest().value();
        for (std::uint64_t i = 1; i <= 6; ++i) {
            character::ControlDefinition control;
            control.id = {i};
            if (i <= 3) control.affected_joints.push_back({i});
            if (i == 6) { control.allowed_axes = character::kAxisTranslateX; control.axis_maximum[0] = 1; }
            require(static_cast<bool>(rig.control_graph.insert(control)), "control fixture");
        }
        authoring.rig = rig.id;
        authoring.rig_digest = character::rig_definition_digest(rig).value();
        authoring.graph_digest = rig.kinematic_graph_digest;
        authoring.limits_digest = rig.joint_limits.digest;
        authoring.chain = {{1}, {2}, {3}};
        authoring.fk_root = {1}; authoring.fk_mid = {2}; authoring.fk_effector = {3};
        authoring.ik_target = {4}; authoring.ik_pole = character::ControlId{5}; authoring.mode_control = {6};
        channel.id = {7}; channel.channel_name = "mode-reference";
        channel.target = {temporal::TargetBindingKind::skinned_mesh, "character.body"};
        channel.duration = 10; channel.scalar_keyframes = {{0, 0}};
        clip.id = {8}; clip.clip_name = "clip"; clip.duration = 10;
        clip.channels = {{channel.id, channel.canonical_digest().value()}};
        document.id = {9}; document.timebase_ticks_per_second = 30;
        document.channels = {{channel.id, channel.canonical_digest().value()}};
        document.clips = {{clip.id, clip.canonical_digest().value()}};
        document.targets = {{{}, {6}}};
        layer.id = {10}; layer.kind = motion::LayerKind::base;
        layer.window_start = {0, 30}; layer.window_end = {120, 30};
        motion::LayerCurve item; item.target.control = {6}; item.curve.id = {12};
        item.curve.timebase_ticks_per_second = 30;
        item.curve.interpolation = motion::InterpolationKind::step;
        item.curve.keys = {{{0, 30}, 0, 0, 0}};
        layer.curves.push_back(item);
        crosswalk = {channel.target, channel.id, channel.canonical_digest().value(),
            layer.id, item.curve.id, item.curve.canonical_digest().value(), {6}, 0};
        context.source_namespace = "project.character";
        context.source_revision = core::Revision{3};
        context.source_digest = hash("source");
        context.motion_document_digest = hash("caller-motion-document");
        context.rig = authoring.rig;
        context.rig_digest = authoring.rig_digest;
        context.rest_graph_digest = authoring.graph_digest;
        context.limits_digest = authoring.limits_digest;
        context.source_pose_digest = hash("caller-pose");
        context.source_pose_assertion = pose::SourcePoseAssertion::evaluated_preview;
        context.authoring_digest = authoring.canonical_digest().value();
        context.reference_frame = "caller-world-frame";
        context.solver_input_frame = pose::SolverInputFrameAssertion::graph_world;
        context.lineage = {"scenario", "branch", "session", "restore"};
        context.clip_tick = {30, 30}; context.clip_start = {0, 30}; context.clip_end = {120, 30};
        context.source_tick = {48000, 48000}; context.source_start = {0, 48000}; context.source_end = {96000, 48000};
        context.channel_tick = {60, 60}; context.channel_start = {0, 60}; context.channel_end = {240, 60};
        fk[{1}] = rotation(0.4); fk[{2}] = rotation(-0.9, 1.0);
    }
    core::Result<pose::IkFkModeKeyIntent> run() const {
        return pose::propose_ik_fk_mode_key_intent(document, clip, channel, layer,
            context, crosswalk, authoring, rig, graph, limits,
            pose::IkFkMode::fk, pose::IkFkMode::ik, fk);
    }
};
}

int main() {
    Fixture fixture;
    const auto original_curve = fixture.layer.curves.front().curve.serialize();
    const auto original_channel = fixture.channel.serialize();
    const auto original_clip = fixture.clip.serialize();
    auto result = fixture.run();
    require(result && result.value().mode_before == 0 && result.value().mode_at_key == 1 &&
        result.value().proposed_curve.sample({29, 30}).value() == 0 &&
        result.value().proposed_curve.sample({30, 30}).value() == 1,
        "explicit mode key produces an authored step proposal");
    require(result.value().context.source_tick.tick == 48000 &&
        result.value().context.clip_tick.tick == 30 &&
        result.value().context.lineage.restore == "restore" &&
        result.value().crosswalk.layer == fixture.layer.id &&
        result.value().edit.source_digest == fixture.crosswalk.curve_digest &&
        result.value().match.match.position_jump <= fixture.authoring.max_position_jump,
        "separate exact clocks, asserted lineage and existing match receipt survive");
    require(fixture.layer.curves.front().curve.serialize() == original_curve &&
        fixture.channel.serialize() == original_channel && fixture.clip.serialize() == original_clip,
        "source layer, channel and clip remain unchanged");
    auto record = pose::make_ik_fk_mode_key_record(result.value());
    require(record && record.value().validate() && record.value().canonical_digest(),
        "portable intent validates and binds its insertion receipt");
    const auto record_bytes = record.value().serialize();
    auto decoded = pose::IkFkModeKeyRecord::deserialize(record_bytes);
    require(decoded && decoded.value().serialize() == record_bytes &&
        decoded.value().canonical_digest().value() == record.value().canonical_digest().value() &&
        decoded.value().context.clip_tick.tick == 30 && decoded.value().context.source_tick.tick == 48000 &&
        decoded.value().context.channel_tick.tick == 60 && decoded.value().context.channel_tick.ticks_per_second == 60 &&
        decoded.value().context.source_pose_assertion == pose::SourcePoseAssertion::evaluated_preview &&
        decoded.value().context.solver_input_frame == pose::SolverInputFrameAssertion::graph_world &&
        decoded.value().context.lineage.restore == std::optional<std::string>{"restore"},
        "canonical intent round trip preserves distinct clocks and asserted restore");
    require(record_bytes.find("MATCH") == std::string::npos,
        "portable intent does not serialize a match as achieved state");
    auto other_assertion = record.value();
    other_assertion.context.source_pose_digest = hash("another-caller-assertion");
    require(other_assertion.validate() && other_assertion.canonical_digest().value() != record.value().canonical_digest().value(),
        "assertion affects record identity without implying source authentication");
    for (const auto classification : {pose::SourcePoseAssertion::authored_rest,
            pose::SourcePoseAssertion::evaluated_preview,
            pose::SourcePoseAssertion::current_admitted_assertion,
            pose::SourcePoseAssertion::authored_intended_future,
            pose::SourcePoseAssertion::predicted_alternative,
            pose::SourcePoseAssertion::observed_assertion,
            pose::SourcePoseAssertion::executed_assertion}) {
        auto classified = record.value();
        classified.context.source_pose_assertion = classification;
        auto round_trip = pose::IkFkModeKeyRecord::deserialize(classified.serialize());
        require(round_trip && round_trip.value().context.source_pose_assertion == classification,
            "caller pose classification survives canonical encoding without granting authority");
    }
    auto changed_record = record.value();
    changed_record.edit.source_digest = hash("wrong-source");
    require(!changed_record.validate(), "receipt source pin tampering refused");
    changed_record = record.value();
    changed_record.edit.affected_key_count = 2;
    require(!changed_record.validate(), "non-single insertion refused");
    changed_record = record.value();
    changed_record.proposed_curve.keys.front().value = 1;
    require(!changed_record.validate(), "result curve tampering refused");
    changed_record = record.value();
    changed_record.proposed_curve.keys.insert(changed_record.proposed_curve.keys.begin() + 1,
        {{10, 30}, 0, 0, 0});
    changed_record.edit.result_digest = changed_record.proposed_curve.canonical_digest().value();
    require(!changed_record.validate(), "self-consistent result hash cannot hide an extra source edit");
    require(!pose::IkFkModeKeyRecord::deserialize(record_bytes + "trailing"),
        "record trailing bytes refused");
    auto bad_frame_record = record_bytes;
    const auto frame_at = bad_frame_record.find("SOLVER_INPUT_FRAME 1\n");
    require(frame_at != std::string::npos, "frame assertion fixture");
    bad_frame_record.replace(frame_at, std::string("SOLVER_INPUT_FRAME 1\n").size(),
        "SOLVER_INPUT_FRAME 0\n");
    require(!pose::IkFkModeKeyRecord::deserialize(bad_frame_record),
        "portable record refuses an unspecified frame assertion");
    auto exact_end = fixture;
    exact_end.context.clip_end = {300, 30};
    exact_end.context.channel_end = {600, 60};
    require(static_cast<bool>(exact_end.run()), "exact integral clip endpoint is admitted");
    auto back = fixture;
    back.layer.curves.front().curve = result.value().proposed_curve;
    back.crosswalk.curve_digest = back.layer.curves.front().curve.canonical_digest().value();
    back.context.clip_tick = {60, 30}; back.context.source_tick = {90000, 48000};
    auto reverse = pose::propose_ik_fk_mode_key_intent(back.document, back.clip, back.channel, back.layer,
        back.context, back.crosswalk, back.authoring, back.rig, back.graph, back.limits,
        pose::IkFkMode::ik, pose::IkFkMode::fk, {}, result.value().match.match.ik);
    require(reverse && reverse.value().mode_before == 1 && reverse.value().mode_at_key == 0,
        "explicit IK to FK intent is independently proposed");

    auto no_restore = fixture; no_restore.context.lineage.restore.reset();
    auto no_restore_intent = no_restore.run();
    require(static_cast<bool>(no_restore_intent), "unasserted restore stays absent");
    auto no_restore_record = pose::make_ik_fk_mode_key_record(no_restore_intent.value());
    auto no_restore_copy = pose::IkFkModeKeyRecord::deserialize(no_restore_record.value().serialize());
    require(no_restore_copy && !no_restore_copy.value().context.lineage.restore,
        "absent restore is encoded distinctly, not asserted false");
    auto bad = fixture; bad.context.lineage.restore = std::string{};
    require(!bad.run(), "empty asserted restore identity refused");
    bad = fixture; bad.context.lineage.scenario.clear();
    require(!bad.run(), "missing scenario lineage refused");
    bad = fixture; bad.context.lineage.session = "   ";
    require(!bad.run(), "whitespace-only session identity refused");
    bad = fixture; bad.context.source_pose_assertion = pose::SourcePoseAssertion::unspecified;
    require(!bad.run(), "unspecified source pose assertion refused");
    bad = fixture; bad.context.source_pose_assertion = static_cast<pose::SourcePoseAssertion>(255);
    require(!bad.run(), "unknown source pose assertion refused");
    bad = fixture; bad.context.solver_input_frame = pose::SolverInputFrameAssertion::unspecified;
    require(!bad.run(), "unspecified solver input frame assertion refused");
    bad = fixture; bad.context.solver_input_frame = static_cast<pose::SolverInputFrameAssertion>(255);
    require(!bad.run(), "unknown solver input frame assertion refused");
    bad = fixture; bad.context.source_tick.ticks_per_second = 60;
    require(!bad.run(), "mixed source-clock timebases refused");
    bad = fixture; bad.context.channel_tick.ticks_per_second = 30;
    require(!bad.run(), "mixed channel-local timebases refused");
    bad = fixture; bad.context.channel_end = {601, 60};
    require(!bad.run(), "channel-local interval beyond duration refused");
    bad = fixture; bad.context.channel_end.tick = (std::int64_t{1} << 53) + 1;
    require(!bad.run(), "channel endpoint beyond exact integer-to-double range refused");
    bad = fixture;
    bad.channel.duration = 1.0 / 3.0;
    bad.context.channel_tick = {1, 3}; bad.context.channel_start = {0, 3}; bad.context.channel_end = {1, 3};
    bad.crosswalk.channel_digest = bad.channel.canonical_digest().value();
    bad.document.channels.front().digest = bad.crosswalk.channel_digest;
    bad.clip.channels.front().channel_digest = bad.crosswalk.channel_digest;
    bad.document.clips.front().digest = bad.clip.canonical_digest().value();
    require(!bad.run(), "ambiguous fractional duration boundary refuses conservatively");
    bad = fixture; bad.context.clip_tick = {0, 30};
    require(!bad.run(), "missing prior clip tick refused");
    bad = fixture; bad.context.clip_end.tick = (std::int64_t{1} << 53) + 1;
    require(!bad.run(), "clip endpoint beyond exact integer-to-double range refused");
    bad = fixture; bad.context.authoring_digest = hash("stale-config");
    require(!bad.run(), "stale configuration digest refused");
    bad = fixture; bad.crosswalk.channel_digest = hash("other-channel");
    require(!bad.run(), "channel digest mismatch refused");
    bad = fixture; bad.crosswalk.stable_target.stable_id = "other";
    require(!bad.run(), "stable target mismatch refused");
    bad = fixture; bad.crosswalk.layer = {99};
    require(!bad.run(), "wrong layer refused");
    bad = fixture; bad.crosswalk.axis = 1;
    require(!bad.run(), "unasserted control axis refused");
    bad = fixture; bad.document.targets.clear();
    require(!bad.run(), "control outside document refused");
    bad = fixture; bad.layer.window_start = {30, 30};
    require(!bad.run(), "prior tick outside layer window refused");
    bad = fixture; bad.layer.curves.front().curve.keys.front().time.tick = 30;
    bad.crosswalk.curve_digest = bad.layer.curves.front().curve.canonical_digest().value();
    require(!bad.run(), "clamped future first key cannot fabricate a prior mode");
    bad = fixture; bad.layer.curves.front().curve.keys.push_back({{30, 30}, 0, 0, 0});
    bad.crosswalk.curve_digest = bad.layer.curves.front().curve.canonical_digest().value();
    require(!bad.run(), "existing switch-tick key refused without replacement");
    bad = fixture; bad.layer.curves.front().curve.interpolation = motion::InterpolationKind::linear;
    bad.crosswalk.curve_digest = bad.layer.curves.front().curve.canonical_digest().value();
    require(!bad.run(), "interpolated mode curve refused");
    bad = fixture; bad.layer.curves.front().curve.keys.front().value = 0.5;
    bad.crosswalk.curve_digest = bad.layer.curves.front().curve.canonical_digest().value();
    require(!bad.run(), "non-discrete mode value refused");
    bad = fixture; bad.context.rest_graph_digest = hash("other-rest");
    require(!bad.run(), "rest graph mismatch refused");
    std::cout << "IK/FK mode-key intent tests passed\n";
}
