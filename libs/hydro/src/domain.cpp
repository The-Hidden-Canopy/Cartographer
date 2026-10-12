#include <carto/hydro/domain.hpp>

#include "detail.hpp"

#include <iomanip>
#include <sstream>

namespace carto::hydro {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_HYDRO_DOMAIN";

bool valid_kind(HydroBodyKind kind) noexcept {
    switch (kind) {
    case HydroBodyKind::ocean:
    case HydroBodyKind::lake:
    case HydroBodyKind::river:
    case HydroBodyKind::creek:
    case HydroBodyKind::pond:
    case HydroBodyKind::reservoir:
    case HydroBodyKind::canal:
    case HydroBodyKind::flood:
    case HydroBodyKind::compartment:
        return true;
    }
    return false;
}

bool parse_kind(std::string_view value, HydroBodyKind& kind) noexcept {
    for (const HydroBodyKind candidate : {
             HydroBodyKind::ocean, HydroBodyKind::lake, HydroBodyKind::river,
             HydroBodyKind::creek, HydroBodyKind::pond, HydroBodyKind::reservoir,
             HydroBodyKind::canal, HydroBodyKind::flood, HydroBodyKind::compartment}) {
        if (hydro_body_kind_name(candidate) == value) {
            kind = candidate;
            return true;
        }
    }
    return false;
}

} // namespace

std::string_view hydro_body_kind_name(HydroBodyKind kind) noexcept {
    switch (kind) {
    case HydroBodyKind::ocean: return "ocean";
    case HydroBodyKind::lake: return "lake";
    case HydroBodyKind::river: return "river";
    case HydroBodyKind::creek: return "creek";
    case HydroBodyKind::pond: return "pond";
    case HydroBodyKind::reservoir: return "reservoir";
    case HydroBodyKind::canal: return "canal";
    case HydroBodyKind::flood: return "flood";
    case HydroBodyKind::compartment: return "compartment";
    }
    return "unknown";
}

core::Result<void> HydroDomain::validate() const {
    if (!detail::safe_text(stable_id, kMaxIdentityBytes) ||
        !valid_kind(kind) ||
        !detail::safe_text(coordinate_frame, kMaxCoordinateFrameBytes) ||
        source_digest.is_zero() ||
        !detail::safe_text(provenance, kMaxProvenanceBytes)) {
        return core::Result<void>::failure(detail::validation(
            "hydro domain identity, kind, frame, provenance, or digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string HydroDomain::canonical() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n'
           << "ID " << std::quoted(stable_id) << '\n'
           << "KIND " << hydro_body_kind_name(kind) << '\n'
           << "FRAME " << std::quoted(coordinate_frame) << '\n'
           << "REVISION " << source_revision.value() << '\n'
           << "DIGEST " << source_digest.hex() << '\n'
           << "PROVENANCE " << std::quoted(provenance) << '\n'
           << "END\n";
    return output.str();
}

std::string HydroDomain::serialize() const { return canonical(); }

core::Result<assets::Sha256Digest> HydroDomain::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(detail::digest_of(canonical()));
}

core::Result<HydroDomain> HydroDomain::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<HydroDomain>::failure(detail::validation(
            "hydro domain document exceeds its bounded size"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    HydroDomain domain;
    std::string kind;
    if (!(input >> magic >> version) || magic != kMagic) {
        return core::Result<HydroDomain>::failure(detail::parse_error(
            "hydro domain header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<HydroDomain>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch, "unsupported hydro domain version"));
    }
    if (!detail::require_token(input, "ID") || !(input >> std::quoted(domain.stable_id)) ||
        !detail::require_token(input, "KIND") || !(input >> kind) ||
        !parse_kind(kind, domain.kind) ||
        !detail::require_token(input, "FRAME") ||
        !(input >> std::quoted(domain.coordinate_frame)) ||
        !detail::require_token(input, "REVISION") ||
        !detail::read_revision(input, domain.source_revision) ||
        !detail::require_token(input, "DIGEST") ||
        !detail::read_digest(input, domain.source_digest) ||
        !detail::require_token(input, "PROVENANCE") ||
        !(input >> std::quoted(domain.provenance)) ||
        !detail::require_token(input, "END") || !detail::at_end(input)) {
        return core::Result<HydroDomain>::failure(detail::parse_error(
            "hydro domain document is malformed"));
    }
    if (auto result = domain.validate(); !result) {
        return core::Result<HydroDomain>::failure(result.error());
    }
    if (domain.serialize() != text) {
        return core::Result<HydroDomain>::failure(detail::validation(
            "hydro domain is not in canonical form"));
    }
    return core::Result<HydroDomain>::success(std::move(domain));
}

} // namespace carto::hydro
