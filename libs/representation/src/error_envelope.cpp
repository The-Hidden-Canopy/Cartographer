#include <carto/representation/error_envelope.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <sstream>
#include <span>
#include <string>
#include <utility>

namespace carto::representation {
namespace {

constexpr std::string_view kEnvelopeMagic = "CARTOGRAPHER_REPRESENTATION_ERROR_ENVELOPE";

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

double canonical_double(double value) {
    return value == 0.0 ? 0.0 : value;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token)) return false;
    // Reject non-decimal spellings (hex floats, inf, nan) at the token level
    // so parsing stays canonical and platform-independent.
    if (token.find_first_not_of("+-0123456789.eE") != std::string::npos) {
        return false;
    }
    std::istringstream token_input(token);
    token_input >> std::setprecision(17) >> value;
    return static_cast<bool>(token_input) &&
        token_input.peek() == std::istringstream::traits_type::eof();
}

bool read_axis(std::istream& input, std::string_view expected, double& value) {
    std::string token;
    if (!(input >> token) || token != expected) return false;
    return read_double(input, value);
}

void write_axis(std::ostringstream& output, const char* name, double value) {
    output << name << ' ' << std::setprecision(17) << canonical_double(value) << '\n';
}

} // namespace

core::Result<void> RepresentationErrorEnvelope::validate() const {
    if (!finite_nonnegative(max_position_error) ||
        !finite_nonnegative(max_silhouette_error) ||
        !finite_nonnegative(max_normal_error) ||
        !finite_nonnegative(max_radiance_error)) {
        return core::Result<void>::failure(validation(
            "representation error envelope contains a non-finite or negative bound"));
    }
    return core::Result<void>::success();
}

std::string RepresentationErrorEnvelope::canonical() const {
    std::ostringstream output;
    output << "representation-error-envelope-v1{position="
           << std::setprecision(17) << canonical_double(max_position_error)
           << ";silhouette=" << canonical_double(max_silhouette_error)
           << ";normal=" << canonical_double(max_normal_error)
           << ";radiance=" << canonical_double(max_radiance_error) << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> RepresentationErrorEnvelope::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

std::string RepresentationErrorEnvelope::serialize() const {
    std::ostringstream output;
    output << kEnvelopeMagic << ' ' << kSchemaVersion << '\n';
    write_axis(output, "POSITION", max_position_error);
    write_axis(output, "SILHOUETTE", max_silhouette_error);
    write_axis(output, "NORMAL", max_normal_error);
    write_axis(output, "RADIANCE", max_radiance_error);
    output << "END\n";
    return output.str();
}

core::Result<RepresentationErrorEnvelope> RepresentationErrorEnvelope::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RepresentationErrorEnvelope>::failure(parse_error(
            "representation error envelope text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kEnvelopeMagic) {
        return core::Result<RepresentationErrorEnvelope>::failure(parse_error(
            "representation error envelope framing is invalid"));
    }
    std::uint32_t version = 0U;
    if (!(input >> version)) {
        return core::Result<RepresentationErrorEnvelope>::failure(parse_error(
            "representation error envelope version is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RepresentationErrorEnvelope>::failure(version_mismatch(
            "representation error envelope schema version is unsupported"));
    }
    RepresentationErrorEnvelope envelope;
    if (!read_axis(input, "POSITION", envelope.max_position_error) ||
        !read_axis(input, "SILHOUETTE", envelope.max_silhouette_error) ||
        !read_axis(input, "NORMAL", envelope.max_normal_error) ||
        !read_axis(input, "RADIANCE", envelope.max_radiance_error)) {
        return core::Result<RepresentationErrorEnvelope>::failure(parse_error(
            "representation error envelope axis is malformed"));
    }
    std::string end_token;
    std::string trailing;
    if (!(input >> end_token) || end_token != "END" || (input >> trailing)) {
        return core::Result<RepresentationErrorEnvelope>::failure(parse_error(
            "representation error envelope is missing END or contains trailing data"));
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<RepresentationErrorEnvelope>::failure(result.error());
    }
    return core::Result<RepresentationErrorEnvelope>::success(std::move(envelope));
}

RepresentationErrorEnvelope from_surface_envelope(
    const surface::SurfaceErrorEnvelope& envelope) {
    return RepresentationErrorEnvelope{
        envelope.max_position_error,
        envelope.max_silhouette_error,
        envelope.max_normal_error,
        envelope.max_radiance_error,
    };
}

surface::SurfaceErrorEnvelope to_surface_envelope(
    const RepresentationErrorEnvelope& envelope) {
    surface::SurfaceErrorEnvelope surface{};
    surface.max_position_error = envelope.max_position_error;
    surface.max_silhouette_error = envelope.max_silhouette_error;
    surface.max_normal_error = envelope.max_normal_error;
    surface.max_radiance_error = envelope.max_radiance_error;
    return surface;
}

RepresentationErrorEnvelope envelope_for_cluster(
    const web_geometry::ClusterRecord& cluster) {
    return RepresentationErrorEnvelope{
        cluster.geometric_error,
        cluster.geometric_error,
        cluster.geometric_error,
        0.0,
    };
}

core::Result<void> attach_cluster_error(
    web_geometry::ClusterRecord& cluster,
    const RepresentationErrorEnvelope& envelope) {
    if (auto result = envelope.validate(); !result) return result;
    if (!std::isfinite(cluster.geometric_error) || cluster.geometric_error < 0.0) {
        return core::Result<void>::failure(validation(
            "cluster geometric error is non-finite or negative"));
    }
    const double geometric_bound = std::max(
        {envelope.max_position_error, envelope.max_silhouette_error,
         envelope.max_normal_error});
    if (geometric_bound < cluster.geometric_error) {
        return core::Result<void>::failure(validation(
            "attaching the error envelope would tighten the cluster geometric "
            "error bound without new evidence"));
    }
    cluster.geometric_error = geometric_bound;
    return core::Result<void>::success();
}

} // namespace carto::representation
