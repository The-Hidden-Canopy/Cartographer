#include <carto/temporal/geometry_sequence.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace carto::temporal {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_GEOMETRY_SEQUENCE";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool safe_text(std::string_view value, std::size_t max_size) {
    return !value.empty() && value.size() <= max_size &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte >= 0x20U && byte != 0x7fU && byte != '\r' && byte != '\n';
        });
}

bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string hex;
    if (!(input >> hex)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(hex);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

bool read_integer(std::istream& input, std::uint64_t& value) {
    input >> value;
    return static_cast<bool>(input);
}

} // namespace

core::Result<void> GeometrySequence::validate() const {
    if (!id || !safe_text(source_identity, 256U) ||
        !safe_text(source_revision, 256U) || source_digest.is_zero() ||
        revision.exhausted() || frames.empty() || frames.size() > kMaxFrames) {
        return core::Result<void>::failure(validation(
            "geometry sequence identity, source binding, revision, or frame count is invalid"));
    }
    std::set<std::uint64_t> frame_indices;
    double previous_time = -1.0;
    std::size_t estimated_bytes = 2048U;
    assets::Sha256Digest first_topology;
    for (std::size_t index = 0U; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        if (!std::isfinite(frame.time_seconds) || frame.time_seconds < 0.0 ||
            frame.time_seconds > 1'000'000'000.0 || frame.time_seconds <= previous_time ||
            !frame_indices.insert(frame.frame_index).second ||
            !safe_text(frame.source_locator, 4096U) || frame.source_digest.is_zero() ||
            frame.mesh_asset == 0U || frame.mesh_revision.exhausted() ||
            frame.geometry_digest.is_zero() || frame.topology_digest.is_zero() ||
            frame.correspondence_digest.is_zero()) {
            return core::Result<void>::failure(validation(
                "geometry sequence frame identity, time, source, or correspondence is invalid"));
        }
        const std::size_t escaped_locator_bound = frame.source_locator.size() * 2U;
        if (estimated_bytes > kMaxSerializedBytes - 512U ||
            escaped_locator_bound > kMaxSerializedBytes - estimated_bytes - 512U) {
            return core::Result<void>::failure(validation(
                "geometry sequence exceeds its bounded serialized size"));
        }
        estimated_bytes += 512U + escaped_locator_bound;
        if (index == 0U) first_topology = frame.topology_digest;
        else if (topology_stable && frame.topology_digest != first_topology) {
            return core::Result<void>::failure(validation(
                "topology-stable geometry sequence contains a topology change"));
        }
        previous_time = frame.time_seconds;
    }
    return core::Result<void>::success();
}

std::string GeometrySequence::serialize() const {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
        << kMagic << ' ' << kSchemaVersion << '\n'
        << id.value << ' ' << std::quoted(source_identity) << ' '
        << std::quoted(source_revision) << ' ' << source_digest.hex() << ' '
        << revision.value() << ' ' << (topology_stable ? 1 : 0) << ' '
        << frames.size() << '\n';
    for (const auto& frame : frames) {
        output << frame.frame_index << ' ' << frame.time_seconds << ' '
            << std::quoted(frame.source_locator) << ' ' << frame.source_digest.hex() << ' '
            << frame.mesh_asset << ' ' << frame.mesh_revision.value() << ' '
            << frame.geometry_digest.hex() << ' ' << frame.topology_digest.hex() << ' '
            << frame.correspondence_digest.hex() << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> GeometrySequence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string bytes = serialize();
    if (bytes.size() > kMaxSerializedBytes) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "geometry sequence serialization exceeds its byte limit"));
    }
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
            bytes.size())));
}

core::Result<GeometrySequence> GeometrySequence::deserialize(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxSerializedBytes) {
        return core::Result<GeometrySequence>::failure(invalid(
            "geometry sequence record is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    std::string magic;
    std::uint32_t version = 0U;
    std::uint64_t revision_value = 0U;
    std::uint64_t topology_stable_value = 0U;
    std::uint64_t frame_count = 0U;
    GeometrySequence sequence;
    if (!(input >> magic >> version) || magic != kMagic || version != kSchemaVersion ||
        !read_integer(input, sequence.id.value) ||
        !(input >> std::quoted(sequence.source_identity) >> std::quoted(sequence.source_revision)) ||
        !read_digest(input, sequence.source_digest) || !read_integer(input, revision_value) ||
        !read_integer(input, topology_stable_value) || topology_stable_value > 1U ||
        !read_integer(input, frame_count) || frame_count == 0U || frame_count > kMaxFrames) {
        return core::Result<GeometrySequence>::failure(validation(
            "geometry sequence header is malformed"));
    }
    sequence.revision = core::Revision(revision_value);
    sequence.topology_stable = topology_stable_value != 0U;
    sequence.frames.reserve(static_cast<std::size_t>(frame_count));
    for (std::uint64_t index = 0U; index < frame_count; ++index) {
        GeometrySequenceFrame frame;
        std::uint64_t mesh_revision = 0U;
        if (!read_integer(input, frame.frame_index) || !(input >> frame.time_seconds) ||
            !(input >> std::quoted(frame.source_locator)) || !read_digest(input, frame.source_digest) ||
            !read_integer(input, frame.mesh_asset) || !read_integer(input, mesh_revision) ||
            !read_digest(input, frame.geometry_digest) || !read_digest(input, frame.topology_digest) ||
            !read_digest(input, frame.correspondence_digest)) {
            return core::Result<GeometrySequence>::failure(validation(
                "geometry sequence frame is malformed"));
        }
        frame.mesh_revision = core::Revision(mesh_revision);
        sequence.frames.push_back(std::move(frame));
    }
    std::string end;
    if (!(input >> end) || end != "END") {
        return core::Result<GeometrySequence>::failure(validation(
            "geometry sequence is missing its end record"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<GeometrySequence>::failure(validation(
            "geometry sequence contains trailing data"));
    }
    if (auto result = sequence.validate(); !result) {
        return core::Result<GeometrySequence>::failure(result.error());
    }
    return core::Result<GeometrySequence>::success(std::move(sequence));
}

core::Result<void> GeometrySequence::validate_source(
    std::string_view current_identity,
    std::string_view current_revision,
    const assets::Sha256Digest& current_digest) const {
    if (auto result = validate(); !result) return result;
    if (source_identity != current_identity || source_revision != current_revision ||
        source_digest != current_digest) {
        return core::Result<void>::failure(stale(
            "geometry sequence source identity, revision, or digest changed"));
    }
    return core::Result<void>::success();
}

} // namespace carto::temporal
