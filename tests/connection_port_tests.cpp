#include <carto/connection/port.hpp>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

using namespace carto;

connection::ConnectionPort make_port(
    std::uint64_t id,
    std::uint64_t owner,
    std::vector<connection::PortCompatibilityTag> tags,
    std::vector<std::string> required = {},
    std::vector<std::string> offered = {}) {
    connection::ConnectionPort port;
    port.id = connection::PortId{id};
    port.owner = scene::ObjectId{owner};
    port.authoring.kind = world::AuthoringBindingKind::scene_object;
    port.authoring.identity = owner;
    port.local_frame.frame_identity = "frame-" + std::to_string(id);
    port.local_frame.coordinate_convention = "carto.core/v1";
    port.compatibility_tags = std::move(tags);
    port.capability_contract.required = std::move(required);
    port.capability_contract.offered = std::move(offered);
    return port;
}

void reference_frames_validate() {
    connection::ReferenceFrame frame;
    frame.frame_identity = "drawbar-socket";
    frame.parent_frame = "tractor-chassis";
    frame.coordinate_convention = "carto.core/v1";
    require(static_cast<bool>(frame.validate()), "well-formed reference frame validates");

    connection::ReferenceFrame empty_identity;
    empty_identity.coordinate_convention = "carto.core/v1";
    require(!empty_identity.validate(), "empty frame identity is rejected");

    connection::ReferenceFrame self_parent;
    self_parent.frame_identity = "drawbar-socket";
    self_parent.parent_frame = "drawbar-socket";
    self_parent.coordinate_convention = "carto.core/v1";
    require(!self_parent.validate(), "self-referential parent frame is rejected");

    connection::ReferenceFrame non_finite;
    non_finite.frame_identity = "drawbar-socket";
    non_finite.coordinate_convention = "carto.core/v1";
    non_finite.transform_to_parent.translation.x =
        std::numeric_limits<double>::infinity();
    require(!non_finite.validate(), "non-finite frame transform is rejected");

    connection::ReferenceFrame no_convention;
    no_convention.frame_identity = "drawbar-socket";
    require(!no_convention.validate(), "missing convention reference is rejected");
}

void compatibility_refusal_matrix() {
    const auto& pin = connection::compatibility::mechanical_hitch::pin_hitch_19mm;
    const auto& pin_wide =
        connection::compatibility::mechanical_hitch::pin_hitch_25mm;
    const auto& hand = connection::compatibility::equipment::hand_grip;

    auto first = make_port(1U, 101U, {pin}, {std::string{connection::capabilities::load_bearing}});
    auto second = make_port(2U, 102U, {pin}, {}, {std::string{connection::capabilities::load_bearing}});

    auto evidence = connection::check_port_compatibility(first, second);
    require(static_cast<bool>(evidence), "matching tags with offered capability join");
    require(evidence.value().shared_tags.size() == 1U, "evidence names the shared tag");
    require(
        evidence.value().first_satisfied_requirements ==
            std::vector<std::string>{std::string{connection::capabilities::load_bearing}},
        "evidence records which requirements were satisfied");
    require(
        evidence.value().second_satisfied_requirements.empty(),
        "evidence records the satisfied requirements of each side separately");

    auto wrong_term = make_port(3U, 103U, {pin_wide});
    require(
        !connection::check_port_compatibility(first, wrong_term),
        "same namespace with mismatched terms refuses");

    auto wrong_space = make_port(4U, 104U, {hand});
    require(
        !connection::check_port_compatibility(first, wrong_space),
        "cross-namespace endpoints refuse");

    auto unmet_requirement = make_port(
        5U, 105U, {pin}, {std::string{connection::capabilities::power_transfer}});
    require(
        !connection::check_port_compatibility(unmet_requirement, second),
        "unmet required capabilities refuse");
    auto offering = make_port(
        6U, 106U, {pin}, {}, {std::string{connection::capabilities::power_transfer}});
    require(
        static_cast<bool>(connection::check_port_compatibility(unmet_requirement, offering)),
        "offered required capabilities admit the join");

    auto invalid_frame = first;
    invalid_frame.local_frame.frame_identity.clear();
    require(
        !connection::check_port_compatibility(invalid_frame, second),
        "an invalid endpoint refuses the join before any state change");

    auto evidence_round_trip = connection::PortCompatibilityEvidence::deserialize(
        evidence.value().serialize());
    require(
        static_cast<bool>(evidence_round_trip), "compatibility evidence round-trips");
    require(
        evidence_round_trip.value().serialize() == evidence.value().serialize(),
        "evidence canonical form is stable");
    require(
        static_cast<bool>(evidence.value().canonical_digest()),
        "compatibility evidence is digest-bound");
}

void canonical_form_is_deterministic() {
    const auto& pin = connection::compatibility::mechanical_hitch::pin_hitch_19mm;
    auto first = make_port(7U, 107U, {pin}, {}, {std::string{connection::capabilities::load_bearing}});
    auto rebuilt = make_port(7U, 107U, {pin}, {}, {std::string{connection::capabilities::load_bearing}});

    require(first.serialize() == rebuilt.serialize(), "same inputs serialize identically");
    const auto first_digest = first.canonical_digest();
    const auto rebuilt_digest = rebuilt.canonical_digest();
    require(
        static_cast<bool>(first_digest) && first_digest.value() == rebuilt_digest.value(),
        "same inputs digest identically");

    auto parsed = connection::ConnectionPort::deserialize(first.serialize());
    require(static_cast<bool>(parsed), "port deserialize accepts canonical form");
    require(
        parsed.value().serialize() == first.serialize(),
        "port round-trip preserves the canonical form");
    const auto parsed_digest = parsed.value().canonical_digest();
    require(
        static_cast<bool>(parsed_digest) && parsed_digest.value() == first_digest.value(),
        "port round-trip preserves the digest");

    auto unsorted = first;
    const auto& utility_tag = connection::compatibility::utility::electrical_16a;
    unsorted.compatibility_tags = {pin, utility_tag};
    require(
        static_cast<bool>(connection::check_port_compatibility(unsorted, unsorted)),
        "sorted multi-tag port validates");
    auto misordered = first;
    misordered.compatibility_tags = {utility_tag, pin};
    require(!misordered.validate(), "unsorted compatibility tags are rejected");
    auto duplicated = first;
    duplicated.compatibility_tags = {pin, pin};
    require(!duplicated.validate(), "duplicate compatibility tags are rejected");
}

void hostile_port_inputs_fail_closed() {
    const auto& pin = connection::compatibility::mechanical_hitch::pin_hitch_19mm;
    auto port = make_port(8U, 108U, {pin});
    const std::string encoded = port.serialize();

    require(
        !connection::ConnectionPort::deserialize(encoded + "trailing"),
        "trailing unbound data is rejected");

    std::string future = encoded;
    future.replace(
        0U, std::string{"CARTOGRAPHER_CONNECTION_PORT 1"}.size(),
        "CARTOGRAPHER_CONNECTION_PORT 99");
    const auto future_result = connection::ConnectionPort::deserialize(future);
    require(
        !future_result &&
            future_result.error().code == core::ErrorCode::version_mismatch,
        "unknown future versions fail with version_mismatch");

    const std::string oversized_counts =
        "CARTOGRAPHER_CONNECTION_PORT 1\n"
        "PORT 8 108 1 108 FRAME \"frame-8\" \"\" 0 0 0 0 0 0 1 1 1 1 "
        "\"carto.core/v1\" TAGS 33 \"mechanical-hitch\" \"pin-hitch-19mm\" REQUIRED 0 "
        "OFFERED 0\n"
        "END\n";
    require(
        !connection::ConnectionPort::deserialize(oversized_counts),
        "oversized tag counts are rejected before allocation");

    const std::string duplicate_tags =
        "CARTOGRAPHER_CONNECTION_PORT 1\n"
        "PORT 8 108 1 108 FRAME \"frame-8\" \"\" 0 0 0 0 0 0 1 1 1 1 "
        "\"carto.core/v1\" TAGS 2 \"mechanical-hitch\" \"pin-hitch-19mm\" "
        "\"mechanical-hitch\" \"pin-hitch-19mm\" REQUIRED 0 OFFERED 0\n"
        "END\n";
    require(
        !connection::ConnectionPort::deserialize(duplicate_tags),
        "duplicate compatibility tags refuse deserialization");

    require(
        !connection::ConnectionPort::deserialize("not a port record"),
        "malformed framing is rejected");
    require(
        !connection::ConnectionPort::deserialize(std::string(4096U, '\0')),
        "non-textual garbage is rejected");
}

} // namespace

int main() {
    reference_frames_validate();
    compatibility_refusal_matrix();
    canonical_form_is_deterministic();
    hostile_port_inputs_fail_closed();
    std::cout << "connection port tests passed\n";
    return EXIT_SUCCESS;
}
