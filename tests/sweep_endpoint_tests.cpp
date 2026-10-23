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
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
