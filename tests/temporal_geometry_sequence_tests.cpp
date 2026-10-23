#include <carto/temporal/geometry_sequence.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

using namespace carto;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(char nibble) {
    const auto parsed = assets::Sha256Digest::from_hex(std::string(64U, nibble));
    require(static_cast<bool>(parsed), "fixture digest parses");
    return parsed.value();
}

temporal::GeometrySequence make_sequence() {
    temporal::GeometrySequence sequence;
    sequence.id = temporal::SequenceId{17U};
    sequence.source_identity = "capture:take-17";
    sequence.source_revision = "revision-4";
    sequence.source_digest = digest('a');
    sequence.revision = core::Revision{3U};
    sequence.topology_stable = true;
    sequence.frames = {
        {0U, 0.0, "frames/frame-000.obj", digest('b'), 7001U,
            core::Revision{2U}, digest('c'), digest('d'), digest('e')},
        {1U, 0.033, "frames/frame-001.obj", digest('f'), 7001U,
            core::Revision{3U}, digest('1'), digest('d'), digest('2')},
    };
    return sequence;
}

void valid_sequence_round_trips_canonically() {
    const auto sequence = make_sequence();
    require(static_cast<bool>(sequence.validate()), "valid geometry sequence validates");

    const std::string encoded = sequence.serialize();
    const auto decoded = temporal::GeometrySequence::deserialize(encoded);
    require(static_cast<bool>(decoded), "valid geometry sequence deserializes");
    require(decoded.value().serialize() == encoded,
        "geometry sequence serialization is canonical");

    const auto first_digest = sequence.canonical_digest();
    const auto decoded_digest = decoded.value().canonical_digest();
    require(first_digest && decoded_digest && first_digest.value() == decoded_digest.value(),
        "geometry sequence digest survives a round trip");
}

void topology_policy_and_source_freshness_are_enforced() {
    auto sequence = make_sequence();
    sequence.frames[1U].topology_digest = digest('9');
    require(!sequence.validate(),
        "topology-stable sequence rejects a per-frame topology change");

    sequence.topology_stable = false;
    require(static_cast<bool>(sequence.validate()),
        "explicitly topology-changing sequence preserves both topology digests");

    sequence = make_sequence();
    require(static_cast<bool>(sequence.validate_source(
                "capture:take-17", "revision-4", digest('a'))),
        "current source identity, revision, and digest are accepted");
    const auto stale_revision = sequence.validate_source(
        "capture:take-17", "revision-5", digest('a'));
    require(!stale_revision && stale_revision.error().code == core::ErrorCode::stale_data,
        "stale source revision is rejected with stale_data");
    const auto stale_digest = sequence.validate_source(
        "capture:take-17", "revision-4", digest('9'));
    require(!stale_digest && stale_digest.error().code == core::ErrorCode::stale_data,
        "stale source digest is rejected instead of accepted as current data");
    const auto wrong_identity = sequence.validate_source(
        "capture:other-take", "revision-4", digest('a'));
    require(!wrong_identity && wrong_identity.error().code == core::ErrorCode::stale_data,
        "different source identity is rejected");
}

void hostile_frame_boundaries_fail_closed() {
    auto sequence = make_sequence();
    sequence.frames[1U].frame_index = sequence.frames[0U].frame_index;
    require(!sequence.validate(), "duplicate frame indices are rejected");

    sequence = make_sequence();
    sequence.frames[1U].time_seconds = sequence.frames[0U].time_seconds;
    require(!sequence.validate(), "duplicate frame times are rejected");

    sequence = make_sequence();
    sequence.frames[1U].time_seconds = std::numeric_limits<double>::infinity();
    require(!sequence.validate(), "non-finite frame times are rejected");

    sequence = make_sequence();
    sequence.frames[1U].source_locator = "frames/frame-001.obj\nEND";
    require(!sequence.validate(), "control characters in source locators are rejected");

    sequence = make_sequence();
    sequence.frames[1U].correspondence_digest = {};
    require(!sequence.validate(), "missing correspondence evidence is rejected");
}

void malformed_or_unbound_records_fail_closed() {
    const std::string encoded = make_sequence().serialize();
    require(!temporal::GeometrySequence::deserialize(encoded + "unbound"),
        "trailing unbound bytes are rejected");

    std::string future = encoded;
    const std::string current_version = "CARTOGRAPHER_GEOMETRY_SEQUENCE 1";
    future.replace(0U, current_version.size(), "CARTOGRAPHER_GEOMETRY_SEQUENCE 99");
    require(!temporal::GeometrySequence::deserialize(future),
        "unknown schema versions are rejected");

    require(!temporal::GeometrySequence::deserialize(
                std::string(temporal::GeometrySequence::kMaxSerializedBytes + 1U, 'x')),
        "oversized serialized records are rejected before parsing");
}

} // namespace

int main() {
    valid_sequence_round_trips_canonically();
    topology_policy_and_source_freshness_are_enforced();
    hostile_frame_boundaries_fail_closed();
    malformed_or_unbound_records_fail_closed();
    std::cout << "temporal geometry sequence tests passed\n";
    return EXIT_SUCCESS;
}
