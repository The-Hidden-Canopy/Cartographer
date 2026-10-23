#pragma once

#include <carto/character/time.hpp>
#include <carto/kinematic/graph.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::motion {

// Digest-bound pin of one capture source: the declared locator (file plus
// byte/segment range), the exact content digest of that segment, and the
// importer identity that produced it. Pins are sorted by digest so serialized
// ingest records are deterministic regardless of import order.
struct SourcePin {
    static constexpr std::size_t kMaxLocatorBytes = 1'024U;
    static constexpr std::size_t kMaxImporterBytes = 256U;

    std::string locator;
    assets::Sha256Digest segment_digest;
    std::string importer_name;
    std::string importer_version;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const SourcePin&) const noexcept = default;
    [[nodiscard]] bool operator<(const SourcePin& other) const noexcept {
        return segment_digest < other.segment_digest;
    }
};

// Rights evidence evaluated for one use scope. A caller-supplied tag alone is
// never legal evidence: evaluation requires explicit license grant strings
// and a non-empty attesting party, and fails closed when either is absent.
struct RightsAttestation {
    static constexpr std::size_t kMaxScopeBytes = 128U;
    static constexpr std::size_t kMaxGrantBytes = 256U;
    static constexpr std::size_t kMaxAttesterBytes = 256U;
    static constexpr std::size_t kMaxGrants = 64U;

    // Sorted, unique, non-empty license grant strings, e.g.
    // "license:performer-release-v2".
    std::vector<std::string> license_grants;
    std::string attested_by;

    [[nodiscard]] core::Result<void> validate() const;
};

// Evaluates actual permissions for a particular asset/use scope. Every
// required grant must be present in the attestation; any missing or defective
// rights yield an explicit unsupported refusal, never a silent downgrade of
// the intended use.
[[nodiscard]] core::Result<void> evaluate_rights(
    const RightsAttestation& attestation,
    std::string_view use_scope,
    const std::vector<std::string_view>& required_grants);

// Exact rational frame period for a capture declared as frame_rate_numerator
// frames per frame_rate_denominator seconds (30000/1001 for NTSC drop-free
// video rates). The period is exact integer rational time; the seconds error
// bound covers only the rational-to-double reporting.
struct CaptureTimebaseReceipt {
    std::uint32_t frame_rate_numerator = 0U;
    std::uint32_t frame_rate_denominator = 0U;
    character::RationalTime frame_period{0, 0};
    double frame_period_seconds = 0.0;
    double max_abs_error_seconds = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Converts a declared capture frame rate to the exact rational frame period.
// Non-finite-adjacent or zero rates fail closed; mixed rational rates such as
// 30000/1001 keep full precision instead of being rounded to a display frame.
[[nodiscard]] core::Result<CaptureTimebaseReceipt> convert_capture_timebase(
    std::uint32_t frame_rate_numerator,
    std::uint32_t frame_rate_denominator);

// One captured joint track: original sample timestamps on the capture
// timebase and the raw observed local transforms. Capture-space joint names
// are data, never semantic inference.
struct MocapJointTrack {
    static constexpr std::size_t kMaxJointNameBytes = 256U;
    static constexpr std::size_t kMaxSamplesPerTrack = 65'536U;

    std::string joint_name;
    std::vector<character::RationalTime> sample_times;
    std::vector<core::Transform> sample_poses;

    [[nodiscard]] core::Result<void> validate() const;
};

// The revisioned record of one accepted capture ingest. Raw capture bytes
// stay outside project source; this record pins the authorized references
// and digests. Validation fails closed when pins, importer identity, rights
// evidence or the timebase are missing or defective.
class MocapIngestRecord final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxPins = 256U;
    static constexpr std::size_t kMaxTracks = 4'096U;

    std::vector<SourcePin> pins;
    RightsAttestation rights;
    CaptureTimebaseReceipt timebase;
    std::vector<MocapJointTrack> tracks;
    core::Revision revision = core::Revision{1U};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<MocapIngestRecord> deserialize(
        std::string_view text);
};

} // namespace carto::motion
