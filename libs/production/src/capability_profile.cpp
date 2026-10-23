#include <carto/production/capability_profile.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <type_traits>
#include <utility>

namespace carto::production {

namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_DESTINATION_CAPABILITIES";
constexpr std::size_t kMaxIdentityBytes = 192U;
constexpr std::size_t kMaxDetailBytes = 2'048U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-' || byte == ':' || byte == '/';
        });
}

bool valid_evidence(PreparedCapabilityEvidence evidence) {
    // Unsupported evidence is expressed by absence from the profile.
    return evidence == PreparedCapabilityEvidence::preserved_source ||
        evidence == PreparedCapabilityEvidence::derived_product ||
        evidence == PreparedCapabilityEvidence::native_realization ||
        evidence == PreparedCapabilityEvidence::runtime_validated;
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
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

bool read_count(std::istream& input, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > kMaxDestinationCapabilities ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

} // namespace

core::Result<void> DestinationCapability::validate() const {
    if (!safe_identity(feature) || !valid_evidence(evidence) ||
        !safe_text(detail, kMaxDetailBytes)) {
        return core::Result<void>::failure(invalid(
            "destination capability feature, evidence, or detail is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> DestinationCapabilityProfile::validate() const {
    if (!safe_identity(identity) || profile_revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "destination capability profile identity or revision is invalid"));
    }
    if (capabilities.size() > kMaxCapabilities) {
        return core::Result<void>::failure(validation(
            "destination capability profile exceeds its bounded capacity"));
    }
    for (std::size_t index = 0U; index < capabilities.size(); ++index) {
        if (auto result = capabilities[index].validate(); !result) return result;
        if (index > 0U && !(capabilities[index - 1U].feature < capabilities[index].feature)) {
            return core::Result<void>::failure(validation(
                "destination capabilities must be canonically sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

std::string DestinationCapabilityProfile::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "PROFILE " << std::quoted(identity) << ' '
           << profile_revision.value() << '\n';
    output << "CAPABILITIES " << capabilities.size() << '\n';
    for (const auto& capability : capabilities) {
        output << "CAPABILITY " << std::quoted(capability.feature) << ' '
               << static_cast<unsigned>(capability.evidence) << ' '
               << std::quoted(capability.detail) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<DestinationCapabilityProfile> DestinationCapabilityProfile::deserialize(
    std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<DestinationCapabilityProfile>::failure(validation(
            "destination capability profile exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kMagic) || !read_unsigned(input, version)) {
        return core::Result<DestinationCapabilityProfile>::failure(validation(
            "destination capability profile header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<DestinationCapabilityProfile>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "destination capability profile schema version is unsupported"));
    }

    DestinationCapabilityProfile profile;
    std::uint64_t revision = 0U;
    if (!require_token(input, "PROFILE") ||
        !(input >> std::quoted(profile.identity)) ||
        !read_unsigned(input, revision)) {
        return core::Result<DestinationCapabilityProfile>::failure(validation(
            "destination capability profile identity or revision is invalid"));
    }
    profile.profile_revision = core::Revision{revision};

    std::size_t count = 0U;
    if (!require_token(input, "CAPABILITIES") || !read_count(input, count)) {
        return core::Result<DestinationCapabilityProfile>::failure(validation(
            "destination capability profile count is invalid"));
    }
    profile.capabilities.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        DestinationCapability capability;
        unsigned evidence = 0U;
        if (!require_token(input, "CAPABILITY") ||
            !(input >> std::quoted(capability.feature)) ||
            !read_unsigned(input, evidence) ||
            evidence > static_cast<unsigned>(PreparedCapabilityEvidence::unsupported) ||
            !(input >> std::quoted(capability.detail))) {
            return core::Result<DestinationCapabilityProfile>::failure(validation(
                "destination capability record is invalid"));
        }
        capability.evidence = static_cast<PreparedCapabilityEvidence>(evidence);
        profile.capabilities.push_back(std::move(capability));
    }
    if (!require_token(input, "END")) {
        return core::Result<DestinationCapabilityProfile>::failure(validation(
            "destination capability profile is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<DestinationCapabilityProfile>::failure(validation(
            "destination capability profile contains trailing data"));
    }
    if (auto result = profile.validate(); !result) {
        return core::Result<DestinationCapabilityProfile>::failure(result.error());
    }
    return core::Result<DestinationCapabilityProfile>::success(std::move(profile));
}

core::Result<assets::Sha256Digest> DestinationCapabilityProfile::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

const DestinationCapability* DestinationCapabilityProfile::find(
    std::string_view feature) const noexcept {
    const auto found = std::lower_bound(
        capabilities.begin(), capabilities.end(), feature,
        [](const DestinationCapability& capability, std::string_view name) {
            return capability.feature < name;
        });
    if (found == capabilities.end() || found->feature != feature) return nullptr;
    return &*found;
}

core::Result<void> RequestedCapability::validate() const {
    if (!safe_identity(feature)) {
        return core::Result<void>::failure(invalid(
            "requested capability feature is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<PreparedCapabilityResult>> check_destination_capabilities(
    const DestinationCapabilityProfile& profile,
    std::span<const RequestedCapability> requested) {
    if (auto result = profile.validate(); !result) {
        return core::Result<std::vector<PreparedCapabilityResult>>::failure(result.error());
    }
    for (const auto& request : requested) {
        if (auto result = request.validate(); !result) {
            return core::Result<std::vector<PreparedCapabilityResult>>::failure(result.error());
        }
    }
    std::vector<PreparedCapabilityResult> results;
    results.reserve(requested.size());
    for (const auto& request : requested) {
        const auto* offered = profile.find(request.feature);
        PreparedCapabilityResult result;
        result.feature = request.feature;
        if (offered == nullptr) {
            result.evidence = PreparedCapabilityEvidence::unsupported;
            result.required = false;
            result.detail = "feature not offered by destination profile " +
                profile.identity +
                (request.required ? "; required demand must be refused" :
                                    "; optional demand may be skipped");
        } else {
            result.evidence = offered->evidence;
            result.required = request.required;
            result.detail = offered->detail;
        }
        results.push_back(std::move(result));
    }
    return core::Result<std::vector<PreparedCapabilityResult>>::success(std::move(results));
}

} // namespace carto::production
