#include <carto/semantic/lowering.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::semantic {
namespace {

constexpr std::string_view kLoweringMagic = "CARTOGRAPHER_TAG_LOWERING";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

assets::Sha256Digest digest_of(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool read_uint(std::istream& input, std::uint64_t& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
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

bool read_count(std::istream& input, std::uint64_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_uint(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

} // namespace

core::Result<void> LoweredTagBinding::validate() const {
    if (!safe_text(authored_tag, kMaxTagBytes)) {
        return core::Result<void>::failure(invalid(
            "lowered tag binding authored tag is invalid"));
    }
    if (!safe_text(lowered_tag, kMaxTagBytes)) {
        return core::Result<void>::failure(invalid(
            "lowered tag binding lowered tag is invalid"));
    }
    if (!safe_text(transformation_evidence, kMaxEvidenceBytes)) {
        return core::Result<void>::failure(invalid(
            "lowered tag binding transformation evidence is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> TagLoweringMap::insert_binding(LoweredTagBinding binding) {
    if (bindings_.size() >= kMaxBindingsPerMap) {
        return core::Result<void>::failure(validation(
            "tag lowering map has reached its bounded capacity"));
    }
    if (auto result = binding.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "tag lowering admission"));
    }
    const auto position = std::lower_bound(
        bindings_.begin(), bindings_.end(), binding.authored_tag,
        [](const LoweredTagBinding& existing, const std::string& tag) {
            return existing.authored_tag < tag;
        });
    if (position != bindings_.end() && position->authored_tag == binding.authored_tag) {
        return core::Result<void>::failure(validation(
            "tag lowering map already binds this authored tag"));
    }
    bindings_.insert(position, std::move(binding));
    return core::Result<void>::success();
}

const LoweredTagBinding* TagLoweringMap::find(
    std::string_view authored_tag) const noexcept {
    const auto position = std::lower_bound(
        bindings_.begin(), bindings_.end(), authored_tag,
        [](const LoweredTagBinding& existing, const std::string_view tag) {
            return existing.authored_tag < tag;
        });
    if (position == bindings_.end() || position->authored_tag != authored_tag) {
        return nullptr;
    }
    return &*position;
}

core::Result<const LoweredTagBinding*> TagLoweringMap::binding_for(
    std::string_view authored_tag) const {
    const auto* binding = find(authored_tag);
    if (binding == nullptr) {
        return core::Result<const LoweredTagBinding*>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "authored tag has no lowering binding"));
    }
    return core::Result<const LoweredTagBinding*>::success(binding);
}

core::Result<void> TagLoweringMap::verify_against(const SemanticSidecar& sidecar) const {
    const auto sidecar_digest = sidecar.canonical_digest();
    if (!sidecar_digest) {
        return core::Result<void>::failure(sidecar_digest.error().with_context(
            "tag lowering sidecar verification"));
    }
    if (bound_sidecar_digest_.is_zero() ||
        bound_sidecar_digest_ != sidecar_digest.value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "tag lowering map is bound to a different sidecar digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> TagLoweringMap::validate() const {
    if (bound_sidecar_digest_.is_zero()) {
        return core::Result<void>::failure(validation(
            "tag lowering map is not bound to a sidecar digest"));
    }
    if (bindings_.size() > kMaxBindingsPerMap) {
        return core::Result<void>::failure(validation(
            "tag lowering map exceeds its bounded capacity"));
    }
    for (const auto& binding : bindings_) {
        if (auto result = binding.validate(); !result) return result;
    }
    if (!std::is_sorted(bindings_.begin(), bindings_.end(),
            [](const LoweredTagBinding& left, const LoweredTagBinding& right) {
                return left.authored_tag < right.authored_tag;
            }) ||
        std::adjacent_find(bindings_.begin(), bindings_.end(),
            [](const LoweredTagBinding& left, const LoweredTagBinding& right) {
                return left.authored_tag == right.authored_tag;
            }) != bindings_.end()) {
        return core::Result<void>::failure(validation(
            "tag lowering bindings must be sorted and unique by authored tag"));
    }
    return core::Result<void>::success();
}

std::string TagLoweringMap::serialize() const {
    std::ostringstream output;
    output << kLoweringMagic << ' ' << kSchemaVersion << '\n';
    output << "SIDECAR_DIGEST " << bound_sidecar_digest_.hex() << '\n';
    output << "BINDINGS " << bindings_.size() << '\n';
    for (const auto& binding : bindings_) {
        output << "BINDING " << std::quoted(binding.authored_tag) << ' '
               << std::quoted(binding.lowered_tag) << ' '
               << std::quoted(binding.transformation_evidence) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<TagLoweringMap> TagLoweringMap::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<TagLoweringMap>::failure(parse_error(
            "tag lowering map text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint64_t version = 0U;
    if (!(input >> magic) || magic != kLoweringMagic || !read_uint(input, version) ||
        version != kSchemaVersion) {
        return core::Result<TagLoweringMap>::failure(parse_error(
            "tag lowering map framing or schema version is invalid"));
    }
    TagLoweringMap lowering;
    if (!require_token(input, "SIDECAR_DIGEST") ||
        !read_digest(input, lowering.bound_sidecar_digest_)) {
        return core::Result<TagLoweringMap>::failure(parse_error(
            "tag lowering map sidecar digest is malformed"));
    }
    std::size_t binding_count = 0U;
    if (!require_token(input, "BINDINGS") ||
        !read_count(input, TagLoweringMap::kMaxBindingsPerMap, binding_count)) {
        return core::Result<TagLoweringMap>::failure(parse_error(
            "tag lowering map binding header is malformed"));
    }
    for (std::size_t index = 0U; index < binding_count; ++index) {
        LoweredTagBinding binding;
        if (!require_token(input, "BINDING") ||
            !(input >> std::quoted(binding.authored_tag) >>
              std::quoted(binding.lowered_tag) >>
              std::quoted(binding.transformation_evidence))) {
            return core::Result<TagLoweringMap>::failure(parse_error(
                "tag lowering map binding is malformed"));
        }
        if (auto result = lowering.insert_binding(std::move(binding)); !result) {
            return core::Result<TagLoweringMap>::failure(result.error());
        }
    }
    if (!require_token(input, "END")) {
        return core::Result<TagLoweringMap>::failure(parse_error(
            "tag lowering map is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<TagLoweringMap>::failure(parse_error(
            "tag lowering map contains trailing unbound data"));
    }
    if (auto result = lowering.validate(); !result) {
        return core::Result<TagLoweringMap>::failure(result.error());
    }
    return core::Result<TagLoweringMap>::success(std::move(lowering));
}

core::Result<assets::Sha256Digest> TagLoweringMap::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

} // namespace carto::semantic
