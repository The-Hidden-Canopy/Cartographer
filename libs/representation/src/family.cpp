#include <carto/representation/family.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::representation {
namespace {

constexpr std::string_view kFamilyMagic = "CARTOGRAPHER_REPRESENTATION_FAMILY";
constexpr std::string_view kCertificationMagic =
    "CARTOGRAPHER_REPRESENTATION_CERTIFICATION";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool valid_purpose(RepresentationPurpose purpose) {
    switch (purpose) {
    case RepresentationPurpose::render:
    case RepresentationPurpose::collision:
    case RepresentationPurpose::navigation:
    case RepresentationPurpose::sensor:
    case RepresentationPurpose::shadow:
        return true;
    }
    return false;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_digest(std::istream& input, assets::Sha256Digest& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(token);
    if (!parsed) return false;
    value = parsed.value();
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

void write_variant(std::ostringstream& output, const RepresentationVariant& variant) {
    output << "VARIANT " << variant.id.value << ' '
           << representation_purpose_name(variant.purpose) << ' '
           << variant.error_envelope_digest.hex() << ' ' << variant.resident_bytes << ' '
           << variant.continuity.value << '\n';
}

bool read_variant(std::istream& input, RepresentationVariant& variant) {
    std::string purpose_name;
    if (!require_token(input, "VARIANT") || !read_integer(input, variant.id.value) ||
        !(input >> purpose_name) || !read_digest(input, variant.error_envelope_digest) ||
        !read_integer(input, variant.resident_bytes) ||
        !read_integer(input, variant.continuity.value)) {
        return false;
    }
    const auto purpose = representation_purpose_from_name(purpose_name);
    if (!purpose) return false;
    variant.purpose = purpose.value();
    return true;
}

bool require_end(std::istream& input) {
    if (!require_token(input, "END")) return false;
    std::string trailing;
    return !(input >> trailing);
}

} // namespace

std::string_view representation_purpose_name(RepresentationPurpose purpose) noexcept {
    switch (purpose) {
    case RepresentationPurpose::render: return "render";
    case RepresentationPurpose::collision: return "collision";
    case RepresentationPurpose::navigation: return "navigation";
    case RepresentationPurpose::sensor: return "sensor";
    case RepresentationPurpose::shadow: return "shadow";
    }
    return "unknown";
}

core::Result<RepresentationPurpose> representation_purpose_from_name(
    std::string_view name) {
    for (const auto purpose : {
             RepresentationPurpose::render,
             RepresentationPurpose::collision,
             RepresentationPurpose::navigation,
             RepresentationPurpose::sensor,
             RepresentationPurpose::shadow,
         }) {
        if (representation_purpose_name(purpose) == name) {
            return core::Result<RepresentationPurpose>::success(purpose);
        }
    }
    return core::Result<RepresentationPurpose>::failure(
        invalid("representation purpose name is unsupported"));
}

core::Result<void> AuthoredSourceIdentity::validate() const {
    if (!safe_text(source_namespace, RepresentationFamily::kMaxNamespaceBytes)) {
        return core::Result<void>::failure(invalid(
            "representation source namespace is empty, unsafe, or exceeds its bound"));
    }
    if (source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "representation source revision is exhausted"));
    }
    if (source_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "representation source digest must be bound"));
    }
    return core::Result<void>::success();
}

core::Result<void> RepresentationVariant::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "representation variant identity must be non-zero"));
    }
    if (!valid_purpose(purpose)) {
        return core::Result<void>::failure(invalid(
            "representation variant purpose is invalid"));
    }
    if (error_envelope_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "representation variant must reference a bound error envelope"));
    }
    if (!continuity) {
        return core::Result<void>::failure(validation(
            "representation variant continuity identity must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> RepresentationFamily::upsert_variant(
    RepresentationVariant variant) {
    if (auto result = variant.validate(); !result) return result;
    const auto existing = std::lower_bound(
        variants_.begin(), variants_.end(), variant.id,
        [](const RepresentationVariant& entry, RepresentationVariantId id) {
            return entry.id < id;
        });
    if (existing != variants_.end() && existing->id == variant.id) {
        if (existing->continuity != variant.continuity) {
            return core::Result<void>::failure(validation(
                "replacement variant must preserve the continuity identity"));
        }
        *existing = variant;
        return core::Result<void>::success();
    }
    if (std::any_of(variants_.begin(), variants_.end(), [&](const auto& entry) {
            return entry.continuity == variant.continuity;
        })) {
        return core::Result<void>::failure(invalid(
            "representation variant continuity identity is duplicated"));
    }
    if (variants_.size() >= kMaxVariantsPerFamily) {
        return core::Result<void>::failure(validation(
            "representation family has reached its bounded variant capacity"));
    }
    variants_.insert(existing, variant);
    return core::Result<void>::success();
}

const RepresentationVariant* RepresentationFamily::find_variant(
    RepresentationVariantId id) const noexcept {
    const auto found = std::lower_bound(
        variants_.begin(), variants_.end(), id,
        [](const RepresentationVariant& entry, RepresentationVariantId value) {
            return entry.id < value;
        });
    return found != variants_.end() && found->id == id ? &*found : nullptr;
}

const RepresentationVariant* RepresentationFamily::find_continuity(
    RepresentationContinuityId continuity) const noexcept {
    const auto found = std::find_if(
        variants_.begin(), variants_.end(), [&](const RepresentationVariant& entry) {
            return entry.continuity == continuity;
        });
    return found != variants_.end() ? &*found : nullptr;
}

core::Result<void> RepresentationFamily::validate() const {
    if (!id_) {
        return core::Result<void>::failure(invalid(
            "representation family identity must be non-zero"));
    }
    if (auto result = source_.validate(); !result) return result;
    if (variants_.size() > kMaxVariantsPerFamily) {
        return core::Result<void>::failure(validation(
            "representation family exceeds its bounded variant capacity"));
    }
    if (!std::is_sorted(variants_.begin(), variants_.end(),
                        [](const auto& left, const auto& right) {
                            return left.id < right.id;
                        }) ||
        std::adjacent_find(variants_.begin(), variants_.end(),
                           [](const auto& left, const auto& right) {
                               return left.id == right.id;
                           }) != variants_.end()) {
        return core::Result<void>::failure(validation(
            "representation variants must be sorted by identity and unique"));
    }
    for (std::size_t index = 0U; index < variants_.size(); ++index) {
        if (auto result = variants_[index].validate(); !result) return result;
        if (index > 0U && variants_[index - 1U].continuity == variants_[index].continuity) {
            return core::Result<void>::failure(validation(
                "representation variant continuity identities must be unique"));
        }
    }
    return core::Result<void>::success();
}

std::string RepresentationFamily::serialize() const {
    std::ostringstream output;
    output << kFamilyMagic << ' ' << kSchemaVersion << '\n';
    output << "FAMILY " << id_.value << '\n';
    output << "SOURCE " << std::quoted(source_.source_namespace) << ' '
           << source_.source_revision.value() << ' ' << source_.source_digest.hex()
           << '\n';
    output << "VARIANTS " << variants_.size() << '\n';
    for (const auto& variant : variants_) {
        write_variant(output, variant);
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> RepresentationFamily::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

core::Result<RepresentationFamily> RepresentationFamily::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kFamilyMagic) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family framing is invalid"));
    }
    std::uint32_t version = 0U;
    if (!read_integer(input, version)) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family version is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RepresentationFamily>::failure(version_mismatch(
            "representation family schema version is unsupported"));
    }

    RepresentationFamily family;
    if (!require_token(input, "FAMILY") || !read_integer(input, family.id_.value)) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family identity is malformed"));
    }
    std::uint64_t revision = 0U;
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(family.source_.source_namespace)) ||
        !read_integer(input, revision) ||
        !read_digest(input, family.source_.source_digest)) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family source record is malformed"));
    }
    family.source_.source_revision = core::Revision{revision};

    std::size_t variant_count = 0U;
    if (!require_token(input, "VARIANTS") ||
        !read_count(input, kMaxVariantsPerFamily, variant_count)) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family variant header is invalid"));
    }
    for (std::size_t index = 0U; index < variant_count; ++index) {
        RepresentationVariant variant;
        if (!read_variant(input, variant)) {
            return core::Result<RepresentationFamily>::failure(parse_error(
                "representation variant record is malformed"));
        }
        if (auto result = family.upsert_variant(std::move(variant)); !result) {
            return core::Result<RepresentationFamily>::failure(result.error());
        }
    }
    if (!require_end(input)) {
        return core::Result<RepresentationFamily>::failure(parse_error(
            "representation family is missing END or contains trailing data"));
    }
    if (auto result = family.validate(); !result) {
        return core::Result<RepresentationFamily>::failure(result.error());
    }
    return core::Result<RepresentationFamily>::success(std::move(family));
}

core::Result<void> RepresentationCertification::validate() const {
    if (!family || !variant) {
        return core::Result<void>::failure(invalid(
            "representation certification family or variant identity is zero"));
    }
    if (certified_source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "representation certification source revision is exhausted"));
    }
    if (selection_evidence_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "representation certification must bind selection evidence"));
    }
    if (!safe_text(algorithm, kMaxAlgorithmBytes)) {
        return core::Result<void>::failure(invalid(
            "representation certification algorithm is empty, unsafe, or exceeds its bound"));
    }
    if (digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "representation certification digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string RepresentationCertification::canonical_without_digest() const {
    std::ostringstream output;
    output << "representation-certification-v1{family=" << family.value
           << ";variant=" << variant.value
           << ";source-revision=" << certified_source_revision.value()
           << ";evidence=" << selection_evidence_digest.hex()
           << ";algorithm=" << algorithm << '}';
    return output.str();
}

core::Result<void> RepresentationCertification::refresh_digest() {
    if (!family || !variant || certified_source_revision.exhausted() ||
        selection_evidence_digest.is_zero() ||
        !safe_text(algorithm, kMaxAlgorithmBytes)) {
        return core::Result<void>::failure(validation(
            "representation certification cannot refresh an incomplete digest"));
    }
    digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string RepresentationCertification::serialize() const {
    std::ostringstream output;
    output << kCertificationMagic << ' ' << kSchemaVersion << '\n';
    output << "FAMILY " << family.value << '\n';
    output << "VARIANT " << variant.value << '\n';
    output << "SOURCE_REVISION " << certified_source_revision.value() << '\n';
    output << "SELECTION_EVIDENCE " << selection_evidence_digest.hex() << '\n';
    output << "ALGORITHM " << std::quoted(algorithm) << '\n';
    output << "DIGEST " << digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<RepresentationCertification> RepresentationCertification::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RepresentationCertification>::failure(parse_error(
            "representation certification text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kCertificationMagic) {
        return core::Result<RepresentationCertification>::failure(parse_error(
            "representation certification framing is invalid"));
    }
    std::uint32_t version = 0U;
    if (!read_integer(input, version)) {
        return core::Result<RepresentationCertification>::failure(parse_error(
            "representation certification version is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RepresentationCertification>::failure(version_mismatch(
            "representation certification schema version is unsupported"));
    }

    RepresentationCertification certification;
    std::uint64_t revision = 0U;
    if (!require_token(input, "FAMILY") || !read_integer(input, certification.family.value) ||
        !require_token(input, "VARIANT") ||
        !read_integer(input, certification.variant.value) ||
        !require_token(input, "SOURCE_REVISION") || !read_integer(input, revision) ||
        !require_token(input, "SELECTION_EVIDENCE") ||
        !read_digest(input, certification.selection_evidence_digest) ||
        !require_token(input, "ALGORITHM") ||
        !(input >> std::quoted(certification.algorithm)) ||
        !require_token(input, "DIGEST") || !read_digest(input, certification.digest) ||
        !require_end(input)) {
        return core::Result<RepresentationCertification>::failure(parse_error(
            "representation certification record is malformed"));
    }
    certification.certified_source_revision = core::Revision{revision};
    if (auto result = certification.validate(); !result) {
        return core::Result<RepresentationCertification>::failure(result.error());
    }
    return core::Result<RepresentationCertification>::success(std::move(certification));
}

} // namespace carto::representation
