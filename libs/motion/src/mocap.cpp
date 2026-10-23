#include <carto/motion/mocap.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace carto::motion {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(
        input >> transform.translation.x >> transform.translation.y >>
        transform.translation.z >> transform.rotation.x >> transform.rotation.y >>
        transform.rotation.z >> transform.rotation.w >> transform.scale.x >>
        transform.scale.y >> transform.scale.z);
}

} // namespace

core::Result<void> SourcePin::validate() const {
    if (!safe_text(locator, kMaxLocatorBytes) || segment_digest.is_zero() ||
        !safe_text(importer_name, kMaxImporterBytes) ||
        !safe_text(importer_version, kMaxImporterBytes)) {
        return core::Result<void>::failure(invalid(
            "source pin requires a locator, segment digest and importer identity"));
    }
    return core::Result<void>::success();
}

core::Result<void> RightsAttestation::validate() const {
    if (license_grants.empty() || license_grants.size() > kMaxGrants ||
        !safe_text(attested_by, kMaxAttesterBytes)) {
        return core::Result<void>::failure(invalid(
            "rights attestation requires grants within bounds and an attesting party"));
    }
    for (const std::string& grant : license_grants) {
        if (!safe_text(grant, kMaxGrantBytes)) {
            return core::Result<void>::failure(invalid(
                "rights attestation grant is empty or exceeds its byte bound"));
        }
    }
    for (std::size_t index = 1U; index < license_grants.size(); ++index) {
        if (!(license_grants[index - 1U] < license_grants[index])) {
            return core::Result<void>::failure(validation(
                "rights attestation grants must be sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> evaluate_rights(
    const RightsAttestation& attestation,
    std::string_view use_scope,
    const std::vector<std::string_view>& required_grants) {
    if (auto result = attestation.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (!safe_text(use_scope, RightsAttestation::kMaxScopeBytes)) {
        return core::Result<void>::failure(invalid(
            "rights use scope is empty or exceeds its byte bound"));
    }
    for (const std::string_view required : required_grants) {
        const bool granted = std::binary_search(
            attestation.license_grants.begin(), attestation.license_grants.end(),
            required);
        if (!granted) {
            return core::Result<void>::failure(unsupported(
                "capture rights do not grant the required license for this use scope"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> CaptureTimebaseReceipt::validate() const {
    if (frame_rate_numerator == 0U || frame_rate_denominator == 0U) {
        return core::Result<void>::failure(invalid(
            "capture frame rate must be a positive rational"));
    }
    if (auto result = frame_period.validate(); !result) return result;
    if (frame_period.tick != static_cast<std::int64_t>(frame_rate_denominator) ||
        frame_period.ticks_per_second != frame_rate_numerator) {
        return core::Result<void>::failure(validation(
            "capture frame period must equal denominator/numerator exactly"));
    }
    if (!std::isfinite(frame_period_seconds) || frame_period_seconds <= 0.0 ||
        !std::isfinite(max_abs_error_seconds) || max_abs_error_seconds < 0.0) {
        return core::Result<void>::failure(invalid(
            "capture frame period reporting must be finite, positive and error-bounded"));
    }
    return core::Result<void>::success();
}

core::Result<CaptureTimebaseReceipt> convert_capture_timebase(
    std::uint32_t frame_rate_numerator,
    std::uint32_t frame_rate_denominator) {
    if (frame_rate_numerator == 0U || frame_rate_denominator == 0U) {
        return core::Result<CaptureTimebaseReceipt>::failure(invalid(
            "capture frame rate must be a positive rational"));
    }
    const character::RationalTime frame_period{
        static_cast<std::int64_t>(frame_rate_denominator), frame_rate_numerator};
    const auto seconds = character::to_seconds(frame_period);
    CaptureTimebaseReceipt receipt{
        frame_rate_numerator, frame_rate_denominator, frame_period,
        seconds.seconds, seconds.max_abs_error};
    if (auto result = receipt.validate(); !result) {
        return core::Result<CaptureTimebaseReceipt>::failure(result.error());
    }
    return core::Result<CaptureTimebaseReceipt>::success(std::move(receipt));
}

core::Result<void> MocapJointTrack::validate() const {
    if (!safe_text(joint_name, kMaxJointNameBytes)) {
        return core::Result<void>::failure(invalid(
            "capture joint name is empty or exceeds its byte bound"));
    }
    if (sample_times.empty() || sample_times.size() > kMaxSamplesPerTrack ||
        sample_times.size() != sample_poses.size()) {
        return core::Result<void>::failure(validation(
            "capture joint track requires aligned samples within its bounded capacity"));
    }
    for (const character::RationalTime& time : sample_times) {
        if (auto result = time.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < sample_times.size(); ++index) {
        if (sample_times[index - 1U].ticks_per_second !=
                sample_times[index].ticks_per_second ||
            sample_times[index - 1U].tick >= sample_times[index].tick) {
            return core::Result<void>::failure(validation(
                "capture joint track samples must be strictly increasing on one timebase"));
        }
    }
    for (const core::Transform& pose : sample_poses) {
        if (!pose.finite()) {
            return core::Result<void>::failure(invalid(
                "capture joint track poses must be finite"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> MocapIngestRecord::validate() const {
    if (pins.empty() || pins.size() > kMaxPins) {
        return core::Result<void>::failure(validation(
            "mocap ingest requires between one and its bounded capacity of source pins"));
    }
    for (const SourcePin& pin : pins) {
        if (auto result = pin.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < pins.size(); ++index) {
        if (!(pins[index - 1U] < pins[index])) {
            return core::Result<void>::failure(validation(
                "mocap ingest pins must be sorted by digest and unique"));
        }
    }
    for (std::size_t index = 1U; index < pins.size(); ++index) {
        if (!(pins[index - 1U] < pins[index])) {
            return core::Result<void>::failure(validation(
                "mocap ingest pins must be sorted by digest and unique"));
        }
    }
    if (auto result = rights.validate(); !result) return result;
    if (auto result = timebase.validate(); !result) return result;
    if (tracks.size() > kMaxTracks) {
        return core::Result<void>::failure(validation(
            "mocap ingest track collection exceeds its bounded capacity"));
    }
    for (const MocapJointTrack& track : tracks) {
        if (auto result = track.validate(); !result) return result;
        for (const character::RationalTime& time : track.sample_times) {
            if (time.ticks_per_second != timebase.frame_period.ticks_per_second) {
                return core::Result<void>::failure(validation(
                    "capture track samples must use the declared capture timebase"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string MocapIngestRecord::serialize() const {
    std::ostringstream output;
    output << "CARTO_MOCAP_INGEST 1\n";
    output << "PINS " << pins.size() << '\n';
    for (const SourcePin& pin : pins) {
        output << "PIN \"" << pin.locator << "\" " << pin.segment_digest.hex()
               << " \"" << pin.importer_name << "\" \"" << pin.importer_version
               << "\"\n";
    }
    output << "RIGHTS " << rights.license_grants.size() << " \""
           << rights.attested_by << "\"\n";
    for (const std::string& grant : rights.license_grants) {
        output << "GRANT \"" << grant << "\"\n";
    }
    output << "TIMEBASE " << timebase.frame_rate_numerator << ' '
           << timebase.frame_rate_denominator << ' ' << timebase.frame_period.tick
           << ' ' << timebase.frame_period.ticks_per_second << ' '
           << std::setprecision(17) << timebase.frame_period_seconds << ' '
           << timebase.max_abs_error_seconds << '\n';
    output << "REVISION " << revision.value() << '\n';
    output << "TRACKS " << tracks.size() << '\n';
    for (const MocapJointTrack& track : tracks) {
        output << "TRACK \"" << track.joint_name << "\" " << track.sample_times.size()
               << '\n';
        for (std::size_t index = 0U; index < track.sample_times.size(); ++index) {
            output << "SAMPLE " << track.sample_times[index].tick << ' '
                   << track.sample_times[index].ticks_per_second << ' ';
            write_transform(output, track.sample_poses[index]);
            output << '\n';
        }
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> MocapIngestRecord::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<MocapIngestRecord> MocapIngestRecord::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_MOCAP_INGEST" || version != kSchemaVersion) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest framing or schema version is invalid"));
    }
    MocapIngestRecord record;
    std::string token;
    auto read_count = [&input, &token](std::string_view expected,
                                       std::uint64_t maximum,
                                       std::uint64_t& value) {
        return static_cast<bool>(input >> token) && token == expected &&
            static_cast<bool>(input >> value) && value <= maximum;
    };
    std::uint64_t count = 0U;
    if (!read_count("PINS", kMaxPins, count)) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest pin count is malformed or exceeds its bound"));
    }
    record.pins.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0U; index < count; ++index) {
        SourcePin pin;
        if (!(input >> token) || token != "PIN" ||
            !(input >> std::quoted(pin.locator)) ||
            !(input >> token)) {
            return core::Result<MocapIngestRecord>::failure(parse_error(
                "mocap ingest pin payload is malformed"));
        }
        const auto digest = assets::Sha256Digest::from_hex(token);
        if (!digest || !(input >> std::quoted(pin.importer_name)) ||
            !(input >> std::quoted(pin.importer_version))) {
            return core::Result<MocapIngestRecord>::failure(parse_error(
                "mocap ingest pin payload is malformed"));
        }
        pin.segment_digest = digest.value();
        record.pins.push_back(std::move(pin));
    }
    std::uint64_t grant_count = 0U;
    if (!read_count("RIGHTS", RightsAttestation::kMaxGrants, grant_count) ||
        !(input >> std::quoted(record.rights.attested_by))) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest rights header is malformed"));
    }
    record.rights.license_grants.reserve(static_cast<std::size_t>(grant_count));
    for (std::uint64_t index = 0U; index < grant_count; ++index) {
        std::string grant;
        if (!(input >> token) || token != "GRANT" ||
            !(input >> std::quoted(grant))) {
            return core::Result<MocapIngestRecord>::failure(parse_error(
                "mocap ingest grant payload is malformed"));
        }
        record.rights.license_grants.push_back(std::move(grant));
    }
    if (!(input >> token) || token != "TIMEBASE" ||
        !(input >> record.timebase.frame_rate_numerator >>
            record.timebase.frame_rate_denominator >> record.timebase.frame_period.tick >>
            record.timebase.frame_period.ticks_per_second >>
            record.timebase.frame_period_seconds >>
            record.timebase.max_abs_error_seconds)) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest timebase payload is malformed"));
    }
    std::uint64_t revision = 0U;
    if (!read_count("REVISION", std::numeric_limits<std::uint64_t>::max(), revision)) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest revision is malformed"));
    }
    record.revision = core::Revision(revision);
    std::uint64_t track_count = 0U;
    if (!read_count("TRACKS", kMaxTracks, track_count)) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest track count is malformed or exceeds its bound"));
    }
    record.tracks.reserve(static_cast<std::size_t>(track_count));
    for (std::uint64_t index = 0U; index < track_count; ++index) {
        MocapJointTrack track;
        std::uint64_t sample_count = 0U;
        if (!(input >> token) || token != "TRACK" ||
            !(input >> std::quoted(track.joint_name)) ||
            !(input >> sample_count) ||
            sample_count == 0U || sample_count > MocapJointTrack::kMaxSamplesPerTrack) {
            return core::Result<MocapIngestRecord>::failure(parse_error(
                "mocap ingest track header is malformed"));
        }
        track.sample_times.reserve(static_cast<std::size_t>(sample_count));
        track.sample_poses.reserve(static_cast<std::size_t>(sample_count));
        for (std::uint64_t sample = 0U; sample < sample_count; ++sample) {
            character::RationalTime time;
            core::Transform pose;
            if (!(input >> token) || token != "SAMPLE" ||
                !(input >> time.tick >> time.ticks_per_second) ||
                !read_transform(input, pose)) {
                return core::Result<MocapIngestRecord>::failure(parse_error(
                    "mocap ingest sample payload is malformed"));
            }
            track.sample_times.push_back(time);
            track.sample_poses.push_back(pose);
        }
        record.tracks.push_back(std::move(track));
    }
    if (!(input >> token) || token != "END") {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest terminator is missing"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<MocapIngestRecord>::failure(parse_error(
            "mocap ingest text contains trailing unbound data"));
    }
    if (auto result = record.validate(); !result) {
        return core::Result<MocapIngestRecord>::failure(result.error());
    }
    return core::Result<MocapIngestRecord>::success(std::move(record));
}

} // namespace carto::motion
