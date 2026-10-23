#include <carto/deform/affine.hpp>
#include <carto/deform/corrective.hpp>
#include <carto/deform/delta_smooth.hpp>
#include <carto/deform/dual_quaternion.hpp>
#include <carto/deform/graph.hpp>
#include <carto/deform/morph.hpp>
#include <carto/deform/receipt.hpp>
#include <carto/deform/skin.hpp>

#include <carto/character/drivers.hpp>
#include <carto/character/morph.hpp>
#include <carto/kinematic/skin.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::assets::Sha256Digest;
using carto::core::Vec3d;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(double left, double right, double epsilon = 1e-12) {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= epsilon * scale;
}

bool near_vec(Vec3d left, Vec3d right, double epsilon = 1e-12) {
    return near(left.x, right.x, epsilon) && near(left.y, right.y, epsilon) &&
        near(left.z, right.z, epsilon);
}

Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

// ---------------------------------------------------------------------------
// Two-joint bend fixture, hand-computable.
//
// joint 1: rest identity, pose identity.
// joint 2: rest T(1,0,0), pose T(1,0,0) + Rz(90 deg) about the joint origin.
// ---------------------------------------------------------------------------

carto::kinematic::JointId kJoint1{1U};
carto::kinematic::JointId kJoint2{2U};

carto::core::Quaternion rotation_z(double degrees) {
    const double half = degrees * 3.14159265358979323846 / 360.0;
    return {0.0, 0.0, std::sin(half), std::cos(half)};
}

carto::deform::JointPoseSet bend_poses(double angle_degrees) {
    carto::deform::JointPoseSet poses;
    poses.world_rest[kJoint1] = carto::core::Transform::identity();
    poses.world_rest[kJoint2] = {{1.0, 0.0, 0.0}, carto::core::Quaternion::identity(), {1.0, 1.0, 1.0}};
    poses.world_pose[kJoint1] = carto::core::Transform::identity();
    poses.world_pose[kJoint2] = {{1.0, 0.0, 0.0}, rotation_z(angle_degrees), {1.0, 1.0, 1.0}};
    return poses;
}

std::vector<Vec3d> bend_positions() {
    // v0: rigid on joint 1; v1: rigid on joint 2 at the pivot; v2: 50/50 blend.
    return {{2.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {2.0, 0.0, 0.0}};
}

carto::kinematic::SkinBinding bend_binding(const Sha256Digest& mesh_digest) {
    carto::kinematic::SkinBinding binding;
    binding.mesh_asset = 42U;
    binding.mesh_digest = mesh_digest;
    binding.graph_digest = digest("kinematic-graph:v1");
    binding.influences = {
        {0U, {{kJoint1, 1.0}}},
        {1U, {{kJoint2, 1.0}}},
        {2U, {{kJoint1, 0.5}, {kJoint2, 0.5}}},
    };
    require(static_cast<bool>(binding.validate()), "bend binding validates");
    return binding;
}

void lbs_matches_hand_computed_positions() {
    static std::vector<Vec3d> positions = bend_positions();
    const Sha256Digest mesh_digest = carto::deform::positions_digest(positions);
    const carto::kinematic::SkinBinding binding = bend_binding(mesh_digest);
    carto::deform::JointPoseSet poses = bend_poses(90.0);
    const carto::deform::SkinEvaluationInput input{
        .bind_positions = positions,
        .mesh_content_digest = mesh_digest,
        .binding = &binding,
        .poses = &poses};

    const auto result = carto::deform::evaluate_lbs(input);
    require(static_cast<bool>(result), "LBS evaluates the bend fixture");
    require(result.value().positions.size() == 3U, "LBS returns one point per vertex");
    require(near_vec(result.value().positions[0], {2.0, 0.0, 0.0}),
        "joint-1 rigid vertex stays put");
    require(near_vec(result.value().positions[1], {1.0, 0.0, 0.0}),
        "joint-2 pivot vertex stays put");
    require(near_vec(result.value().positions[2], {1.5, 0.5, 0.0}),
        "blended vertex averages identity and Rz90-about-(1,0,0)");

    const auto dqs = carto::deform::evaluate_dqs(input);
    require(static_cast<bool>(dqs), "DQS evaluates the rigid bend fixture");
    const double s = std::sqrt(0.5);
    require(near_vec(dqs.value().positions[2], {1.0 + s, s, 0.0}, 1e-9),
        "DQS blends the 50/50 vertex as a 45 degree rotation about the pivot");

    const auto deviation = carto::deform::compare_skin_methods(input);
    require(static_cast<bool>(deviation), "skin method comparison evaluates");
    require(deviation.value().compared_points == 3U, "comparison covers every vertex");
    require(deviation.value().max_deviation > 0.0,
        "LBS and DQS disagree on the blended vertex (measured, not assumed)");
}

// ---------------------------------------------------------------------------
// Closed tube fixture for volume evidence: 5 rings of 8 points along x,
// triangle fans capping both ends.
// ---------------------------------------------------------------------------

struct TubeFixture {
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> triangles;
    Sha256Digest mesh_digest;
    carto::kinematic::SkinBinding binding;
    carto::deform::JointPoseSet poses;
};

TubeFixture make_tube(double angle_degrees) {
    constexpr std::uint32_t kSegments = 8U;
    constexpr std::uint32_t kRings = 5U;
    constexpr double kRadius = 0.25;
    TubeFixture tube;
    std::map<std::uint32_t, std::vector<carto::kinematic::JointInfluence>> influences;
    for (std::uint32_t ring = 0U; ring < kRings; ++ring) {
        const double x = 0.5 * static_cast<double>(ring);
        const double joint2_weight = static_cast<double>(ring) / 4.0;
        for (std::uint32_t segment = 0U; segment < kSegments; ++segment) {
            const double angle =
                2.0 * 3.14159265358979323846 * static_cast<double>(segment) /
                static_cast<double>(kSegments);
            tube.positions.push_back(
                {x, kRadius * std::cos(angle), kRadius * std::sin(angle)});
            const std::uint32_t index = ring * kSegments + segment;
            if (ring == 0U) {
                influences[index] = {{kJoint1, 1.0}};
            } else if (ring + 1U == kRings) {
                influences[index] = {{kJoint2, 1.0}};
            } else {
                influences[index] = {
                    {kJoint1, 1.0 - joint2_weight}, {kJoint2, joint2_weight}};
            }
        }
    }
    const std::uint32_t cap0_center = kRings * kSegments;
    const std::uint32_t cap1_center = kRings * kSegments + 1U;
    tube.positions.push_back({0.0, 0.0, 0.0});
    tube.positions.push_back({2.0, 0.0, 0.0});
    influences[cap0_center] = {{kJoint1, 1.0}};
    influences[cap1_center] = {{kJoint2, 1.0}};
    for (std::uint32_t ring = 0U; ring + 1U < kRings; ++ring) {
        for (std::uint32_t segment = 0U; segment < kSegments; ++segment) {
            const std::uint32_t next = (segment + 1U) % kSegments;
            const std::uint32_t a = ring * kSegments + segment;
            const std::uint32_t b = ring * kSegments + next;
            const std::uint32_t c = (ring + 1U) * kSegments + next;
            const std::uint32_t d = (ring + 1U) * kSegments + segment;
            tube.triangles.insert(tube.triangles.end(), {a, b, c, a, c, d});
        }
    }
    for (std::uint32_t segment = 0U; segment < kSegments; ++segment) {
        const std::uint32_t next = (segment + 1U) % kSegments;
        tube.triangles.insert(
            tube.triangles.end(), {cap0_center, next, segment});
        const std::uint32_t base = (kRings - 1U) * kSegments;
        tube.triangles.insert(
            tube.triangles.end(), {cap1_center, base + segment, base + next});
    }

    tube.mesh_digest = carto::deform::positions_digest(tube.positions);
    tube.binding.mesh_asset = 77U;
    tube.binding.mesh_digest = tube.mesh_digest;
    tube.binding.graph_digest = digest("tube-graph:v1");
    tube.binding.influences = std::move(influences);
    require(static_cast<bool>(tube.binding.validate()), "tube binding validates");
    tube.poses = bend_poses(angle_degrees);
    return tube;
}

double signed_volume(
    const std::vector<Vec3d>& positions,
    const std::vector<std::uint32_t>& triangles) {
    double volume = 0.0;
    for (std::size_t index = 0U; index + 2U < triangles.size(); index += 3U) {
        const Vec3d& a = positions[triangles[index]];
        const Vec3d& b = positions[triangles[index + 1U]];
        const Vec3d& c = positions[triangles[index + 2U]];
        volume += carto::core::dot(a, carto::core::cross(b, c)) / 6.0;
    }
    return volume;
}

void dqs_volume_evidence_beats_lbs_on_bend() {
    TubeFixture rest = make_tube(0.0);
    TubeFixture bent = make_tube(90.0);

    carto::deform::SkinEvaluationInput input{
        .bind_positions = bent.positions,
        .mesh_content_digest = bent.mesh_digest,
        .binding = &bent.binding,
        .poses = &bent.poses};
    const auto lbs = carto::deform::evaluate_lbs(input);
    const auto dqs = carto::deform::evaluate_dqs(input);
    require(static_cast<bool>(lbs) && static_cast<bool>(dqs),
        "both skin methods evaluate the bent tube");

    const double rest_volume = std::abs(signed_volume(rest.positions, rest.triangles));
    const double lbs_volume = std::abs(signed_volume(lbs.value().positions, bent.triangles));
    const double dqs_volume = std::abs(signed_volume(dqs.value().positions, bent.triangles));
    require(rest_volume > 0.0, "rest tube has positive volume");
    require(
        std::abs(dqs_volume - rest_volume) < std::abs(lbs_volume - rest_volume),
        "DQS preserves volume better than LBS on the 90 degree bend "
        "(measured evidence, not a fixed threshold)");

    const auto receipt = carto::deform::compare_skin_methods(input);
    require(receipt.value().max_deviation > 0.0,
        "receipt reports where the methods diverge");
}

void dqs_rejects_non_rigid_joints() {
    TubeFixture tube = make_tube(0.0);
    tube.poses.world_pose[kJoint2].scale = {2.0, 1.0, 1.0};
    carto::deform::SkinEvaluationInput input{
        .bind_positions = tube.positions,
        .mesh_content_digest = tube.mesh_digest,
        .binding = &tube.binding,
        .poses = &tube.poses};
    const auto dqs = carto::deform::evaluate_dqs(input);
    require(!dqs, "DQS refuses shear/scale joints");
    require(dqs.error().code == carto::core::ErrorCode::unsupported,
        "DQS refusal is an explicit unsupported, not a silent fallback");
    const auto lbs = carto::deform::evaluate_lbs(input);
    require(static_cast<bool>(lbs),
        "LBS honestly evaluates the declared affine subset");
}

void stale_mesh_digest_fails_closed() {
    static std::vector<Vec3d> positions = bend_positions();
    const carto::kinematic::SkinBinding binding =
        bend_binding(carto::deform::positions_digest(positions));
    carto::deform::JointPoseSet poses = bend_poses(90.0);
    carto::deform::SkinEvaluationInput input{
        .bind_positions = positions,
        .mesh_content_digest = digest("some-other-mesh"),
        .binding = &binding,
        .poses = &poses};
    const auto result = carto::deform::evaluate_lbs(input);
    require(!result, "stale mesh digest refuses evaluation");
    require(result.error().code == carto::core::ErrorCode::stale_data,
        "stale refusal carries the stale_data code");
}

// ---------------------------------------------------------------------------
// Morph target application.
// ---------------------------------------------------------------------------

carto::character::MorphTarget make_morph_target(
    const Sha256Digest& mesh_digest,
    const std::vector<std::uint8_t>& blob,
    const Sha256Digest& correspondence) {
    carto::character::MorphTarget target;
    target.id = carto::character::MorphTargetId{5U};
    target.source_mesh_asset = 9U;
    target.source_mesh_digest = mesh_digest;
    target.displaced_point_count = 1U;
    target.delta_blob_digest = carto::assets::sha256(blob);
    target.delta_blob_bytes = static_cast<std::uint64_t>(blob.size());
    target.topology_correspondence_digest = correspondence;
    target.tangent_in = carto::character::TangentPolicy::source_preserved;
    target.tangent_out = carto::character::TangentPolicy::recompute;
    target.precision_bound = 1.0;
    require(static_cast<bool>(target.validate()), "morph target record validates");
    return target;
}

void morph_delta_application_is_targeted_and_bounded() {
    const std::vector<Vec3d> positions = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
                                          {0.0, 1.0, 0.0}, {1.0, 1.0, 0.0}};
    const Sha256Digest mesh_digest = carto::deform::positions_digest(positions);
    const Sha256Digest correspondence = digest("tube-correspondence:v1");

    const auto blob = carto::deform::encode_delta_blob(
        {{2U, {0.0, 0.5, 0.0}}});
    require(static_cast<bool>(blob), "single-point delta blob encodes");
    const carto::character::MorphTarget target =
        make_morph_target(mesh_digest, blob.value(), correspondence);

    carto::deform::MorphApplicationInput input{
        positions, &target, blob.value(), mesh_digest, correspondence, 1.0};
    const auto applied = carto::deform::apply_morph_target(input);
    require(static_cast<bool>(applied), "morph applies at full weight");
    require(near_vec(applied.value().positions[2], {0.0, 1.5, 0.0}),
        "targeted point receives the full delta");
    require(near_vec(applied.value().positions[0], positions[0]) &&
            near_vec(applied.value().positions[1], positions[1]) &&
            near_vec(applied.value().positions[3], positions[3]),
        "untargeted points are bit-stable");
    require(applied.value().tangents_require_recompute,
        "recompute tangent policy is honored as an explicit demand");

    input.weight = 0.25;
    const auto partial = carto::deform::apply_morph_target(input);
    require(static_cast<bool>(partial), "morph applies at partial weight");
    require(near_vec(partial.value().positions[2], {0.0, 1.125, 0.0}),
        "partial weight scales the delta exactly");

    input.weight = 1.5;
    require(!carto::deform::apply_morph_target(input),
        "weight above 1 is refused");

    input.weight = 1.0;
    input.topology_correspondence_digest = digest("correspondence:CHANGED");
    const auto stale_correspondence = carto::deform::apply_morph_target(input);
    require(!stale_correspondence, "stale correspondence digest is refused");
    require(stale_correspondence.error().code == carto::core::ErrorCode::stale_data,
        "correspondence refusal carries the stale_data code");

    input.topology_correspondence_digest = correspondence;
    input.mesh_content_digest = digest("mesh:CHANGED");
    const auto stale_mesh = carto::deform::apply_morph_target(input);
    require(!stale_mesh && stale_mesh.error().code == carto::core::ErrorCode::stale_data,
        "stale source mesh digest is refused");

    input.mesh_content_digest = mesh_digest;
    std::vector<std::uint8_t> tampered = blob.value();
    tampered.back() =
        static_cast<std::uint8_t>(tampered.back() ^ static_cast<std::uint8_t>(0x01U));
    input.delta_blob = tampered;
    require(!carto::deform::apply_morph_target(input),
        "tampered delta blob fails the digest check");

    input.delta_blob = blob.value();
    const auto oversized = carto::deform::encode_delta_blob({{2U, {0.0, 5.0, 0.0}}});
    require(static_cast<bool>(oversized), "oversized delta encodes");
    carto::character::MorphTarget tight = target;
    tight.precision_bound = 1.0;
    input.target = &tight;
    std::vector<std::uint8_t> big_blob = oversized.value();
    tight.delta_blob_digest = carto::assets::sha256(big_blob);
    tight.delta_blob_bytes = static_cast<std::uint64_t>(big_blob.size());
    input.delta_blob = big_blob;
    require(!carto::deform::apply_morph_target(input),
        "delta beyond the declared precision bound is refused");

    require(!carto::deform::encode_delta_blob({{0U, {0.0, 0.0, 0.0}}, {0U, {1.0, 0.0, 0.0}}}),
        "duplicate delta indexes fail closed at encode");
    require(!carto::deform::encode_delta_blob(
                {{0U, {std::numeric_limits<double>::infinity(), 0.0, 0.0}}}),
        "non-finite deltas fail closed at encode");
}

// ---------------------------------------------------------------------------
// Corrective driver evaluation.
// ---------------------------------------------------------------------------

carto::character::CorrectiveDriver make_driver(
    carto::character::CorrectiveDriverId id,
    std::uint32_t order,
    carto::character::DriverInterpolation interpolation,
    std::vector<carto::character::DriverInput> inputs,
    std::vector<double> domain_min,
    std::vector<double> domain_max) {
    carto::character::CorrectiveDriver driver;
    driver.id = id;
    driver.order_index = order;
    driver.interpolation = interpolation;
    driver.inputs = std::move(inputs);
    driver.domain_minimum = std::move(domain_min);
    driver.domain_maximum = std::move(domain_max);
    require(static_cast<bool>(driver.validate()), "corrective driver record validates");
    return driver;
}

carto::character::DriverInput control_input(carto::character::ControlId control) {
    carto::character::DriverInput input;
    input.kind = carto::character::DriverInput::Kind::control;
    input.control = control;
    return input;
}

void corrective_driver_interpolates_over_domain_edges() {
    const carto::character::ControlId control{7U};
    const auto driver = make_driver(
        carto::character::CorrectiveDriverId{1U}, 0U,
        carto::character::DriverInterpolation::linear, {control_input(control)},
        {-1.0}, {2.0});

    carto::deform::DriverChannelValues channels;
    channels.controls[control] = -1.0;
    const auto at_min = carto::deform::evaluate_driver(driver, channels);
    require(static_cast<bool>(at_min) && near(at_min.value().weight, 0.0),
        "driver weight is exactly 0 at the domain minimum");

    channels.controls[control] = 2.0;
    const auto at_max = carto::deform::evaluate_driver(driver, channels);
    require(static_cast<bool>(at_max) && near(at_max.value().weight, 1.0),
        "driver weight is exactly 1 at the domain maximum");

    channels.controls[control] = 0.5;
    const auto at_mid = carto::deform::evaluate_driver(driver, channels);
    require(static_cast<bool>(at_mid) && near(at_mid.value().weight, 0.5),
        "linear interpolation hits the midpoint");

    channels.controls[control] = 5.0;
    const auto clamped = carto::deform::evaluate_driver(driver, channels);
    require(static_cast<bool>(clamped) && near(clamped.value().weight, 1.0) &&
            clamped.value().clamped_inputs == 1U,
        "out-of-domain input clamps to the domain and reports the clamp");

    const auto smooth = make_driver(
        carto::character::CorrectiveDriverId{2U}, 1U,
        carto::character::DriverInterpolation::smoothstep, {control_input(control)},
        {0.0}, {1.0});
    channels.controls[control] = 0.5;
    const auto smooth_mid = carto::deform::evaluate_driver(smooth, channels);
    require(static_cast<bool>(smooth_mid) && near(smooth_mid.value().weight, 0.5),
        "smoothstep interpolation passes through 0.5 at t = 0.5");

    channels.controls.erase(control);
    require(!carto::deform::evaluate_driver(driver, channels),
        "missing control channel fails closed");

    carto::character::DriverInput self_reference;
    self_reference.kind = carto::character::DriverInput::Kind::driver;
    self_reference.driver = carto::character::CorrectiveDriverId{9U};
    carto::character::CorrectiveDriver selfish;
    selfish.id = carto::character::CorrectiveDriverId{9U};
    selfish.order_index = 0U;
    selfish.interpolation = carto::character::DriverInterpolation::linear;
    selfish.inputs = {self_reference};
    selfish.domain_minimum = {0.0};
    selfish.domain_maximum = {1.0};
    require(!carto::deform::evaluate_driver(selfish, channels),
        "self-referencing driver is rejected");
}

void corrective_stack_orders_and_chains_deterministically() {
    const carto::character::ControlId control{3U};
    const auto primary = make_driver(
        carto::character::CorrectiveDriverId{1U}, 0U,
        carto::character::DriverInterpolation::linear, {control_input(control)},
        {0.0}, {1.0});
    carto::character::DriverInput chained;
    chained.kind = carto::character::DriverInput::Kind::driver;
    chained.driver = carto::character::CorrectiveDriverId{1U};
    const auto secondary = make_driver(
        carto::character::CorrectiveDriverId{2U}, 1U,
        carto::character::DriverInterpolation::linear, {chained}, {0.0}, {1.0});

    std::vector<const carto::character::CorrectiveDriver*> stack{&secondary, &primary};
    carto::deform::DriverChannelValues channels;
    channels.controls[control] = 0.25;
    const auto evaluated = carto::deform::evaluate_driver_stack(stack, channels);
    require(static_cast<bool>(evaluated), "stack evaluates in declared order");
    require(evaluated.value().count(carto::character::CorrectiveDriverId{1U}) == 1U &&
            evaluated.value().count(carto::character::CorrectiveDriverId{2U}) == 1U,
        "stack returns every driver result");
    require(near(evaluated.value().at(carto::character::CorrectiveDriverId{1U}).weight, 0.25) &&
            near(evaluated.value().at(carto::character::CorrectiveDriverId{2U}).weight, 0.25),
        "chained driver resolves against the earlier result in the same pass");

    const auto duplicate_order = make_driver(
        carto::character::CorrectiveDriverId{3U}, 0U,
        carto::character::DriverInterpolation::linear, {control_input(control)},
        {0.0}, {1.0});
    std::vector<const carto::character::CorrectiveDriver*> bad{&primary, &duplicate_order};
    require(!carto::deform::evaluate_driver_stack(bad, channels),
        "duplicate order indexes refuse the stack");
}

// ---------------------------------------------------------------------------
// Deformation graph: ordered stack, receipts, determinism.
// ---------------------------------------------------------------------------

struct MorphFixture {
    std::vector<Vec3d> positions;
    Sha256Digest mesh_digest;
    Sha256Digest correspondence;
    std::vector<std::uint8_t> blob;
    carto::character::MorphTarget target;
};

MorphFixture make_morph_fixture() {
    MorphFixture fixture;
    fixture.positions = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
                         {1.0, 1.0, 0.0}, {2.0, 0.0, 0.0}};
    fixture.mesh_digest = carto::deform::positions_digest(fixture.positions);
    fixture.correspondence = digest("morph-correspondence:v1");
    const auto blob = carto::deform::encode_delta_blob({{4U, {0.0, 0.0, 1.0}}});
    require(static_cast<bool>(blob), "graph morph blob encodes");
    fixture.blob = blob.value();
    fixture.target = make_morph_target(
        fixture.mesh_digest, fixture.blob, fixture.correspondence);
    return fixture;
}

carto::deform::DeformationGraph graph_with(
    std::initializer_list<carto::deform::DeformationStage> stages) {
    carto::deform::DeformationGraph graph;
    graph.stages.assign(stages.begin(), stages.end());
    return graph;
}

void graph_evaluation_is_ordered_deterministic_and_receipted() {
    MorphFixture fixture = make_morph_fixture();
    carto::kinematic::SkinBinding binding;
    binding.mesh_asset = 42U;
    binding.mesh_digest = fixture.mesh_digest;
    binding.graph_digest = digest("kinematic-graph:v1");
    binding.influences = {
        {0U, {{kJoint1, 1.0}}},
        {1U, {{kJoint2, 1.0}}},
        {2U, {{kJoint1, 0.5}, {kJoint2, 0.5}}},
        {4U, {{kJoint2, 1.0}}},
    };
    require(static_cast<bool>(binding.validate()), "graph binding validates");
    carto::deform::JointPoseSet poses = bend_poses(90.0);

    carto::deform::DeformationStage morph;
    morph.kind = carto::deform::DeformationStage::Kind::morph;
    morph.space = carto::deform::MorphSpace::bind_pre_skin;
    morph.morph_target = fixture.target.id;
    morph.morph_weight = 1.0;
    carto::deform::DeformationStage skin;
    skin.kind = carto::deform::DeformationStage::Kind::skin;
    skin.method = carto::deform::SkinMethod::linear_blend;
    const carto::deform::DeformationGraph graph =
        graph_with({morph, skin});
    require(static_cast<bool>(graph.validate()), "morph-then-skin graph validates");

    carto::deform::DeformationEvaluationInput input;
    input.bind_positions = fixture.positions;
    input.mesh_content_digest = fixture.mesh_digest;
    input.binding = &binding;
    input.poses = &poses;
    input.morphs[fixture.target.id] = carto::deform::MorphSource{
        &fixture.target, fixture.blob, fixture.correspondence};

    const auto first = carto::deform::evaluate(graph, input);
    require(static_cast<bool>(first), "graph evaluates");
    require(first.value().receipt.stages.size() == 2U,
        "receipt carries one entry per stage");
    require(first.value().receipt.stages[0U].label == "morph:5",
        "morph stage receipt label is deterministic");
    require(first.value().receipt.stages[1U].label == "skin:linear_blend",
        "skin stage receipt label is deterministic");
    require(!first.value().receipt.stages[0U].policy_digest.is_zero() &&
            !first.value().receipt.stages[1U].policy_digest.is_zero(),
        "every stage records its policy digest");
    require(!first.value().receipt.has_reference_deviation,
        "no deviation is claimed without a reference");

    // Morph delta moved vertex 4 from (2,0,0) to (2,0,1) before skinning; it
    // is fully weighted to joint 2, so it lands at (1,0,0) + Rz90 * (1,0,1)
    // = (1, 1, 1).
    require(near_vec(first.value().positions[4], {1.0, 1.0, 1.0}),
        "pre-skin morph participates in skinning");

    const auto second = carto::deform::evaluate(graph, input);
    require(static_cast<bool>(second), "graph re-evaluates");
    require(first.value().positions.size() == second.value().positions.size(),
        "deterministic evaluation preserves the point count");
    for (std::size_t index = 0U; index < first.value().positions.size(); ++index) {
        require(first.value().positions[index].x == second.value().positions[index].x &&
                first.value().positions[index].y == second.value().positions[index].y &&
                first.value().positions[index].z == second.value().positions[index].z,
            "evaluation is position-deterministic");
    }
    require(
        first.value().receipt.stages[0U].output_digest ==
            second.value().receipt.stages[0U].output_digest &&
            first.value().receipt.stages[1U].output_digest ==
                second.value().receipt.stages[1U].output_digest,
        "stage digests are deterministic across evaluations");

    input.reference_positions = first.value().positions;
    const auto against_self = carto::deform::evaluate(graph, input);
    require(against_self.value().receipt.has_reference_deviation &&
            near(against_self.value().receipt.reference_deviation.max_deviation, 0.0),
        "reference deviation is measured and zero against an identical reference");
    std::vector<Vec3d> shifted = first.value().positions;
    shifted[0U].x += 0.5;
    input.reference_positions = shifted;
    const auto against_shifted = carto::deform::evaluate(graph, input);
    require(near(against_shifted.value().receipt.reference_deviation.max_deviation, 0.5),
        "deviation receipt reports the maximum point distance");

    // Corrective driver modulation: driver weight 0 must mute the morph.
    const carto::character::ControlId control{11U};
    const auto driver = make_driver(
        carto::character::CorrectiveDriverId{4U}, 0U,
        carto::character::DriverInterpolation::linear, {control_input(control)},
        {0.0}, {1.0});
    carto::deform::DeformationStage driven = morph;
    driven.corrective_driver = driver.id;
    const carto::deform::DeformationGraph driven_graph =
        graph_with({driven, skin});
    carto::deform::DeformationEvaluationInput driven_input = input;
    driven_input.reference_positions = {};
    const std::array<const carto::character::CorrectiveDriver*, 1U> driver_stack{
        &driver};
    driven_input.corrective_drivers =
        std::span<const carto::character::CorrectiveDriver* const>(
            driver_stack.data(), driver_stack.size());
    driven_input.driver_channels.controls[control] = 0.0;
    const auto muted = carto::deform::evaluate(driven_graph, driven_input);
    require(static_cast<bool>(muted), "driver-modulated graph evaluates");
    require(near_vec(muted.value().positions[4], {1.0, 1.0, 0.0}),
        "driver weight 0 mutes the corrective morph stage");
    driven_input.driver_channels.controls[control] = 1.0;
    const auto full = carto::deform::evaluate(driven_graph, driven_input);
    require(near_vec(full.value().positions[4], {1.0, 1.0, 1.0}),
        "driver weight 1 applies the corrective morph stage fully");

    // Ordering and honesty checks.
    carto::deform::DeformationStage post_skin_morph = morph;
    post_skin_morph.space = carto::deform::MorphSpace::post_skin_model;
    const carto::deform::DeformationGraph post_skin =
        graph_with({skin, post_skin_morph});
    require(static_cast<bool>(post_skin.validate()),
        "explicitly declared post-skin morph order validates");
    const carto::deform::DeformationGraph bad_order =
        graph_with({skin, morph});
    require(!bad_order.validate(),
        "bind-space morph placed after skin is a validation failure");

    carto::deform::DeformationGraph double_skin = graph_with({skin, skin});
    require(!double_skin.validate(), "two skin stages are rejected");

    carto::deform::DeformationEvaluationInput stale_input = driven_input;
    stale_input.morphs[fixture.target.id].topology_correspondence_digest =
        digest("correspondence:STALE");
    require(!carto::deform::evaluate(driven_graph, stale_input),
        "stale correspondence inside graph evaluation fails closed");
}

} // namespace

struct BumpyGrid {
    std::vector<Vec3d> positions;
    std::vector<std::uint32_t> triangles;
};

BumpyGrid bumpy_grid() {
    constexpr std::uint32_t side = 6U;
    BumpyGrid grid;
    for (std::uint32_t row = 0U; row < side; ++row) {
        for (std::uint32_t column = 0U; column < side; ++column) {
            const double x = static_cast<double>(column) * 0.2;
            const double y = static_cast<double>(row) * 0.2;
            grid.positions.push_back({x, y, 0.1 * std::sin(3.0 * x) * std::cos(2.0 * y)});
        }
    }
    for (std::uint32_t row = 0U; row + 1U < side; ++row) {
        for (std::uint32_t column = 0U; column + 1U < side; ++column) {
            const std::uint32_t a = row * side + column;
            const std::uint32_t b = a + 1U;
            const std::uint32_t c = a + side;
            const std::uint32_t d = c + 1U;
            grid.triangles.insert(grid.triangles.end(), {a, b, d, a, d, c});
        }
    }
    return grid;
}

void delta_smooth_is_exact_at_rest_equivariant_and_corrective() {
    const BumpyGrid grid = bumpy_grid();
    carto::deform::DeltaSmoothSettings settings;
    auto built = carto::deform::DeltaSmoothBinding::build(grid.positions, grid.triangles, settings);
    require(static_cast<bool>(built), "delta smooth binds");
    const auto& binding = built.value();

    auto rest = binding.apply(grid.positions);
    require(static_cast<bool>(rest), "delta smooth applies at rest");
    for (std::size_t index = 0U; index < grid.positions.size(); ++index) {
        require(near_vec(rest.value().positions[index], grid.positions[index], 1e-9),
            "delta smooth reproduces the bind mesh at rest");
    }
    require(rest.value().max_correction < 1e-9, "rest correction is zero");

    // Rigid motion of the whole mesh is reproduced exactly.
    const double angle = 0.7;
    const Vec3d offset{0.3, -1.2, 2.0};
    std::vector<Vec3d> moved;
    for (const Vec3d& p : grid.positions) {
        moved.push_back(Vec3d{
            std::cos(angle) * p.x - std::sin(angle) * p.z,
            p.y,
            std::sin(angle) * p.x + std::cos(angle) * p.z} + offset);
    }
    auto rigid = binding.apply(moved);
    require(static_cast<bool>(rigid), "delta smooth applies under rigid motion");
    for (std::size_t index = 0U; index < moved.size(); ++index) {
        require(near_vec(rigid.value().positions[index], moved[index], 1e-9),
            "delta smooth is rigid-motion equivariant");
    }

    // A skinning spike on one interior vertex is pulled back toward the
    // authored surface and the correction is measured.
    const std::size_t spike = 2U * 6U + 2U;
    std::vector<Vec3d> spiked = grid.positions;
    spiked[spike].z += 0.3;
    auto corrected = binding.apply(spiked);
    require(static_cast<bool>(corrected), "delta smooth corrects a spike");
    require((corrected.value().positions[spike] - grid.positions[spike]).length() < 0.3,
        "corrected spike lies closer to the authored surface");
    require(corrected.value().max_correction > 0.0 &&
            corrected.value().mean_correction > 0.0,
        "spike correction is measured");

    auto again = carto::deform::DeltaSmoothBinding::build(grid.positions, grid.triangles, settings);
    require(static_cast<bool>(again) && again.value().digest() == binding.digest(),
        "delta smooth binding digest is deterministic");
    carto::deform::DeltaSmoothSettings coarser = settings;
    coarser.iterations = 3U;
    auto other = carto::deform::DeltaSmoothBinding::build(grid.positions, grid.triangles, coarser);
    require(static_cast<bool>(other) && !(other.value().digest() == binding.digest()),
        "delta smooth digest binds its settings");

    // An isolated vertex has no frame and passes through unchanged.
    std::vector<Vec3d> with_loose = grid.positions;
    with_loose.push_back({5.0, 5.0, 5.0});
    auto loose = carto::deform::DeltaSmoothBinding::build(with_loose, grid.triangles, settings);
    require(static_cast<bool>(loose), "delta smooth binds a mesh with a loose vertex");
    std::vector<Vec3d> loose_moved = with_loose;
    loose_moved.back() = {6.0, 5.0, 5.0};
    auto loose_applied = loose.value().apply(loose_moved);
    require(static_cast<bool>(loose_applied) &&
            near_vec(loose_applied.value().positions.back(), loose_moved.back()),
        "a loose vertex passes through");

    require(!binding.apply(std::vector<Vec3d>(3U)), "a size mismatch refuses");
    std::vector<Vec3d> poisoned = grid.positions;
    poisoned[0U].x = std::numeric_limits<double>::quiet_NaN();
    require(!binding.apply(poisoned), "non-finite deformed input refuses");
    std::vector<Vec3d> collapsed(grid.positions.size(), Vec3d{1.0, 1.0, 1.0});
    require(!binding.apply(collapsed), "a collapsed deformation refuses");

    carto::deform::DeltaSmoothSettings hostile = settings;
    hostile.iterations = 0U;
    require(!carto::deform::DeltaSmoothBinding::build(grid.positions, grid.triangles, hostile),
        "zero iterations refuse");
    hostile = settings;
    hostile.step = 1.5;
    require(!carto::deform::DeltaSmoothBinding::build(grid.positions, grid.triangles, hostile),
        "an over-relaxed step refuses");
    std::vector<std::uint32_t> bad = grid.triangles;
    bad[0U] = 999U;
    require(!carto::deform::DeltaSmoothBinding::build(grid.positions, bad, settings),
        "an out-of-range triangle refuses");
    bad = grid.triangles;
    bad[1U] = bad[0U];
    require(!carto::deform::DeltaSmoothBinding::build(grid.positions, bad, settings),
        "a repeated triangle corner refuses");
}

int main() {
    lbs_matches_hand_computed_positions();
    dqs_volume_evidence_beats_lbs_on_bend();
    dqs_rejects_non_rigid_joints();
    stale_mesh_digest_fails_closed();
    morph_delta_application_is_targeted_and_bounded();
    corrective_driver_interpolates_over_domain_edges();
    corrective_stack_orders_and_chains_deterministically();
    graph_evaluation_is_ordered_deterministic_and_receipted();
    delta_smooth_is_exact_at_rest_equivariant_and_corrective();
    std::cout << "deform tests passed\n";
    return EXIT_SUCCESS;
}
