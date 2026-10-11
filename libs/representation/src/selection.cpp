#include <carto/representation/selection.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::representation {
namespace {

constexpr std::string_view kSelectionMagic = "CARTOGRAPHER_REPRESENTATION_SELECTION";
constexpr std::size_t kMaxAlgorithmBytes = 128U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
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

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool finite_nonnegative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
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

bool envelope_satisfies(const RepresentationErrorEnvelope& envelope,
                        const RepresentationCapabilityRequirement& requirement) {
    return envelope.max_position_error <= requirement.max_position_error &&
        envelope.max_silhouette_error <= requirement.max_silhouette_error &&
        envelope.max_normal_error <= requirement.max_normal_error &&
        envelope.max_radiance_error <= requirement.max_radiance_error;
}

} // namespace

core::Result<void> HardwareProfileCapability::validate() const {
    if (!safe_text(profile_identity, kMaxProfileIdentityBytes)) {
        return core::Result<void>::failure(invalid(
            "hardware profile identity is empty, unsafe, or exceeds its bound"));
    }
    if (profile_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "hardware profile capability must bind a profile digest"));
    }
    if (!std::is_sorted(supported_purposes.begin(), supported_purposes.end(),
                        [](RepresentationPurpose left, RepresentationPurpose right) {
                            return static_cast<std::uint8_t>(left) <
                                static_cast<std::uint8_t>(right);
                        }) ||
        std::adjacent_find(supported_purposes.begin(), supported_purposes.end(),
                           [](RepresentationPurpose left, RepresentationPurpose right) {
                               return left == right;
                           }) != supported_purposes.end()) {
        return core::Result<void>::failure(validation(
            "hardware profile purposes must be sorted and unique"));
    }
    if (!std::all_of(supported_purposes.begin(), supported_purposes.end(),
                     valid_purpose)) {
        return core::Result<void>::failure(invalid(
            "hardware profile purpose is invalid"));
    }
    return core::Result<void>::success();
}

bool HardwareProfileCapability::supports(RepresentationPurpose purpose)
    const noexcept {
    return std::binary_search(
        supported_purposes.begin(), supported_purposes.end(), purpose,
        [](RepresentationPurpose left, RepresentationPurpose right) {
            return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right);
        });
}

std::string HardwareProfileCapability::canonical() const {
    std::ostringstream output;
    output << "hardware-profile-v1{identity=" << profile_identity
           << ";digest=" << profile_digest.hex() << ";purposes=[";
    for (std::size_t index = 0U; index < supported_purposes.size(); ++index) {
        if (index > 0U) output << ',';
        output << representation_purpose_name(supported_purposes[index]);
    }
    output << "];resident-budget=" << resident_budget_bytes << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> HardwareProfileCapability::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> RepresentationCapabilityRequirement::validate() const {
    if (!valid_purpose(purpose)) {
        return core::Result<void>::failure(invalid(
            "capability requirement purpose is invalid"));
    }
    if (!finite_nonnegative(max_position_error) ||
        !finite_nonnegative(max_silhouette_error) ||
        !finite_nonnegative(max_normal_error) ||
        !finite_nonnegative(max_radiance_error)) {
        return core::Result<void>::failure(validation(
            "capability requirement contains a non-finite or negative bound"));
    }
    return core::Result<void>::success();
}

std::string RepresentationCapabilityRequirement::canonical() const {
    std::ostringstream output;
    output << "capability-requirement-v1{purpose="
           << representation_purpose_name(purpose) << ";position="
           << std::setprecision(17) << max_position_error << ";silhouette="
           << max_silhouette_error << ";normal=" << max_normal_error
           << ";radiance=" << max_radiance_error << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> RepresentationCapabilityRequirement::
canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

core::Result<void> RepresentationVariantEnvelope::validate() const {
    if (!variant) {
        return core::Result<void>::failure(invalid(
            "variant envelope reference has a zero variant identity"));
    }
    return envelope.validate();
}

core::Result<void> RepresentationSelection::validate() const {
    if (!family || !selected_variant) {
        return core::Result<void>::failure(invalid(
            "representation selection family or variant identity is zero"));
    }
    if (!valid_purpose(purpose)) {
        return core::Result<void>::failure(invalid(
            "representation selection purpose is invalid"));
    }
    if (!safe_text(profile_identity, HardwareProfileCapability::kMaxProfileIdentityBytes)) {
        return core::Result<void>::failure(invalid(
            "representation selection profile identity is empty, unsafe, or exceeds its bound"));
    }
    if (profile_digest.is_zero() || requirement_digest.is_zero() ||
        capability_digest.is_zero() || family_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "representation selection must bind profile, requirement, capability, and family digests"));
    }
    if (!safe_text(algorithm, kMaxAlgorithmBytes)) {
        return core::Result<void>::failure(invalid(
            "representation selection algorithm is empty, unsafe, or exceeds its bound"));
    }
    if (selection_digest != digest_text(canonical_without_digest())) {
        return core::Result<void>::failure(validation(
            "representation selection digest does not match its content"));
    }
    return core::Result<void>::success();
}

std::string RepresentationSelection::canonical_without_digest() const {
    std::ostringstream output;
    output << "representation-selection-v1{family=" << family.value
           << ";variant=" << selected_variant.value
           << ";purpose=" << representation_purpose_name(purpose)
           << ";profile=" << profile_identity
           << ";profile-digest=" << profile_digest.hex()
           << ";requirement-digest=" << requirement_digest.hex()
           << ";capability-digest=" << capability_digest.hex()
           << ";family-digest=" << family_digest.hex()
           << ";algorithm=" << algorithm << '}';
    return output.str();
}

core::Result<void> RepresentationSelection::refresh_digest() {
    if (!family || !selected_variant ||
        !safe_text(profile_identity, HardwareProfileCapability::kMaxProfileIdentityBytes) ||
        profile_digest.is_zero() || requirement_digest.is_zero() ||
        capability_digest.is_zero() || family_digest.is_zero() ||
        !safe_text(algorithm, kMaxAlgorithmBytes)) {
        return core::Result<void>::failure(validation(
            "representation selection cannot refresh an incomplete digest"));
    }
    selection_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

std::string RepresentationSelection::serialize() const {
    std::ostringstream output;
    output << kSelectionMagic << ' ' << kSchemaVersion << '\n';
    output << "FAMILY " << family.value << '\n';
    output << "VARIANT " << selected_variant.value << '\n';
    output << "PURPOSE " << representation_purpose_name(purpose) << '\n';
    output << "PROFILE " << std::quoted(profile_identity) << ' '
           << profile_digest.hex() << '\n';
    output << "REQUIREMENT_DIGEST " << requirement_digest.hex() << '\n';
    output << "CAPABILITY_DIGEST " << capability_digest.hex() << '\n';
    output << "FAMILY_DIGEST " << family_digest.hex() << '\n';
    output << "ALGORITHM " << std::quoted(algorithm) << '\n';
    output << "SELECTION_DIGEST " << selection_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<RepresentationSelection> RepresentationSelection::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kSelectionMagic) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection framing is invalid"));
    }
    std::uint32_t version = 0U;
    if (!read_integer(input, version)) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection version is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RepresentationSelection>::failure(version_mismatch(
            "representation selection schema version is unsupported"));
    }

    RepresentationSelection selection;
    std::string purpose_name;
    if (!require_token(input, "FAMILY") || !read_integer(input, selection.family.value) ||
        !require_token(input, "VARIANT") ||
        !read_integer(input, selection.selected_variant.value) ||
        !require_token(input, "PURPOSE") || !(input >> purpose_name)) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection identity record is malformed"));
    }
    const auto purpose = representation_purpose_from_name(purpose_name);
    if (!purpose) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection purpose is unsupported"));
    }
    selection.purpose = purpose.value();
    if (!require_token(input, "PROFILE") ||
        !(input >> std::quoted(selection.profile_identity)) ||
        !read_digest(input, selection.profile_digest) ||
        !require_token(input, "REQUIREMENT_DIGEST") ||
        !read_digest(input, selection.requirement_digest) ||
        !require_token(input, "CAPABILITY_DIGEST") ||
        !read_digest(input, selection.capability_digest) ||
        !require_token(input, "FAMILY_DIGEST") ||
        !read_digest(input, selection.family_digest) ||
        !require_token(input, "ALGORITHM") ||
        !(input >> std::quoted(selection.algorithm)) ||
        !require_token(input, "SELECTION_DIGEST") ||
        !read_digest(input, selection.selection_digest)) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection digest record is malformed"));
    }
    std::string end_token;
    std::string trailing;
    if (!(input >> end_token) || end_token != "END" || (input >> trailing)) {
        return core::Result<RepresentationSelection>::failure(parse_error(
            "representation selection is missing END or contains trailing data"));
    }
    if (auto result = selection.validate(); !result) {
        return core::Result<RepresentationSelection>::failure(result.error());
    }
    return core::Result<RepresentationSelection>::success(std::move(selection));
}

core::Result<RepresentationSelection> select_variant(
    const RepresentationFamily& family,
    const HardwareProfileCapability& capability,
    const RepresentationCapabilityRequirement& requirement,
    std::span<const RepresentationCertification> certifications,
    std::span<const RepresentationVariantEnvelope> envelopes) {
    if (auto result = family.validate(); !result) {
        return core::Result<RepresentationSelection>::failure(result.error());
    }
    if (auto result = capability.validate(); !result) {
        return core::Result<RepresentationSelection>::failure(result.error());
    }
    if (auto result = requirement.validate(); !result) {
        return core::Result<RepresentationSelection>::failure(result.error());
    }

    if (!capability.supports(requirement.purpose)) {
        return core::Result<RepresentationSelection>::failure(unsupported(
            std::string("hardware profile does not support purpose ") +
            std::string(representation_purpose_name(requirement.purpose)) +
            "; refusing selection instead of falling back"));
    }

    std::vector<RepresentationVariantId> eligible;
    if (!certifications.empty()) {
        for (const auto& certification : certifications) {
            if (auto result = certification.validate(); !result) {
                return core::Result<RepresentationSelection>::failure(result.error());
            }
            if (certification.family != family.id() ||
                certification.certified_source_revision !=
                    family.source().source_revision) {
                return core::Result<RepresentationSelection>::failure(validation(
                    "certification does not match the family identity or source revision"));
            }
            const auto* variant = family.find_variant(certification.variant);
            if (variant == nullptr) {
                return core::Result<RepresentationSelection>::failure(validation(
                    "certification references a variant outside the family"));
            }
            if (std::find(eligible.begin(), eligible.end(), certification.variant) ==
                eligible.end()) {
                eligible.push_back(certification.variant);
            }
        }
    }

    const RepresentationVariant* best = nullptr;
    for (const auto& variant : family.variants()) {
        if (variant.purpose != requirement.purpose) continue;
        if (variant.resident_bytes > capability.resident_budget_bytes) continue;
        if (!certifications.empty() &&
            std::find(eligible.begin(), eligible.end(), variant.id) == eligible.end()) {
            continue;
        }
        const auto envelope_entry = std::find_if(
            envelopes.begin(), envelopes.end(), [&](const auto& entry) {
                return entry.variant == variant.id;
            });
        if (envelope_entry == envelopes.end()) {
            return core::Result<RepresentationSelection>::failure(validation(
                "eligible variant error envelope is not resolved"));
        }
        if (auto result = envelope_entry->validate(); !result) {
            return core::Result<RepresentationSelection>::failure(result.error());
        }
        if (!envelope_satisfies(envelope_entry->envelope, requirement)) continue;
        if (best == nullptr || variant.resident_bytes < best->resident_bytes) {
            best = &variant;
        }
    }

    if (best == nullptr) {
        return core::Result<RepresentationSelection>::failure(validation(
            "no representation variant satisfies the purpose, error, and resident "
            "budgets; explicit refusal without fallback"));
    }

    const auto family_digest = family.canonical_digest();
    if (!family_digest) {
        return core::Result<RepresentationSelection>::failure(family_digest.error());
    }
    const auto requirement_digest = requirement.canonical_digest();
    if (!requirement_digest) {
        return core::Result<RepresentationSelection>::failure(requirement_digest.error());
    }
    const auto capability_digest = capability.canonical_digest();
    if (!capability_digest) {
        return core::Result<RepresentationSelection>::failure(capability_digest.error());
    }

    RepresentationSelection selection;
    selection.family = family.id();
    selection.selected_variant = best->id;
    selection.purpose = requirement.purpose;
    selection.profile_identity = capability.profile_identity;
    selection.profile_digest = capability.profile_digest;
    selection.requirement_digest = requirement_digest.value();
    selection.capability_digest = capability_digest.value();
    selection.family_digest = family_digest.value();
    if (auto result = selection.refresh_digest(); !result) {
        return core::Result<RepresentationSelection>::failure(result.error());
    }
    return core::Result<RepresentationSelection>::success(std::move(selection));
}

} // namespace carto::representation
