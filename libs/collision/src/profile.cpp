#include <carto/collision/profile.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <system_error>

namespace carto::collision {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_COLLISION_PROFILE";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

bool valid_family(CollisionRepresentationFamily family) {
    switch (family) {
    case CollisionRepresentationFamily::primitive:
    case CollisionRepresentationFamily::compound:
    case CollisionRepresentationFamily::convex_proxy:
    case CollisionRepresentationFamily::triangle_mesh:
        return true;
    }
    return false;
}

PrimitiveCollisionKind parse_primitive_kind(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(PrimitiveCollisionKind::cylinder);
         ++value) {
        const auto kind = static_cast<PrimitiveCollisionKind>(value);
        if (primitive_collision_kind_name(kind) == text) return kind;
    }
    return static_cast<PrimitiveCollisionKind>(0xFFU);
}

CollisionRepresentationFamily parse_family(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(CollisionRepresentationFamily::triangle_mesh);
         ++value) {
        const auto family = static_cast<CollisionRepresentationFamily>(value);
        if (collision_representation_family_name(family) == text) return family;
    }
    return static_cast<CollisionRepresentationFamily>(0xFFU);
}

CollisionMotionKind parse_motion(const std::string& text) {
    for (std::uint8_t value = 0U;
         value <= static_cast<std::uint8_t>(CollisionMotionKind::kinematic);
         ++value) {
        const auto kind = static_cast<CollisionMotionKind>(value);
        if (collision_motion_kind_name(kind) == text) return kind;
    }
    return static_cast<CollisionMotionKind>(0xFFU);
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

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
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

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_integer(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

bool read_thresholds(std::istream& input, CollisionErrorThresholdPolicy& policy) {
    const auto read_optional_double = [&input](std::optional<double>& bound) {
        input >> std::ws;
        const int next = input.peek();
        if (next == std::char_traits<char>::eof()) return false;
        if (next == '-') {
            input.get();
            bound = std::nullopt;
            return true;
        }
        std::string token;
        if (!(input >> token)) return false;
        double parsed = 0.0;
        const auto result =
            std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return false;
        }
        bound = parsed;
        return true;
    };
    return require_token(input, "THRESHOLDS") &&
        require_token(input, "VOLUME_OVERLAP") &&
        read_optional_double(policy.max_volume_overlap_residual) &&
        require_token(input, "PENETRATION") &&
        read_optional_double(policy.max_penetration_depth) &&
        require_token(input, "HAUSDORFF") &&
        read_optional_double(policy.max_one_sided_hausdorff) &&
        require_token(input, "CLEARANCE") &&
        read_optional_double(policy.max_clearance_violation);
}

void write_thresholds(
    std::ostream& output, const CollisionErrorThresholdPolicy& policy) {
    const auto encode = [&output](
        const char* label, const std::optional<double>& bound) {
        output << ' ' << label << ' ';
        if (bound.has_value()) {
            output << std::setprecision(17) << bound.value();
        } else {
            output << '-';
        }
    };
    output << "THRESHOLDS";
    encode("VOLUME_OVERLAP", policy.max_volume_overlap_residual);
    encode("PENETRATION", policy.max_penetration_depth);
    encode("HAUSDORFF", policy.max_one_sided_hausdorff);
    encode("CLEARANCE", policy.max_clearance_violation);
}

} // namespace

core::Result<void> CollisionProfile::validate() const {
    if (!safe_text(game_identity, kMaxGameIdentityBytes)) {
        return core::Result<void>::failure(invalid(
            "collision profile game identity is invalid"));
    }
    if (permitted_primitive_kinds.empty() ||
        permitted_primitive_kinds.size() > kMaxPermittedPrimitiveKinds) {
        return core::Result<void>::failure(validation(
            "collision profile must permit a bounded non-empty primitive set"));
    }
    for (PrimitiveCollisionKind kind : permitted_primitive_kinds) {
        switch (kind) {
        case PrimitiveCollisionKind::sphere:
        case PrimitiveCollisionKind::box:
        case PrimitiveCollisionKind::capsule:
        case PrimitiveCollisionKind::cylinder:
            break;
        default:
            return core::Result<void>::failure(invalid(
                "collision profile permits an unknown primitive kind"));
        }
    }
    // Canonical order keeps digests byte-stable.
    if (!std::is_sorted(permitted_primitive_kinds.begin(),
            permitted_primitive_kinds.end()) ||
        std::adjacent_find(permitted_primitive_kinds.begin(),
            permitted_primitive_kinds.end()) != permitted_primitive_kinds.end()) {
        return core::Result<void>::failure(validation(
            "collision profile permitted kinds must be sorted and unique"));
    }
    if (family_preference.empty() || family_preference.size() > kMaxFamilyPreference) {
        return core::Result<void>::failure(validation(
            "collision profile must rank a bounded non-empty family preference"));
    }
    for (CollisionRepresentationFamily family : family_preference) {
        if (!valid_family(family)) {
            return core::Result<void>::failure(invalid(
                "collision profile family preference names an unknown family"));
        }
    }
    if (std::adjacent_find(family_preference.begin(), family_preference.end()) !=
        family_preference.end()) {
        return core::Result<void>::failure(validation(
            "collision profile family preference must not repeat a family"));
    }
    if (auto result = error_budget.validate(); !result) return result;
    // A kinematic default would require an authored parent per node; motion
    // defaults stay stationary or dynamic.
    if (default_motion != CollisionMotionKind::stationary &&
        default_motion != CollisionMotionKind::dynamic) {
        return core::Result<void>::failure(validation(
            "collision profile default motion must be stationary or dynamic"));
    }
    return core::Result<void>::success();
}

std::string CollisionProfile::canonical() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "GAME " << std::quoted(game_identity) << '\n';
    output << "DEFAULT_MOTION " << collision_motion_kind_name(default_motion)
           << '\n';
    output << "CAPABILITIES CONVEX " << (convex_proxy_capability ? 1 : 0)
           << " TRIANGLE " << (triangle_mesh_capability ? 1 : 0) << '\n';
    output << "PRIMITIVES " << permitted_primitive_kinds.size();
    for (PrimitiveCollisionKind kind : permitted_primitive_kinds) {
        output << ' ' << primitive_collision_kind_name(kind);
    }
    output << '\n';
    output << "FAMILIES " << family_preference.size();
    for (CollisionRepresentationFamily family : family_preference) {
        output << ' ' << collision_representation_family_name(family);
    }
    output << '\n';
    write_thresholds(output, error_budget);
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CollisionProfile::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = this->canonical();
    return core::Result<assets::Sha256Digest>::success(digest_text(canonical));
}

std::string CollisionProfile::serialize() const {
    return canonical();
}

core::Result<CollisionProfile> CollisionProfile::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CollisionProfile>::failure(parse_error(
            "collision profile text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic || !read_integer(input, version) ||
        version != kSchemaVersion) {
        return core::Result<CollisionProfile>::failure(parse_error(
            "collision profile framing or schema version is invalid"));
    }
    CollisionProfile profile;
    std::string motion;
    std::size_t primitive_count = 0U;
    std::size_t family_count = 0U;
    if (!require_token(input, "GAME") ||
        !(input >> std::quoted(profile.game_identity)) ||
        !require_token(input, "DEFAULT_MOTION") || !(input >> motion) ||
        !require_token(input, "CAPABILITIES") ||
        !require_token(input, "CONVEX") ||
        !read_bool(input, profile.convex_proxy_capability) ||
        !require_token(input, "TRIANGLE") ||
        !read_bool(input, profile.triangle_mesh_capability) ||
        !require_token(input, "PRIMITIVES") ||
        !read_count(input, kMaxPermittedPrimitiveKinds, primitive_count)) {
        return core::Result<CollisionProfile>::failure(parse_error(
            "collision profile header is malformed"));
    }
    profile.default_motion = parse_motion(motion);
    for (std::size_t index = 0U; index < primitive_count; ++index) {
        std::string kind;
        if (!(input >> kind)) {
            return core::Result<CollisionProfile>::failure(parse_error(
                "collision profile primitive kind is malformed"));
        }
        profile.permitted_primitive_kinds.push_back(parse_primitive_kind(kind));
    }
    if (!require_token(input, "FAMILIES") ||
        !read_count(input, kMaxFamilyPreference, family_count)) {
        return core::Result<CollisionProfile>::failure(parse_error(
            "collision profile family header is malformed"));
    }
    for (std::size_t index = 0U; index < family_count; ++index) {
        std::string family;
        if (!(input >> family)) {
            return core::Result<CollisionProfile>::failure(parse_error(
                "collision profile family is malformed"));
        }
        profile.family_preference.push_back(parse_family(family));
    }
    if (!read_thresholds(input, profile.error_budget) ||
        !require_token(input, "END")) {
        return core::Result<CollisionProfile>::failure(parse_error(
            "collision profile trailer is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CollisionProfile>::failure(parse_error(
            "collision profile has trailing data"));
    }
    if (auto result = profile.validate(); !result) {
        return core::Result<CollisionProfile>::failure(result.error());
    }
    return core::Result<CollisionProfile>::success(std::move(profile));
}

bool CollisionProfile::permits(PrimitiveCollisionKind kind) const noexcept {
    return std::binary_search(
        permitted_primitive_kinds.begin(), permitted_primitive_kinds.end(), kind);
}

bool CollisionProfile::prefers(
    CollisionRepresentationFamily first,
    CollisionRepresentationFamily second) const noexcept {
    const auto position = [this](CollisionRepresentationFamily family) {
        const auto found = std::find(
            family_preference.begin(), family_preference.end(), family);
        return found == family_preference.end()
            ? family_preference.size()
            : static_cast<std::size_t>(found - family_preference.begin());
    };
    return first != second && position(first) < position(second);
}

} // namespace carto::collision
