#include <carto/character/motion_document.hpp>
#include <carto/character/time.hpp>
#include <carto/kinematic/identity.hpp>
#include <carto/motion/curve.hpp>
#include <carto/motion/curve_edit.hpp>
#include <carto/motion/layers.hpp>
#include <carto/motion/mocap.hpp>
#include <carto/motion/performance.hpp>
#include <carto/motion/retarget.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::assets::Sha256Digest;
using carto::character::RationalTime;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1e-9) {
    return std::fabs(left - right) <= epsilon;
}

Sha256Digest digest_of(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

carto::motion::KeyedCurve two_key_curve(
    carto::motion::InterpolationKind kind,
    std::int64_t first_tick,
    double first_value,
    std::int64_t second_tick,
    double second_value,
    double tangent_out = 0.0,
    double tangent_in = 0.0) {
    carto::motion::KeyedCurve curve;
    curve.id = carto::motion::MotionCurveId{7U};
    curve.timebase_ticks_per_second = 30U;
    curve.interpolation = kind;
    curve.keys.push_back(carto::motion::CurveKey{
        RationalTime{first_tick, 30U}, first_value, 0.0, tangent_out});
    curve.keys.push_back(carto::motion::CurveKey{
        RationalTime{second_tick, 30U}, second_value, tangent_in, 0.0});
    return curve;
}

void curve_sampling_is_exact_at_ticks_and_midpoints() {
    using namespace carto::motion;
    KeyedCurve linear = two_key_curve(
        InterpolationKind::linear, 0, 0.0, 30, 10.0);
    require(static_cast<bool>(linear.validate()), "linear curve validates");

    const auto at_start = linear.sample(RationalTime{0, 30U});
    const auto at_end = linear.sample(RationalTime{30, 30U});
    const auto midpoint = linear.sample(RationalTime{15, 30U});
    require(at_start && at_start.value() == 0.0, "linear curve hits its first key exactly");
    require(at_end && at_end.value() == 10.0, "linear curve hits its last key exactly");
    require(midpoint && midpoint.value() == 5.0,
        "linear curve hits the exact midpoint between keys");

    // Exactness is bit-for-bit, not tolerance-based: a curve on a 3-tick
    // timebase sampled exactly at a key returns the stored double unchanged.
    carto::motion::KeyedCurve precise;
    precise.id = carto::motion::MotionCurveId{9U};
    precise.timebase_ticks_per_second = 3U;
    precise.interpolation = carto::motion::InterpolationKind::linear;
    precise.keys.push_back(carto::motion::CurveKey{RationalTime{0, 3U}, 1.0 / 3.0});
    precise.keys.push_back(carto::motion::CurveKey{RationalTime{3, 3U}, 1.0 / 3.0});
    const auto exact = precise.sample(RationalTime{3, 3U});
    require(exact && exact.value() == 1.0 / 3.0,
        "sampling exactly on a key returns the stored double bit-for-bit");
    const auto interpolated = precise.sample(RationalTime{1, 3U});
    require(interpolated && interpolated.value() == 1.0 / 3.0,
        "interpolating equal neighbors reproduces the value exactly");

    KeyedCurve smooth = two_key_curve(
        InterpolationKind::smoothstep, 0, 0.0, 30, 10.0);
    const auto smooth_mid = smooth.sample(RationalTime{15, 30U});
    require(smooth_mid && near(smooth_mid.value(), 5.0),
        "smoothstep passes through the key midpoint");
    const auto smooth_quarter = smooth.sample(RationalTime{15 / 2, 30U});
    require(smooth_quarter && smooth_quarter.value() < 2.5,
        "smoothstep eases below the linear quarter value");

    // Hermite bezier with flat tangents equals smoothstep-like easing.
    KeyedCurve bezier = two_key_curve(
        InterpolationKind::cubic_bezier, 0, 0.0, 30, 10.0, 0.0, 0.0);
    const auto bezier_mid = bezier.sample(RationalTime{15, 30U});
    require(bezier_mid && near(bezier_mid.value(), 5.0),
        "bezier with flat tangents passes through the key midpoint");
    // Unit slopes over a one-second span: h10 * dt * 1 + h11 * dt * 1 at t=.5
    // equals zero by symmetry, so the midpoint stays 5.
    KeyedCurve sloped = two_key_curve(
        InterpolationKind::cubic_bezier, 0, 0.0, 30, 10.0, 1.0, 1.0);
    const auto sloped_mid = sloped.sample(RationalTime{15, 30U});
    require(sloped_mid && near(sloped_mid.value(), 5.0),
        "bezier with symmetric slopes stays at the midpoint");

    const auto clamped = linear.sample(RationalTime{-30, 30U});
    require(clamped && clamped.value() == 0.0, "sampling before the first key clamps");
    const auto clamped_end = linear.sample(RationalTime{90, 30U});
    require(clamped_end && clamped_end.value() == 10.0, "sampling after the last key clamps");
}

void curve_validation_is_monotonic_and_single_timebase() {
    using namespace carto::motion;
    KeyedCurve unsorted = two_key_curve(InterpolationKind::linear, 30, 1.0, 0, 2.0);
    require(!unsorted.validate(), "non-monotonic key ticks fail validation");

    KeyedCurve cross_timebase = two_key_curve(InterpolationKind::linear, 0, 0.0, 30, 1.0);
    cross_timebase.keys[1].time.ticks_per_second = 60U;
    require(!cross_timebase.validate(), "mixed key timebases fail validation");

    KeyedCurve nan_tangent = two_key_curve(
        InterpolationKind::cubic_bezier, 0, 0.0, 30, 1.0);
    nan_tangent.keys[0].tangent_out = std::numeric_limits<double>::quiet_NaN();
    require(!nan_tangent.validate(), "non-finite tangents fail validation");

    KeyedCurve curve = two_key_curve(InterpolationKind::linear, 0, 0.0, 30, 1.0);
    const auto wrong_timebase = curve.sample(RationalTime{15, 60U});
    require(!wrong_timebase, "sampling off the curve timebase fails closed");
}

void rescale_receipts_distinguish_exact_from_rounded() {
    using namespace carto::motion;
    KeyedCurve curve = two_key_curve(InterpolationKind::linear, 0, 0.0, 15, 3.0);

    const auto exact_case = rescale_timebase(curve, 60U);
    require(static_cast<bool>(exact_case), "doubling the timebase rescales");
    require(exact_case.value().first.keys[1].time.tick == 30 &&
            exact_case.value().first.keys[1].time.ticks_per_second == 60U,
        "exact rescale doubles the timebase on every key tick");
    require(exact_case.value().second.exact &&
            exact_case.value().second.max_abs_time_error_seconds == 0.0,
        "exact rescale reports a zero error bound");

    const auto rounded_case = rescale_timebase(curve, 7U);
    require(static_cast<bool>(rounded_case), "incommensurate timebase rescale still succeeds");
    require(!rounded_case.value().second.exact,
        "incommensurate rescale is flagged inexact");
    require(rounded_case.value().second.max_abs_time_error_seconds > 0.0,
        "inexact rescale carries a positive error bound");
    // 15 ticks @30fps = 0.5s; nearest tick at 7tps is 4 (4/7 = 0.5714) or
    // 3 (0.4286): 3.5 ties to even -> 4? 0.5*7 = 3.5 -> ties-to-even gives 4? no:
    // nearbyint(3.5) rounds to even -> 4. The receipt records the bound.
    require(rounded_case.value().second.max_abs_time_error_seconds <= 0.5 / 7.0 + 1e-12,
        "error bound covers half a tick of grid rounding");

    const auto before = curve.canonical_digest();
    require(before && before.value() == exact_case.value().second.source_digest,
        "rescale receipt pins the exact source digest");
}

void curve_codec_round_trips_and_is_deterministic() {
    using namespace carto::motion;
    KeyedCurve curve = two_key_curve(
        InterpolationKind::cubic_bezier, 0, 0.25, 45, 2.5, 0.5, -0.25);
    const std::string text = curve.serialize();
    const auto parsed = KeyedCurve::deserialize(text);
    require(static_cast<bool>(parsed), "keyed curve deserializes");
    require(parsed.value().serialize() == text, "keyed curve codec round trips");
    const auto digest_a = curve.canonical_digest();
    const auto digest_b = KeyedCurve::deserialize(text).value().canonical_digest();
    require(digest_a && digest_b && digest_a.value() == digest_b.value(),
        "keyed curve digests are stable across codec round trips");
    require(!KeyedCurve::deserialize("CARTO_KEYED_CURVE 1\nKEYS 999999\n"),
        "oversized key counts fail closed before allocation");
    require(!KeyedCurve::deserialize("garbage"),
        "malformed curve text fails closed");
}

carto::motion::AnimationLayer make_layer(
    std::uint64_t id,
    carto::motion::LayerKind kind,
    double weight,
    const carto::character::MotionTargetRef& target,
    const carto::motion::KeyedCurve& curve,
    std::int64_t window_start = 0,
    std::int64_t window_end = 300) {
    carto::motion::AnimationLayer layer;
    layer.id = carto::motion::AnimationLayerId{id};
    layer.kind = kind;
    layer.weight = weight;
    layer.window_start = RationalTime{window_start, 30U};
    layer.window_end = RationalTime{window_end, 30U};
    layer.curves.push_back(carto::motion::LayerCurve{target, curve});
    return layer;
}

void layer_compositing_order_and_weights_are_deterministic() {
    using namespace carto;
    using namespace carto::motion;
    const character::MotionTargetRef target{
        kinematic::JointId{3U}, character::ControlId{0U}};

    AnimationLayerStack stack;
    stack.base_layer = AnimationLayerId{1U};
    stack.timebase_ticks_per_second = 30U;

    AnimationLayer base;
    base.id = AnimationLayerId{1U};
    base.kind = LayerKind::base;
    base.window_start = RationalTime{0, 30U};
    base.window_end = RationalTime{300, 30U};
    base.curves.push_back(LayerCurve{target, two_key_curve(
        InterpolationKind::linear, 0, 0.0, 300, 10.0)});
    stack.layers.push_back(base);

    // Layer 2 overrides fully toward 20; layer 3 adds a half-weight delta of 4.
    stack.layers.push_back(make_layer(
        2U, LayerKind::override_layer, 1.0, target,
        two_key_curve(InterpolationKind::linear, 0, 20.0, 300, 20.0)));
    AnimationLayer additive = make_layer(
        3U, LayerKind::additive, 0.5, target,
        two_key_curve(InterpolationKind::linear, 0, 4.0, 300, 4.0));
    additive.additive_reference = 0.0;
    stack.layers.push_back(additive);

    require(static_cast<bool>(stack.validate()), "layer stack validates");

    // base 0 at tick 0 -> override(w=1) 20 -> additive w=0.5 of 4 adds 2 => 22.
    const auto composed = stack.evaluate(target, RationalTime{0, 30U});
    require(composed && near(composed.value().value, 22.0),
        "override then additive compositing math is exact");

    // Half-weight override: base 0 lerps halfway to 20 -> 10; additive adds 2 -> 12.
    stack.layers[1].weight = 0.5;
    const auto half = stack.evaluate(target, RationalTime{0, 30U});
    require(half && near(half.value().value, 12.0),
        "per-layer weight scales the override contribution");
    stack.layers[1].weight = 1.0;

    // Order is content: swapping which id carries the override versus the
    // additive changes the composite. With the additive applied first
    // (id 2) and the override second (id 3), the override wins outright:
    // (10 + 0.5*4) then overridden to 20, not 22.
    AnimationLayerStack swapped;
    swapped.base_layer = AnimationLayerId{1U};
    swapped.timebase_ticks_per_second = 30U;
    swapped.layers.push_back(base);
    AnimationLayer early_additive = additive;
    early_additive.id = AnimationLayerId{2U};
    AnimationLayer late_override = stack.layers[1];
    late_override.id = AnimationLayerId{3U};
    swapped.layers.push_back(early_additive);
    swapped.layers.push_back(late_override);
    require(static_cast<bool>(swapped.validate()), "swapped stack validates");
    const auto swapped_value = swapped.evaluate(target, RationalTime{0, 30U});
    require(swapped_value && near(swapped_value.value().value, 20.0),
        "compositing order changes the result deterministically");
}

void layer_windows_and_missing_targets_are_honest() {
    using namespace carto;
    using namespace carto::motion;
    const character::MotionTargetRef target{
        kinematic::JointId{3U}, character::ControlId{0U}};
    const character::MotionTargetRef unknown{
        kinematic::JointId{99U}, character::ControlId{0U}};

    AnimationLayerStack stack;
    stack.base_layer = AnimationLayerId{1U};
    stack.timebase_ticks_per_second = 30U;
    AnimationLayer base;
    base.id = AnimationLayerId{1U};
    base.kind = LayerKind::base;
    base.window_start = RationalTime{0, 30U};
    base.window_end = RationalTime{300, 30U};
    base.curves.push_back(LayerCurve{target, two_key_curve(
        InterpolationKind::linear, 0, 0.0, 300, 10.0)});
    stack.layers.push_back(base);

    AnimationLayer windowed = make_layer(
        2U, LayerKind::override_layer, 1.0, target,
        two_key_curve(InterpolationKind::linear, 0, 50.0, 300, 50.0),
        60, 120);
    stack.layers.push_back(windowed);
    require(static_cast<bool>(stack.validate()), "windowed stack validates");

    const auto inside = stack.evaluate(target, RationalTime{90, 30U});
    require(inside && near(inside.value().value, 50.0),
        "override layer applies inside its window");
    const auto outside = stack.evaluate(target, RationalTime{30, 30U});
    require(outside && near(outside.value().value, 1.0),
        "override layer contributes nothing outside its window");

    const auto missing = stack.evaluate(unknown, RationalTime{30, 30U});
    require(!missing && missing.error().code == core::ErrorCode::not_found,
        "targets no layer addresses fail closed as not_found");

    // Missing-target policy against the motion document.
    character::MotionDocument document;
    document.id = character::MotionDocumentId{5U};
    document.timebase_ticks_per_second = 30U;
    document.targets.push_back(target);
    document.missing_target = character::MissingTargetHandling::fail;
    require(static_cast<bool>(document.validate()), "motion document validates");

    stack.layers[1].curves.push_back(LayerCurve{
        unknown, two_key_curve(InterpolationKind::linear, 0, 1.0, 300, 1.0)});
    std::sort(stack.layers[1].curves.begin(), stack.layers[1].curves.end());
    const auto hard = stack.check_targets(document);
    require(!hard, "missing target with fail policy is a hard error");

    document.missing_target = character::MissingTargetHandling::skip_with_receipt;
    const auto soft = stack.check_targets(document);
    require(soft && soft.value().skipped_targets.size() == 1U &&
            soft.value().skipped_targets.front() == unknown,
        "missing target with skip policy is reported, not faked");
}

carto::kinematic::JointCorrespondence correspondence_with_rests(
    const std::vector<std::pair<std::uint64_t, carto::core::Vec3d>>& rests) {
    carto::kinematic::JointCorrespondence correspondence;
    correspondence.source_namespace = "test-capture";
    correspondence.mesh_asset = 1U;
    correspondence.mesh_revision = carto::core::Revision{1U};
    correspondence.mesh_digest = digest_of("mesh");
    for (const auto& [joint, rest_translation] : rests) {
        carto::kinematic::JointIdentityRecord record;
        record.joint = carto::kinematic::JointId{joint};
        record.stable_name = "joint-" + std::to_string(joint);
        record.authored_rest.translation = rest_translation;
        record.bindings.push_back(carto::kinematic::MeshRevisionBinding{
            carto::core::Revision{1U}, digest_of("mesh"), 0U, record.authored_rest});
        require(static_cast<bool>(correspondence.insert(std::move(record))),
            "correspondence accepts a joint record");
    }
    require(static_cast<bool>(correspondence.validate()),
        "correspondence validates");
    return correspondence;
}

void retarget_maps_matching_joints_and_reports_residuals() {
    using namespace carto;
    using namespace carto::motion;
    // Humanoid A and B share semantics with different rest proportions.
    auto source = correspondence_with_rests({
        {1U, {0.0, 0.0, 0.0}},   // hips/root
        {2U, {0.0, 0.1, 0.0}},   // spine
        {3U, {0.2, 0.3, 0.0}},   // arm
    });
    auto destination = correspondence_with_rests({
        {11U, {0.0, 0.0, 0.0}},
        {12U, {0.0, 0.15, 0.0}},  // longer spine segment
        {13U, {0.3, 0.45, 0.0}},  // longer arm segment
        {14U, {0.0, 0.5, 0.1}},   // extra finger joint: unmapped
    });

    RetargetMap map;
    map.entries.push_back(JointMapEntry{kinematic::JointId{1U}, kinematic::JointId{11U}});
    map.entries.push_back(JointMapEntry{kinematic::JointId{2U}, kinematic::JointId{12U}});
    map.entries.push_back(JointMapEntry{kinematic::JointId{3U}, kinematic::JointId{13U}});
    require(static_cast<bool>(map.validate()), "retarget map validates");

    PoseSample sample;
    sample.time = RationalTime{15, 30U};
    // Source spine rotates 90 degrees about z and lifts 0.05 units.
    core::Transform spine_pose;
    spine_pose.translation = {0.0, 0.15, 0.0};
    spine_pose.rotation = {0.0, 0.0, std::sqrt(0.5), std::sqrt(0.5)};
    sample.locals.emplace(kinematic::JointId{2U}, spine_pose);
    sample.locals.emplace(kinematic::JointId{1U}, core::Transform::identity());
    sample.locals.emplace(kinematic::JointId{3U}, core::Transform::identity());

    RetargetPolicy policy;
    policy.translation_scale = 1.5;  // destination proportions are 1.5x

    const auto result = retarget_pose(source, destination, map, policy, sample);
    require(static_cast<bool>(result), "humanoid A->B retarget succeeds");
    require(result.value().receipt.mapped_joint_count == 3U,
        "receipt counts every mapped joint");
    require(result.value().receipt.unmapped_source_joints.empty(),
        "every source joint was mapped");
    require(result.value().receipt.unmapped_target_joints.size() == 1U &&
            result.value().receipt.unmapped_target_joints.front() ==
                kinematic::JointId{14U},
        "unmapped destination joints are reported, never faked");
    require(result.value().receipt.max_orientation_residual < 1e-9 &&
            result.value().receipt.max_position_residual < 1e-9,
        "round-trip residuals vanish for consistent rests");
    require(static_cast<bool>(result.value().receipt.validate()),
        "retarget receipt validates");

    // Rest-pose delta compensation: the 0.05 source lift scales by 1.5.
    const auto spine = result.value().pose.destination.locals.find(
        kinematic::JointId{12U});
    require(spine != result.value().pose.destination.locals.end(),
        "mapped destination joint receives a pose");
    require(near(spine->second.translation.y,
            0.15 + 0.05 * policy.translation_scale),
        "translation delta is scaled by rest proportions");

    // Deterministic receipts: identical inputs produce identical digests.
    const auto again = retarget_pose(source, destination, map, policy, sample);
    require(again && again.value().receipt.receipt_digest ==
            result.value().receipt.receipt_digest,
        "retarget receipts are deterministic");
}

void retarget_root_motion_proposal_never_mutates_source() {
    using namespace carto;
    using namespace carto::motion;
    auto source = correspondence_with_rests({{1U, {0.0, 0.0, 0.0}}});
    auto destination = correspondence_with_rests({{11U, {0.0, 0.0, 0.0}}});
    RetargetMap map;
    map.entries.push_back(JointMapEntry{kinematic::JointId{1U}, kinematic::JointId{11U}});

    PoseSample sample;
    sample.time = RationalTime{10, 30U};
    core::Transform root_pose;
    root_pose.translation = {1.0, 0.0, 0.0};
    sample.locals.emplace(kinematic::JointId{1U}, root_pose);

    RetargetPolicy proposal_policy;
    proposal_policy.root = kinematic::JointId{1U};
    proposal_policy.root_motion = character::RootMotionPolicy::proposal_only;

    const PoseSample original = sample;
    const auto proposed = retarget_pose(source, destination, map, proposal_policy, sample);
    require(static_cast<bool>(proposed), "proposal-only retarget succeeds");
    require(sample.locals.at(kinematic::JointId{1U}).translation.x == 1.0,
        "proposal-only retarget leaves the source sample untouched");
    require(proposed.value().pose.destination.locals.empty(),
        "proposal-only retarget keeps the root out of the destination pose");
    require(proposed.value().pose.root_proposal.has_value() &&
            proposed.value().pose.root_proposal->translations.size() == 1U &&
            near(proposed.value().pose.root_proposal->translations.front().x, 1.0),
        "the root motion is emitted as a proposal record");

    RetargetPolicy authored_policy = proposal_policy;
    authored_policy.root_motion = character::RootMotionPolicy::authored;
    const auto authored = retarget_pose(source, destination, map, authored_policy, sample);
    require(authored && !authored.value().pose.root_proposal.has_value() &&
            authored.value().pose.destination.locals.count(kinematic::JointId{11U}) == 1U,
        "authored policy ships the root in the destination pose");

    RetargetPolicy none_policy = proposal_policy;
    none_policy.root_motion = character::RootMotionPolicy::none;
    require(!none_policy.validate(),
        "a retarget root with policy none is rejected");
}

void retarget_map_fails_closed_on_duplicates() {
    using namespace carto;
    using namespace carto::motion;
    RetargetMap map;
    map.entries.push_back(JointMapEntry{kinematic::JointId{1U}, kinematic::JointId{11U}});
    map.entries.push_back(JointMapEntry{kinematic::JointId{1U}, kinematic::JointId{12U}});
    require(!map.validate(), "duplicate source joints fail closed");
    const std::string text = map.serialize();
    require(!RetargetMap::deserialize(
                "CARTO_RETARGET_MAP 1\nENTRIES 70000\n"),
        "oversized map counts fail closed");
    map.entries[1].source = kinematic::JointId{2U};
    const auto roundtrip = RetargetMap::deserialize(map.serialize());
    require(roundtrip && roundtrip.value().serialize() == map.serialize(),
        "retarget map codec round trips");
}

void mocap_ingest_refuses_defective_rights() {
    using namespace carto;
    using namespace carto::motion;

    RightsAttestation attestation;
    attestation.license_grants = {"license:authoring-preview", "license:performer-release"};
    attestation.attested_by = "rights-office";
    require(static_cast<bool>(attestation.validate()), "rights attestation validates");

    const auto ok = evaluate_rights(
        attestation, "authoring-preview", {"license:performer-release"});
    require(static_cast<bool>(ok), "granted rights evaluate clean");

    const auto missing = evaluate_rights(
        attestation, "public-market", {"license:market-redistribution"});
    require(!missing && missing.error().code == core::ErrorCode::unsupported,
        "missing grants are an explicit unsupported refusal");

    RightsAttestation defective;
    defective.attested_by = "rights-office";
    require(!defective.validate(), "empty grant set fails closed");
    const auto defective_eval = evaluate_rights(
        defective, "authoring-preview", {"license:performer-release"});
    require(!defective_eval, "defective rights refuse evaluation");

    RightsAttestation tag_only;
    tag_only.license_grants = {"tag:mocap"};
    tag_only.attested_by = "rights-office";
    const auto tag_eval = evaluate_rights(
        tag_only, "authoring-preview", {"license:performer-release"});
    require(!tag_eval, "a caller-supplied tag alone is never legal evidence");

    MocapIngestRecord record;
    record.rights = defective;
    record.timebase = convert_capture_timebase(30U, 1U).value();
    record.pins.push_back(SourcePin{
        "capture/day01/take03.bvh#frames[0-240]",
        digest_of("capture-bytes"), "carto-bvh-profile", "1.0.0"});
    require(!record.validate(), "ingest record with defective rights fails closed");
}

void capture_timebase_conversion_is_exact_rational() {
    using namespace carto;
    using namespace carto::motion;
    const auto ntsc = convert_capture_timebase(30000U, 1001U);
    require(static_cast<bool>(ntsc), "NTSC rational rate converts");
    require(ntsc.value().frame_period.tick == 1001 &&
            ntsc.value().frame_period.ticks_per_second == 30000U,
        "30000/1001 keeps its exact rational frame period");
    require(near(ntsc.value().frame_period_seconds, 1001.0 / 30000.0),
        "frame period seconds match the exact rational");
    require(ntsc.value().max_abs_error_seconds > 0.0,
        "frame period reporting carries an error bound");

    const auto film = convert_capture_timebase(24U, 1U);
    require(film && film.value().frame_period.tick == 1 &&
            film.value().frame_period.ticks_per_second == 24U,
        "integer frame rates convert exactly");

    require(!convert_capture_timebase(0U, 1U), "zero frame rates fail closed");
    const auto receipt_digest_a = character::canonical_digest(ntsc.value().frame_period);
    const auto receipt_digest_b = character::canonical_digest(ntsc.value().frame_period);
    require(receipt_digest_a && receipt_digest_b &&
            receipt_digest_a.value() == receipt_digest_b.value(),
        "timebase conversion evidence digests deterministically");
}

void ingest_record_round_trips_and_orders_pins() {
    using namespace carto;
    using namespace carto::motion;
    MocapIngestRecord record;
    // Digest order, not locator order, pins the record; build digests whose
    // order is explicit so the unsorted case is deterministic.
    const auto low = Sha256Digest::from_hex(
        "01" + std::string(62U, '0'));
    const auto high = Sha256Digest::from_hex(
        "02" + std::string(62U, '0'));
    require(low && high, "pin digests parse");
    record.pins.push_back(SourcePin{
        "a/take01.bvh", high.value(), "carto-bvh-profile", "1.0.0"});
    record.pins.push_back(SourcePin{
        "b/take02.bvh", low.value(), "carto-bvh-profile", "1.0.0"});
    record.rights.license_grants = {"license:authoring-preview"};
    record.rights.attested_by = "rights-office";
    record.timebase = convert_capture_timebase(60U, 1U).value();
    MocapJointTrack track;
    track.joint_name = "hips";
    track.sample_times = {RationalTime{0, 60U}, RationalTime{1, 60U}};
    track.sample_poses = {core::Transform::identity(), core::Transform::identity()};
    record.tracks.push_back(track);
    require(!record.validate(), "unsorted pins fail validation");
    std::sort(record.pins.begin(), record.pins.end());
    require(static_cast<bool>(record.validate()), "sorted pins validate");

    const std::string text = record.serialize();
    const auto parsed = MocapIngestRecord::deserialize(text);
    require(parsed && parsed.value().serialize() == text,
        "ingest record codec round trips");
    const auto digest_a = record.canonical_digest();
    const auto digest_b = parsed.value().canonical_digest();
    require(digest_a && digest_b && digest_a.value() == digest_b.value(),
        "ingest digests survive the codec");
    require(!MocapIngestRecord::deserialize("CARTO_MOCAP_INGEST 1\nPINS 999999\n"),
        "oversized ingest counts fail closed before allocation");
}

void performance_catalog_is_bounded_and_digest_bound() {
    using namespace carto;
    using namespace carto::motion;
    MocapIngestRecord record;
    record.pins.push_back(SourcePin{
        "a/take01.bvh", digest_of("aaa"), "carto-bvh-profile", "1.0.0"});
    record.rights.license_grants = {"license:authoring-preview"};
    record.rights.attested_by = "rights-office";
    record.timebase = convert_capture_timebase(30U, 1U).value();
    const auto ingest_digest = record.canonical_digest();
    require(static_cast<bool>(ingest_digest), "ingest record digests");

    PerformanceTakeCatalog catalog;
    PerformanceTake take;
    take.id = PerformanceTakeId{1U};
    take.take_name = "walk-loop";
    take.ingest_digest = ingest_digest.value();
    take.tags = {"locomotion", "humanoid"};
    require(static_cast<bool>(catalog.insert(std::move(take))),
        "catalog accepts a take");
    require(!catalog.insert(PerformanceTake{
                PerformanceTakeId{1U}, "dupe", core::Revision{1U},
                ingest_digest.value(), std::nullopt, {}}),
        "duplicate take identities fail closed");

    const auto digest_a = catalog.canonical_digest();
    const auto parsed = PerformanceTakeCatalog::deserialize(catalog.serialize());
    require(parsed && parsed.value().canonical_digest() &&
            digest_a.value() == parsed.value().canonical_digest().value(),
        "catalog digests survive the codec");
    const PerformanceTake* found = catalog.find(PerformanceTakeId{1U});
    require(found && found->ingest_digest == ingest_digest.value(),
        "catalog find returns the digest-bound take");
}

void determinism_holds_across_repeated_serialization() {
    using namespace carto::motion;
    KeyedCurve curve = two_key_curve(
        InterpolationKind::smoothstep, 0, 1.0, 60, 3.0);
    const auto first = curve.canonical_digest();
    const auto second = curve.canonical_digest();
    require(first && second && first.value() == second.value(),
        "canonical digests are stable across calls");
    const auto rescaled = rescale_timebase(curve, 60U);
    require(rescaled && rescaled.value().second.source_digest == first.value(),
        "rescale receipts pin the identical source digest");
}

} // namespace

carto::motion::KeyedCurve three_key_curve() {
    carto::motion::KeyedCurve curve = two_key_curve(
        carto::motion::InterpolationKind::cubic_bezier, 0, 0.0, 30, 3.0);
    curve.keys.push_back(carto::motion::CurveKey{RationalTime{60, 30U}, 1.0, 0.0, 0.0});
    require(static_cast<bool>(curve.validate()), "three key fixture validates");
    return curve;
}

void curve_edits_are_pure_receipted_and_collision_safe() {
    using namespace carto::motion;
    const KeyedCurve source = three_key_curve();

    auto inserted = insert_key(source, CurveKey{RationalTime{15, 30U}, 2.0, 0.0, 0.0});
    require(static_cast<bool>(inserted), "key inserts");
    require(inserted.value().curve.keys.size() == 4U &&
            inserted.value().curve.keys[1U].time.tick == 15,
        "inserted key lands in tick order");
    require(static_cast<bool>(inserted.value().receipt.validate()) &&
            !(inserted.value().receipt.source_digest == inserted.value().receipt.result_digest),
        "insert receipt binds distinct source and result");
    require(source.keys.size() == 3U, "the source curve is untouched");
    require(!insert_key(source, CurveKey{RationalTime{30, 30U}, 9.0, 0.0, 0.0}),
        "inserting onto an occupied tick refuses");
    auto replaced = insert_key(source, CurveKey{RationalTime{30, 30U}, 9.0, 0.0, 0.0}, true);
    require(static_cast<bool>(replaced) && replaced.value().curve.keys[1U].value == 9.0,
        "explicit replace overwrites the key");
    require(!insert_key(source, CurveKey{RationalTime{15, 24U}, 2.0, 0.0, 0.0}),
        "a key on another timebase refuses");

    auto deleted = delete_keys(source, {30});
    require(static_cast<bool>(deleted) && deleted.value().curve.keys.size() == 2U,
        "key deletes");
    require(!delete_keys(source, {0, 30, 60}), "deleting every key refuses");
    require(!delete_keys(source, {31}), "deleting a missing key refuses");
    require(!delete_keys(source, {30, 30}), "a repeated selection refuses");

    auto offset = offset_keys(source, {30}, 10);
    require(static_cast<bool>(offset) && offset.value().curve.keys[1U].time.tick == 40,
        "key offsets");
    require(!offset_keys(source, {30}, 30), "offsetting onto an unselected key refuses");
    auto block = offset_keys(source, {30, 60}, 30);
    require(static_cast<bool>(block) && block.value().curve.keys[2U].time.tick == 90,
        "a block offset may pass over its own vacated ticks");
    require(!offset_keys(source, {30}, 0), "a zero offset refuses");

    CurveKey sloped{RationalTime{30, 30U}, 3.0, 2.0, 4.0};
    auto with_tangents = insert_key(source, sloped, true);
    require(static_cast<bool>(with_tangents), "sloped key replaces");
    auto stretched = scale_keys(with_tangents.value().curve, {30, 60}, 0, 2U, 1U);
    require(static_cast<bool>(stretched), "keys retime");
    require(stretched.value().curve.keys[1U].time.tick == 60 &&
            stretched.value().curve.keys[2U].time.tick == 120,
        "retime scales ticks about the pivot");
    require(near(stretched.value().curve.keys[1U].tangent_in, 1.0) &&
            near(stretched.value().curve.keys[1U].tangent_out, 2.0),
        "retime divides tangents by the scale");
    require(!scale_keys(source, {30, 60}, 0, 1U, 100U),
        "a retime that collapses keys refuses");
    require(!scale_keys(source, {30}, 0, 0U, 1U), "a zero retime scale refuses");

    auto flat = set_tangents(with_tangents.value().curve, {30}, TangentMode::flat);
    require(static_cast<bool>(flat) && flat.value().curve.keys[1U].tangent_in == 0.0 &&
            flat.value().curve.keys[1U].tangent_out == 0.0,
        "flat tangents zero both slopes");
    auto linear = set_tangents(source, {30}, TangentMode::linear);
    require(static_cast<bool>(linear) && near(linear.value().curve.keys[1U].tangent_in, 3.0) &&
            near(linear.value().curve.keys[1U].tangent_out, -2.0),
        "linear tangents follow neighbour slopes in value per second");
    auto smooth = set_tangents(source, {0, 30, 60}, TangentMode::auto_smooth);
    require(static_cast<bool>(smooth), "auto tangents rewrite");
    require(smooth.value().curve.keys[0U].tangent_out == 0.0 &&
            smooth.value().curve.keys[2U].tangent_in == 0.0,
        "auto tangents keep end keys flat");
    require(near(smooth.value().curve.keys[1U].tangent_in, 0.5) &&
            near(smooth.value().curve.keys[1U].tangent_out, 0.5),
        "auto tangents use the previous-to-next slope");
    require(smooth.value().receipt.affected_key_count == 3U,
        "tangent receipt counts the selection");

    auto repeat = set_tangents(source, {0, 30, 60}, TangentMode::auto_smooth);
    require(static_cast<bool>(repeat) &&
            repeat.value().receipt.result_digest == smooth.value().receipt.result_digest,
        "curve edits are deterministic");
}

int main() {
    curve_sampling_is_exact_at_ticks_and_midpoints();
    curve_validation_is_monotonic_and_single_timebase();
    rescale_receipts_distinguish_exact_from_rounded();
    curve_codec_round_trips_and_is_deterministic();
    curve_edits_are_pure_receipted_and_collision_safe();
    layer_compositing_order_and_weights_are_deterministic();
    layer_windows_and_missing_targets_are_honest();
    retarget_maps_matching_joints_and_reports_residuals();
    retarget_root_motion_proposal_never_mutates_source();
    retarget_map_fails_closed_on_duplicates();
    mocap_ingest_refuses_defective_rights();
    capture_timebase_conversion_is_exact_rational();
    ingest_record_round_trips_and_orders_pins();
    performance_catalog_is_bounded_and_digest_bound();
    determinism_holds_across_repeated_serialization();
    std::cout << "motion tests passed\n";
    return EXIT_SUCCESS;
}
