#include <carto/condition/sweep_endpoint.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* expression) {
    if (!condition) throw std::runtime_error(expression);
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

using carto::condition::SweepEndpointPortBinding;
using carto::condition::SweepEndpointSide;
using carto::connection::ConnectionPort;
using carto::connection::PortCompatibilityTag;
using carto::geometry::SweepSource;

carto::geometry::CurvePath straight_centerline() {
    carto::geometry::CurvePath path;
    path.source_identity = "route.centerline";
    path.source_revision = carto::core::Revision{3U};
    path.segments.push_back({
        {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0 / 3.0},
        {0.0, 0.0, 2.0 / 3.0}, {0.0, 0.0, 1.0}});
    REQUIRE(path.validate());
    return path;
}

SweepSource sweep_source(std::string identity, std::uint64_t revision = 1U) {
    SweepSource source;
    source.source_identity = std::move(identity);
    source.source_revision = carto::core::Revision{revision};
    source.centerline_identity = "route.centerline";
    source.centerline_revision = carto::core::Revision{3U};
    source.profile = {{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    REQUIRE(source.validate());
    return source;
}

ConnectionPort port(
    std::uint64_t id,
    std::uint64_t owner,
    PortCompatibilityTag tag,
    std::vector<std::string> required = {},
    std::vector<std::string> offered = {}) {
    ConnectionPort result;
    result.id = carto::connection::PortId{id};
    result.owner = carto::scene::ObjectId{owner};
    result.authoring.kind = carto::world::AuthoringBindingKind::scene_object;
    result.authoring.identity = owner;
    result.local_frame.frame_identity = "sweep-end-" + std::to_string(id);
    result.local_frame.coordinate_convention = "carto.core/v1";
    result.compatibility_tags.push_back(std::move(tag));
    result.capability_contract.required = std::move(required);
    result.capability_contract.offered = std::move(offered);
    REQUIRE(result.validate());
    return result;
}

SweepEndpointPortBinding binding(
    const SweepSource& source,
    SweepEndpointSide side,
    std::uint64_t port_id) {
    return {source.source_identity, source.source_revision, side,
        carto::connection::PortId{port_id}};
}

void compatible_endpoint_pair_returns_source_bound_deterministic_evidence() {
    using carto::connection::capabilities::fluid_transfer;
    const auto first_source = sweep_source("conduit.water.main");
    const auto second_source = sweep_source("conduit.water.branch", 4U);
    const auto first_port = port(11U, 101U,
        carto::connection::compatibility::utility::water_25mm,
        {std::string{fluid_transfer}});
    const auto second_port = port(12U, 102U,
        carto::connection::compatibility::utility::water_25mm,
        {}, {std::string{fluid_transfer}});
    const auto first_binding = binding(first_source, SweepEndpointSide::end, 11U);
    const auto second_binding = binding(second_source, SweepEndpointSide::start, 12U);

    const std::string first_source_before = first_source.serialize();
    const std::string second_source_before = second_source.serialize();
    const std::string first_port_before = first_port.serialize();
    const std::string second_port_before = second_port.serialize();
    const auto evidence = carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, second_port);
    REQUIRE(evidence);
    REQUIRE(evidence.value().validate());
    REQUIRE(evidence.value().first.sweep_source_identity == first_source.source_identity);
    REQUIRE(evidence.value().first.side == SweepEndpointSide::end);
    REQUIRE(evidence.value().second.side == SweepEndpointSide::start);
    REQUIRE(!evidence.value().first_sweep_digest.is_zero());
    REQUIRE(!evidence.value().second_sweep_digest.is_zero());
    const auto first_port_digest = first_port.canonical_digest();
    const auto second_port_digest = second_port.canonical_digest();
    REQUIRE(first_port_digest);
    REQUIRE(second_port_digest);
    REQUIRE(evidence.value().first_port_digest == first_port_digest.value());
    REQUIRE(evidence.value().second_port_digest == second_port_digest.value());
    REQUIRE(evidence.value().port_compatibility.first_satisfied_requirements ==
        std::vector<std::string>{std::string{fluid_transfer}});

    const auto repeated = carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, second_port);
    REQUIRE(repeated);
    REQUIRE(repeated.value().serialize() == evidence.value().serialize());
    REQUIRE(repeated.value().canonical_digest().value() ==
        evidence.value().canonical_digest().value());
    auto hollow_source = first_source;
    hollow_source.inner_profile_scale = 0.5;
    const auto hollow_evidence = carto::condition::check_sweep_endpoint_compatibility(
        hollow_source, first_binding, first_port,
        second_source, second_binding, second_port);
    REQUIRE(hollow_evidence && hollow_evidence.value().validate());
    REQUIRE(hollow_evidence.value().first_sweep_digest != evidence.value().first_sweep_digest);
    REQUIRE(hollow_evidence.value().second_sweep_digest == evidence.value().second_sweep_digest);
    REQUIRE(hollow_evidence.value().first_port_digest == evidence.value().first_port_digest);
    REQUIRE(first_source.serialize() == first_source_before);
    REQUIRE(second_source.serialize() == second_source_before);
    REQUIRE(first_port.serialize() == first_port_before);
    REQUIRE(second_port.serialize() == second_port_before);
}

void stale_or_mismatched_bindings_refuse_before_compatibility() {
    const auto source = sweep_source("conduit.stale");
    const auto other = sweep_source("conduit.other");
    const auto first_port = port(21U, 201U,
        carto::connection::compatibility::utility::water_25mm);
    const auto second_port = port(22U, 202U,
        carto::connection::compatibility::utility::water_25mm);

    auto stale = binding(source, SweepEndpointSide::start, 21U);
    stale.sweep_source_revision = carto::core::Revision{2U};
    const auto stale_result = carto::condition::check_sweep_endpoint_compatibility(
        source, stale, first_port,
        other, binding(other, SweepEndpointSide::end, 22U), second_port);
    REQUIRE(!stale_result);
    REQUIRE(stale_result.error().code == carto::core::ErrorCode::stale_data);

    auto wrong_port = binding(source, SweepEndpointSide::start, 999U);
    const auto wrong_port_result = carto::condition::check_sweep_endpoint_compatibility(
        source, wrong_port, first_port,
        other, binding(other, SweepEndpointSide::end, 22U), second_port);
    REQUIRE(!wrong_port_result);
    REQUIRE(wrong_port_result.error().code == carto::core::ErrorCode::stale_data);

    auto invalid_side = binding(source, SweepEndpointSide::start, 21U);
    invalid_side.side = static_cast<SweepEndpointSide>(255U);
    const auto invalid_side_result = carto::condition::check_sweep_endpoint_compatibility(
        source, invalid_side, first_port,
        other, binding(other, SweepEndpointSide::end, 22U), second_port);
    REQUIRE(!invalid_side_result);
}

void incompatible_and_duplicate_endpoint_pairs_fail_closed() {
    const auto first_source = sweep_source("conduit.first");
    const auto second_source = sweep_source("conduit.second");
    const auto water_port = port(31U, 301U,
        carto::connection::compatibility::utility::water_25mm);
    const auto electrical_port = port(32U, 302U,
        carto::connection::compatibility::utility::electrical_16a);

    const auto incompatible = carto::condition::check_sweep_endpoint_compatibility(
        first_source, binding(first_source, SweepEndpointSide::end, 31U), water_port,
        second_source, binding(second_source, SweepEndpointSide::start, 32U), electrical_port);
    REQUIRE(!incompatible);

    auto duplicate_port = port(31U, 303U,
        carto::connection::compatibility::utility::water_25mm);
    const auto repeated_port = carto::condition::check_sweep_endpoint_compatibility(
        first_source, binding(first_source, SweepEndpointSide::end, 31U), water_port,
        second_source, binding(second_source, SweepEndpointSide::start, 31U), duplicate_port);
    REQUIRE(!repeated_port);

    const auto repeated_endpoint = carto::condition::check_sweep_endpoint_compatibility(
        first_source, binding(first_source, SweepEndpointSide::end, 31U), water_port,
        first_source, binding(first_source, SweepEndpointSide::end, 32U), electrical_port);
    REQUIRE(!repeated_endpoint);
}

void owner_bound_sweeps_require_ports_from_the_same_scene_object() {
    auto source = sweep_source("conduit.owner-bound");
    source.owner_object_id = 204U;
    REQUIRE(source.validate());

    const auto matching_port = port(24U, 204U,
        carto::connection::compatibility::utility::water_25mm);
    const auto matching_binding = binding(source, SweepEndpointSide::start, 24U);
    REQUIRE(matching_binding.validate(source, matching_port));

    const auto foreign_port = port(25U, 205U,
        carto::connection::compatibility::utility::water_25mm);
    const auto foreign_binding = binding(source, SweepEndpointSide::start, 25U);
    const auto refused = foreign_binding.validate(source, foreign_port);
    REQUIRE(!refused);
    REQUIRE(refused.error().code == carto::core::ErrorCode::validation_failed);

    auto other_source = sweep_source("conduit.owner-bound.other");
    other_source.owner_object_id = 206U;
    REQUIRE(other_source.validate());
    const auto other_port = port(26U, 206U,
        carto::connection::compatibility::utility::water_25mm);
    const auto paired = carto::condition::check_sweep_endpoint_compatibility(
        source, matching_binding, matching_port,
        other_source, binding(other_source, SweepEndpointSide::end, 26U), other_port);
    REQUIRE(paired);
}

void same_source_endpoints_require_one_exact_revision_and_recipe() {
    const auto source = sweep_source("conduit.loop");
    const auto start_port = port(41U, 401U,
        carto::connection::compatibility::utility::water_25mm);
    const auto end_port = port(42U, 401U,
        carto::connection::compatibility::utility::water_25mm);
    const auto loop = carto::condition::check_sweep_endpoint_compatibility(
        source, binding(source, SweepEndpointSide::start, 41U), start_port,
        source, binding(source, SweepEndpointSide::end, 42U), end_port);
    REQUIRE(loop);
    REQUIRE(loop.value().first_sweep_digest == loop.value().second_sweep_digest);

    auto conflicting_snapshot = source;
    conflicting_snapshot.end_scale = 2.0;
    REQUIRE(conflicting_snapshot.validate());
    const auto conflict = carto::condition::check_sweep_endpoint_compatibility(
        source, binding(source, SweepEndpointSide::start, 41U), start_port,
        conflicting_snapshot, binding(conflicting_snapshot, SweepEndpointSide::end, 42U),
        end_port);
    REQUIRE(!conflict);
    REQUIRE(conflict.error().code == carto::core::ErrorCode::stale_data);

    auto other_revision = source;
    other_revision.source_revision = carto::core::Revision{2U};
    const auto revision_conflict = carto::condition::check_sweep_endpoint_compatibility(
        source, binding(source, SweepEndpointSide::start, 41U), start_port,
        other_revision, binding(other_revision, SweepEndpointSide::end, 42U), end_port);
    REQUIRE(!revision_conflict);
    REQUIRE(revision_conflict.error().code == carto::core::ErrorCode::stale_data);
}

void malformed_binding_records_fail_closed_at_identity_boundaries() {
    const auto source = sweep_source(
        std::string(SweepEndpointPortBinding::kMaxIdentityBytes, 'a'));
    const auto endpoint_port = port(51U, 501U,
        carto::connection::compatibility::utility::water_25mm);
    const auto valid_binding = binding(source, SweepEndpointSide::start, 51U);
    REQUIRE(valid_binding.validate(source, endpoint_port));

    const std::vector<std::string> invalid_identities{
        "", ".hidden", "_hidden", "-hidden", "contains space", "contains/slash",
        std::string{"embedded\0null", 13U},
        std::string(SweepEndpointPortBinding::kMaxIdentityBytes + 1U, 'a')};
    for (const auto& identity : invalid_identities) {
        auto malformed = valid_binding;
        malformed.sweep_source_identity = identity;
        const auto refused = malformed.validate(source, endpoint_port);
        REQUIRE(!refused);
        REQUIRE(refused.error().code == carto::core::ErrorCode::invalid_argument);
    }

    auto missing_revision = valid_binding;
    missing_revision.sweep_source_revision = carto::core::Revision{0U};
    REQUIRE(!missing_revision.validate(source, endpoint_port));
    auto missing_port = valid_binding;
    missing_port.port_id = carto::connection::PortId{0U};
    REQUIRE(!missing_port.validate(source, endpoint_port));

    auto wrong_identity = valid_binding;
    wrong_identity.sweep_source_identity = "different.source";
    const auto refused_identity = wrong_identity.validate(source, endpoint_port);
    REQUIRE(!refused_identity);
    REQUIRE(refused_identity.error().code == carto::core::ErrorCode::stale_data);
}

void second_endpoint_validation_cannot_be_bypassed_by_compatible_tags() {
    const auto first_source = sweep_source("conduit.valid");
    const auto second_source = sweep_source("conduit.checked");
    const auto first_port = port(61U, 601U,
        carto::connection::compatibility::utility::water_25mm);
    const auto second_port = port(62U, 602U,
        carto::connection::compatibility::utility::water_25mm);
    const auto first_binding = binding(first_source, SweepEndpointSide::end, 61U);
    const auto second_binding = binding(second_source, SweepEndpointSide::start, 62U);

    auto stale_binding = second_binding;
    stale_binding.sweep_source_revision = carto::core::Revision{2U};
    const auto stale = carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port, second_source, stale_binding, second_port);
    REQUIRE(!stale);
    REQUIRE(stale.error().code == carto::core::ErrorCode::stale_data);

    auto invalid_source = second_source;
    invalid_source.profile.clear();
    REQUIRE(!carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        invalid_source, second_binding, second_port));
    auto invalid_port = second_port;
    invalid_port.local_frame.transform_to_parent.translation.x =
        std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, invalid_port));
    invalid_port = second_port;
    invalid_port.owner = carto::scene::ObjectId{0U};
    REQUIRE(!carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, invalid_port));

    REQUIRE(carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, second_port));
}

void endpoint_capability_requirements_are_enforced_in_both_directions() {
    const std::string fluid{carto::connection::capabilities::fluid_transfer};
    const std::string data{carto::connection::capabilities::data_transfer};
    const auto first_source = sweep_source("conduit.requirements.first");
    const auto second_source = sweep_source("conduit.requirements.second");
    const auto first_port = port(71U, 701U,
        carto::connection::compatibility::utility::water_25mm, {fluid}, {data});
    const auto second_port = port(72U, 702U,
        carto::connection::compatibility::utility::water_25mm, {data}, {fluid});
    const auto first_binding = binding(first_source, SweepEndpointSide::end, 71U);
    const auto second_binding = binding(second_source, SweepEndpointSide::start, 72U);
    const auto compatible = carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, second_port);
    REQUIRE(compatible);
    REQUIRE(compatible.value().port_compatibility.first_satisfied_requirements ==
        std::vector<std::string>{fluid});
    REQUIRE(compatible.value().port_compatibility.second_satisfied_requirements ==
        std::vector<std::string>{data});

    auto missing_first_offer = first_port;
    missing_first_offer.capability_contract.offered.clear();
    REQUIRE(!carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, missing_first_offer,
        second_source, second_binding, second_port));
    auto missing_second_offer = second_port;
    missing_second_offer.capability_contract.offered.clear();
    REQUIRE(!carto::condition::check_sweep_endpoint_compatibility(
        first_source, first_binding, first_port,
        second_source, second_binding, missing_second_offer));
}

void malformed_evidence_cannot_receive_a_canonical_digest() {
    const auto source = sweep_source("conduit.evidence");
    const auto first_port = port(81U, 801U,
        carto::connection::compatibility::utility::water_25mm);
    const auto second_port = port(82U, 801U,
        carto::connection::compatibility::utility::water_25mm);
    const auto checked = carto::condition::check_sweep_endpoint_compatibility(
        source, binding(source, SweepEndpointSide::start, 81U), first_port,
        source, binding(source, SweepEndpointSide::end, 82U), second_port);
    REQUIRE(checked);
    REQUIRE(checked.value().canonical_digest());

    using Evidence = carto::condition::SweepEndpointCompatibilityEvidence;
    std::vector<Evidence> malformed;
    auto changed = checked.value();
    changed.first_sweep_digest = {};
    malformed.push_back(changed);
    changed = checked.value();
    changed.second_sweep_digest = {};
    malformed.push_back(changed);
    changed = checked.value();
    changed.first_port_digest = {};
    malformed.push_back(changed);
    changed = checked.value();
    changed.second_port_digest = {};
    malformed.push_back(changed);
    changed = checked.value();
    changed.second = changed.first;
    malformed.push_back(changed);
    changed = checked.value();
    changed.second.port_id = changed.first.port_id;
    malformed.push_back(changed);
    changed = checked.value();
    changed.second.sweep_source_revision = carto::core::Revision{2U};
    malformed.push_back(changed);
    changed = checked.value();
    changed.second_sweep_digest = changed.first_port_digest;
    malformed.push_back(changed);
    changed = checked.value();
    changed.port_compatibility.shared_tags.clear();
    malformed.push_back(changed);
    for (const auto& evidence : malformed) {
        REQUIRE(!evidence.validate());
        REQUIRE(!evidence.canonical_digest());
    }
    REQUIRE(checked.value().canonical_digest());
}

void spatial_review_resolves_exact_owner_frames_and_explicit_tolerances() {
    carto::scene::Scene scene;
    auto owner_transform = carto::core::Transform::identity();
    owner_transform.translation = {4.0, -2.0, 7.0};
    const auto owner = scene.create_object("sweep owner", owner_transform);
    REQUIRE(owner);

    auto source = sweep_source("conduit.spatial");
    source.owner_object_id = owner.value().value;
    REQUIRE(source.validate());
    const auto path = straight_centerline();

    auto endpoint_port = port(61U, owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    endpoint_port.local_frame.transform_to_parent.translation = {0.0, 0.0, 1.0};
    carto::connection::ConnectionPortGraph graph;
    REQUIRE(graph.insert_port(endpoint_port));

    carto::condition::SweepEndpointSpatialPolicy policy;
    policy.maximum_position_error = 0.001;
    policy.minimum_axis_alignment = 0.99;
    REQUIRE(!policy.validate());
    policy.port_outward_axis_local = {0.0, 0.0, 1.0};
    const auto endpoint_binding = binding(source, SweepEndpointSide::end, 61U);
    const auto review = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, graph, scene, policy,
        5U, carto::core::Revision{12U});
    REQUIRE(review && review.value().validate() && review.value().aligned);
    REQUIRE(review.value().position_error == 0.0);
    REQUIRE(review.value().axis_alignment == 1.0);
    REQUIRE(review.value().sweep_endpoint_world.x == 4.0);
    REQUIRE(review.value().sweep_endpoint_world.y == -2.0);
    REQUIRE(review.value().sweep_endpoint_world.z == 8.0);
    REQUIRE(review.value().port_origin_world.x == 4.0);
    REQUIRE(review.value().port_origin_world.y == -2.0);
    REQUIRE(review.value().port_origin_world.z == 8.0);
    REQUIRE(review.value().frame_chain_port_ids == std::vector<std::uint64_t>{61U});
    const auto repeated = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, graph, scene, policy,
        5U, carto::core::Revision{12U});
    REQUIRE(repeated && repeated.value().serialize() == review.value().serialize());
    REQUIRE(repeated.value().canonical_digest().value() ==
        review.value().canonical_digest().value());

    auto changed_measurement = review.value();
    changed_measurement.position_error += 0.25;
    REQUIRE(!changed_measurement.validate());
    REQUIRE(!changed_measurement.canonical_digest());

    auto offset_port = endpoint_port;
    offset_port.local_frame.transform_to_parent.translation.z = 1.25;
    carto::connection::ConnectionPortGraph offset_graph;
    REQUIRE(offset_graph.insert_port(offset_port));
    const auto offset = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, offset_graph, scene, policy,
        5U, carto::core::Revision{12U});
    REQUIRE(offset && !offset.value().aligned &&
        !offset.value().position_within_tolerance && offset.value().position_error == 0.25);

    auto reversed_port = endpoint_port;
    reversed_port.local_frame.transform_to_parent.rotation = {1.0, 0.0, 0.0, 0.0};
    carto::connection::ConnectionPortGraph reversed_graph;
    REQUIRE(reversed_graph.insert_port(reversed_port));
    const auto reversed = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, reversed_graph, scene, policy,
        5U, carto::core::Revision{12U});
    REQUIRE(reversed && !reversed.value().aligned &&
        reversed.value().position_within_tolerance &&
        !reversed.value().axis_within_tolerance && reversed.value().axis_alignment == -1.0);

    const auto foreign_owner = scene.create_object("foreign port owner");
    REQUIRE(foreign_owner);
    const auto foreign_port = port(62U, foreign_owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    carto::connection::ConnectionPortGraph foreign_graph;
    REQUIRE(foreign_graph.insert_port(foreign_port));
    REQUIRE(!carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, binding(source, SweepEndpointSide::end, 62U),
        foreign_graph, scene, policy, 5U, carto::core::Revision{12U}));

    auto stale_path = path;
    stale_path.source_revision = carto::core::Revision{4U};
    REQUIRE(!carto::condition::review_sweep_endpoint_spatial_fit(
        source, stale_path, endpoint_binding, graph, scene, policy,
        5U, carto::core::Revision{12U}));
    auto invalid_policy = policy;
    invalid_policy.port_outward_axis_local = {0.0, 0.0, 2.0};
    REQUIRE(!carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, graph, scene, invalid_policy,
        5U, carto::core::Revision{12U}));
}

void spatial_review_resolves_parent_frames_and_refuses_ambiguous_chains() {
    carto::scene::Scene scene;
    const auto owner = scene.create_object("frame owner");
    REQUIRE(owner);
    const auto source = [&] {
        auto value = sweep_source("conduit.parent-frame");
        value.owner_object_id = owner.value().value;
        REQUIRE(value.validate());
        return value;
    }();
    const auto path = straight_centerline();

    auto parent = port(71U, owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    parent.local_frame.frame_identity = "mount";
    parent.local_frame.transform_to_parent.translation = {0.0, 0.0, 0.5};
    auto endpoint = port(72U, owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    endpoint.local_frame.frame_identity = "end";
    endpoint.local_frame.parent_frame = "mount";
    endpoint.local_frame.transform_to_parent.translation = {0.0, 0.0, 0.5};
    carto::connection::ConnectionPortGraph graph;
    REQUIRE(graph.insert_port(parent));
    REQUIRE(graph.insert_port(endpoint));

    carto::condition::SweepEndpointSpatialPolicy policy;
    policy.maximum_position_error = 0.001;
    policy.minimum_axis_alignment = 0.99;
    REQUIRE(!policy.validate());
    policy.port_outward_axis_local = {0.0, 0.0, 1.0};
    const auto endpoint_binding = binding(source, SweepEndpointSide::end, 72U);
    const auto resolved = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, graph, scene, policy,
        0U, carto::core::Revision{8U});
    REQUIRE(resolved && resolved.value().aligned);
    REQUIRE(resolved.value().frame_chain_port_ids ==
        (std::vector<std::uint64_t>{72U, 71U}));
    REQUIRE(resolved.value().port_origin_world.z == 1.0);

    auto duplicate_parent = port(73U, owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    duplicate_parent.local_frame.frame_identity = "mount";
    carto::connection::ConnectionPortGraph ambiguous;
    REQUIRE(ambiguous.insert_port(parent));
    REQUIRE(ambiguous.insert_port(endpoint));
    REQUIRE(ambiguous.insert_port(duplicate_parent));
    const auto refused = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, ambiguous, scene, policy,
        0U, carto::core::Revision{8U});
    REQUIRE(!refused && refused.error().code == carto::core::ErrorCode::validation_failed);

    carto::connection::ConnectionPortGraph missing_parent;
    REQUIRE(missing_parent.insert_port(endpoint));
    const auto missing = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, missing_parent, scene, policy,
        0U, carto::core::Revision{8U});
    REQUIRE(!missing && missing.error().code == carto::core::ErrorCode::not_found);

    auto foreign_convention_parent = parent;
    foreign_convention_parent.local_frame.coordinate_convention = "other.frame/v1";
    carto::connection::ConnectionPortGraph mixed_conventions;
    REQUIRE(mixed_conventions.insert_port(foreign_convention_parent));
    REQUIRE(mixed_conventions.insert_port(endpoint));
    const auto mixed = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, endpoint_binding, mixed_conventions, scene, policy,
        0U, carto::core::Revision{8U});
    REQUIRE(!mixed && mixed.error().code == carto::core::ErrorCode::validation_failed);

    auto cycle_first = port(74U, owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    cycle_first.local_frame.frame_identity = "cycle-a";
    cycle_first.local_frame.parent_frame = "cycle-b";
    auto cycle_second = port(75U, owner.value().value,
        carto::connection::compatibility::utility::water_25mm);
    cycle_second.local_frame.frame_identity = "cycle-b";
    cycle_second.local_frame.parent_frame = "cycle-a";
    carto::connection::ConnectionPortGraph cyclic;
    REQUIRE(cyclic.insert_port(cycle_first));
    REQUIRE(cyclic.insert_port(cycle_second));
    const auto refused_cycle = carto::condition::review_sweep_endpoint_spatial_fit(
        source, path, binding(source, SweepEndpointSide::end, 74U),
        cyclic, scene, policy, 0U, carto::core::Revision{8U});
    REQUIRE(!refused_cycle &&
        refused_cycle.error().code == carto::core::ErrorCode::validation_failed);
}

} // namespace

int main() {
    try {
        compatible_endpoint_pair_returns_source_bound_deterministic_evidence();
        stale_or_mismatched_bindings_refuse_before_compatibility();
        incompatible_and_duplicate_endpoint_pairs_fail_closed();
        same_source_endpoints_require_one_exact_revision_and_recipe();
        malformed_binding_records_fail_closed_at_identity_boundaries();
        second_endpoint_validation_cannot_be_bypassed_by_compatible_tags();
        endpoint_capability_requirements_are_enforced_in_both_directions();
        malformed_evidence_cannot_receive_a_canonical_digest();
        spatial_review_resolves_exact_owner_frames_and_explicit_tolerances();
        spatial_review_resolves_parent_frames_and_refuses_ambiguous_chains();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
