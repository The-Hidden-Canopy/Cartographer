#include <carto/representation/continuity.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <istream>
#include <sstream>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::representation {
namespace {

constexpr std::string_view kContinuityMagic = "CARTOGRAPHER_REPRESENTATION_CONTINUITY";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic version_mismatch(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
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

} // namespace

core::Result<void> RepresentationContinuityBinding::validate() const {
    if (!family || !continuity) {
        return core::Result<void>::failure(invalid(
            "continuity binding family or continuity identity is zero"));
    }
    if (source_revision.exhausted()) {
        return core::Result<void>::failure(validation(
            "continuity binding source revision is exhausted"));
    }
    if (family_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "continuity binding must bind the family digest"));
    }
    return core::Result<void>::success();
}

std::string RepresentationContinuityBinding::canonical() const {
    std::ostringstream output;
    output << "representation-continuity-v1{family=" << family.value
           << ";continuity=" << continuity.value
           << ";source-revision=" << source_revision.value()
           << ";family-digest=" << family_digest.hex() << '}';
    return output.str();
}

core::Result<assets::Sha256Digest> RepresentationContinuityBinding::canonical_digest()
    const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical()));
}

std::string RepresentationContinuityBinding::serialize() const {
    std::ostringstream output;
    output << kContinuityMagic << ' ' << kSchemaVersion << '\n';
    output << "FAMILY " << family.value << '\n';
    output << "CONTINUITY " << continuity.value << '\n';
    output << "SOURCE_REVISION " << source_revision.value() << '\n';
    output << "FAMILY_DIGEST " << family_digest.hex() << '\n';
    output << "END\n";
    return output.str();
}

core::Result<RepresentationContinuityBinding> RepresentationContinuityBinding::
deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<RepresentationContinuityBinding>::failure(parse_error(
            "continuity binding text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    if (!(input >> magic) || magic != kContinuityMagic) {
        return core::Result<RepresentationContinuityBinding>::failure(parse_error(
            "continuity binding framing is invalid"));
    }
    std::uint32_t version = 0U;
    if (!read_integer(input, version)) {
        return core::Result<RepresentationContinuityBinding>::failure(parse_error(
            "continuity binding version is malformed"));
    }
    if (version != kSchemaVersion) {
        return core::Result<RepresentationContinuityBinding>::failure(version_mismatch(
            "continuity binding schema version is unsupported"));
    }
    RepresentationContinuityBinding binding;
    std::uint64_t revision = 0U;
    if (!require_token(input, "FAMILY") || !read_integer(input, binding.family.value) ||
        !require_token(input, "CONTINUITY") ||
        !read_integer(input, binding.continuity.value) ||
        !require_token(input, "SOURCE_REVISION") || !read_integer(input, revision) ||
        !require_token(input, "FAMILY_DIGEST") ||
        !read_digest(input, binding.family_digest)) {
        return core::Result<RepresentationContinuityBinding>::failure(parse_error(
            "continuity binding record is malformed"));
    }
    binding.source_revision = core::Revision{revision};
    std::string end_token;
    std::string trailing;
    if (!(input >> end_token) || end_token != "END" || (input >> trailing)) {
        return core::Result<RepresentationContinuityBinding>::failure(parse_error(
            "continuity binding is missing END or contains trailing data"));
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<RepresentationContinuityBinding>::failure(result.error());
    }
    return core::Result<RepresentationContinuityBinding>::success(std::move(binding));
}

core::Result<void> verify_reimport_continuity(
    const RepresentationFamily& previous,
    const RepresentationFamily& reimported) {
    if (auto result = previous.validate(); !result) return result;
    if (auto result = reimported.validate(); !result) return result;
    if (reimported.id() != previous.id()) {
        return core::Result<void>::failure(stale(
            "reimport changed the stable family identity"));
    }
    if (reimported.source().source_namespace != previous.source().source_namespace) {
        return core::Result<void>::failure(stale(
            "reimport changed the authored source namespace"));
    }
    if (reimported.source().source_revision < previous.source().source_revision) {
        return core::Result<void>::failure(stale(
            "reimport regressed the source revision"));
    }
    if (reimported.source().source_digest != previous.source().source_digest &&
        reimported.source().source_revision == previous.source().source_revision) {
        return core::Result<void>::failure(stale(
            "reimport changed the source digest without advancing the revision"));
    }
    for (const auto& variant : previous.variants()) {
        const auto* replacement = reimported.find_continuity(variant.continuity);
        if (replacement == nullptr) {
            return core::Result<void>::failure(validation(
                "reimport dropped a continuity identity"));
        }
        if (replacement->purpose != variant.purpose) {
            return core::Result<void>::failure(validation(
                "reimport changed the purpose bound to a continuity identity"));
        }
    }
    return core::Result<void>::success();
}

core::Result<RepresentationContinuityBinding> bind_reimport_continuity(
    const RepresentationFamily& previous,
    const RepresentationFamily& reimported,
    RepresentationContinuityId continuity) {
    if (!continuity) {
        return core::Result<RepresentationContinuityBinding>::failure(invalid(
            "continuity binding requires a non-zero continuity identity"));
    }
    if (auto result = verify_reimport_continuity(previous, reimported); !result) {
        return core::Result<RepresentationContinuityBinding>::failure(result.error());
    }
    if (previous.find_continuity(continuity) == nullptr ||
        reimported.find_continuity(continuity) == nullptr) {
        return core::Result<RepresentationContinuityBinding>::failure(validation(
            "continuity identity is unknown to the previous or reimported family"));
    }
    const auto family_digest = reimported.canonical_digest();
    if (!family_digest) {
        return core::Result<RepresentationContinuityBinding>::failure(
            family_digest.error());
    }
    RepresentationContinuityBinding binding;
    binding.family = reimported.id();
    binding.continuity = continuity;
    binding.source_revision = reimported.source().source_revision;
    binding.family_digest = family_digest.value();
    if (auto result = binding.validate(); !result) {
        return core::Result<RepresentationContinuityBinding>::failure(result.error());
    }
    return core::Result<RepresentationContinuityBinding>::success(std::move(binding));
}

} // namespace carto::representation
