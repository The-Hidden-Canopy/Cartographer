#include <carto/collision/policy.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace carto::collision {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_COLLISION_MOTION_POLICY";

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

bool valid_motion(CollisionMotionKind kind) {
    switch (kind) {
    case CollisionMotionKind::stationary:
    case CollisionMotionKind::dynamic:
    case CollisionMotionKind::kinematic:
        return true;
    }
    return false;
}

bool is_identity_transform(const core::Transform& transform) {
    return transform.translation.x == 0.0 && transform.translation.y == 0.0 &&
        transform.translation.z == 0.0 && transform.rotation.x == 0.0 &&
        transform.rotation.y == 0.0 && transform.rotation.z == 0.0 &&
        transform.rotation.w == 1.0 && transform.scale.x == 1.0 &&
        transform.scale.y == 1.0 && transform.scale.z == 1.0;
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

bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token)) return false;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

bool read_optional_text(std::istream& input, std::optional<std::string>& value) {
    input >> std::ws;
    const int next = input.peek();
    if (next == std::char_traits<char>::eof()) return false;
    if (next == '-') {
        input.get();
        value = std::nullopt;
        return true;
    }
    std::string decoded;
    if (!(input >> std::quoted(decoded))) return false;
    value = std::move(decoded);
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

bool read_transform(std::istream& input, core::Transform& transform) {
    return read_double(input, transform.translation.x) &&
        read_double(input, transform.translation.y) &&
        read_double(input, transform.translation.z) &&
        read_double(input, transform.rotation.x) &&
        read_double(input, transform.rotation.y) &&
        read_double(input, transform.rotation.z) &&
        read_double(input, transform.rotation.w) &&
        read_double(input, transform.scale.x) &&
        read_double(input, transform.scale.y) &&
        read_double(input, transform.scale.z);
}

void write_transform(std::ostream& output, const core::Transform& transform) {
    output << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' '
           << transform.scale.y << ' ' << transform.scale.z;
}

} // namespace

std::string_view collision_motion_kind_name(CollisionMotionKind kind) noexcept {
    switch (kind) {
    case CollisionMotionKind::stationary: return "stationary";
    case CollisionMotionKind::dynamic: return "dynamic";
    case CollisionMotionKind::kinematic: return "kinematic";
    }
    return "unknown";
}

core::Result<void> CollisionMotionBinding::validate() const {
    if (!safe_text(node_identity, kMaxIdentityBytes)) {
        return core::Result<void>::failure(invalid(
            "collision motion node identity is invalid"));
    }
    if (!valid_motion(motion)) {
        return core::Result<void>::failure(invalid(
            "collision motion kind is invalid"));
    }
    if (motion == CollisionMotionKind::kinematic) {
        if (!parent_identity.has_value() ||
            !safe_text(parent_identity.value(), kMaxIdentityBytes)) {
            return core::Result<void>::failure(validation(
                "kinematic collision node must name a valid parent identity"));
        }
        if (!rest_transform.finite()) {
            return core::Result<void>::failure(validation(
                "kinematic collision node must carry a finite rest transform"));
        }
        if (!runtime_mapping.has_value() ||
            !safe_text(runtime_mapping.value(), kMaxMappingBytes)) {
            return core::Result<void>::failure(validation(
                "kinematic collision node must declare a valid runtime mapping"));
        }
        return core::Result<void>::success();
    }
    if (parent_identity.has_value() || runtime_mapping.has_value() ||
        !is_identity_transform(rest_transform)) {
        return core::Result<void>::failure(validation(
            "stationary and dynamic collision nodes must leave the kinematic "
            "parent, rest transform, and runtime mapping unset"));
    }
    return core::Result<void>::success();
}

std::string CollisionMotionBinding::canonical() const {
    std::ostringstream output;
    output << "NODE " << std::quoted(node_identity) << ' '
           << collision_motion_kind_name(motion) << " PARENT ";
    if (parent_identity.has_value()) {
        output << std::quoted(parent_identity.value());
    } else {
        output << '-';
    }
    output << " REST ";
    write_transform(output, rest_transform);
    output << " MAPPING ";
    if (runtime_mapping.has_value()) {
        output << std::quoted(runtime_mapping.value());
    } else {
        output << '-';
    }
    return output.str();
}

core::Result<void> CollisionMotionPolicy::insert(CollisionMotionBinding binding) {
    if (nodes_.size() >= kMaxNodes) {
        return core::Result<void>::failure(validation(
            "collision motion policy has reached its bounded node count"));
    }
    if (auto result = binding.validate(); !result) return result;
    const auto [iterator, inserted] =
        nodes_.emplace(binding.node_identity, std::move(binding));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(
            "collision motion node identity is duplicated"));
    }
    return core::Result<void>::success();
}

const CollisionMotionBinding* CollisionMotionPolicy::find(
    std::string_view node_identity) const noexcept {
    const auto found = nodes_.find(std::string(node_identity));
    return found == nodes_.end() ? nullptr : &found->second;
}

core::Result<void> CollisionMotionPolicy::validate() const {
    if (nodes_.size() > kMaxNodes) {
        return core::Result<void>::failure(validation(
            "collision motion policy exceeds its bounded node count"));
    }
    for (const auto& [identity, binding] : nodes_) {
        if (identity != binding.node_identity) {
            return core::Result<void>::failure(validation(
                "collision motion map identity does not match its binding"));
        }
        if (auto result = binding.validate(); !result) return result;
    }
    for (const auto& [identity, binding] : nodes_) {
        static_cast<void>(identity);
        if (binding.motion != CollisionMotionKind::kinematic) continue;
        const CollisionMotionBinding* parent = find(binding.parent_identity.value());
        if (parent == nullptr) {
            return core::Result<void>::failure(validation(
                "kinematic collision node references a missing parent identity"));
        }
        // Walk the parent chain; a revisit means the policy encodes a cycle,
        // which would make rest-transform composition ill-defined.
        std::set<std::string> visited;
        visited.insert(binding.node_identity);
        const CollisionMotionBinding* current = parent;
        while (current->motion == CollisionMotionKind::kinematic) {
            if (!visited.insert(current->node_identity).second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::cycle_detected,
                    "collision motion policy contains a kinematic parent cycle"));
            }
            current = find(current->parent_identity.value());
            if (current == nullptr) {
                return core::Result<void>::failure(validation(
                    "kinematic collision node references a missing parent identity"));
            }
        }
    }
    return core::Result<void>::success();
}

std::string CollisionMotionPolicy::canonical() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "NODES " << nodes_.size() << '\n';
    for (const auto& [identity, binding] : nodes_) {
        static_cast<void>(identity);
        output << binding.canonical() << '\n';
    }
    output << "END\n";
    return output.str();
}

std::string CollisionMotionPolicy::serialize() const {
    return canonical();
}

core::Result<CollisionMotionPolicy> CollisionMotionPolicy::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<CollisionMotionPolicy>::failure(parse_error(
            "collision motion policy text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic || !read_integer(input, version) ||
        version != kSchemaVersion) {
        return core::Result<CollisionMotionPolicy>::failure(parse_error(
            "collision motion policy framing or schema version is invalid"));
    }
    std::size_t node_count = 0U;
    if (!require_token(input, "NODES") ||
        !read_count(input, kMaxNodes, node_count)) {
        return core::Result<CollisionMotionPolicy>::failure(parse_error(
            "collision motion policy node header is invalid"));
    }
    CollisionMotionPolicy policy;
    for (std::size_t index = 0U; index < node_count; ++index) {
        CollisionMotionBinding binding;
        std::string motion;
        if (!require_token(input, "NODE") ||
            !(input >> std::quoted(binding.node_identity)) ||
            !(input >> motion) ||
            !require_token(input, "PARENT") ||
            !read_optional_text(input, binding.parent_identity) ||
            !require_token(input, "REST") ||
            !read_transform(input, binding.rest_transform) ||
            !require_token(input, "MAPPING") ||
            !read_optional_text(input, binding.runtime_mapping)) {
            return core::Result<CollisionMotionPolicy>::failure(parse_error(
                "collision motion policy node is malformed"));
        }
        binding.motion = parse_motion(motion);
        if (auto result = policy.insert(std::move(binding)); !result) {
            return core::Result<CollisionMotionPolicy>::failure(
                result.error().with_context(
                    "collision motion policy node does not survive admission"));
        }
    }
    if (!require_token(input, "END")) {
        return core::Result<CollisionMotionPolicy>::failure(parse_error(
            "collision motion policy trailer is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<CollisionMotionPolicy>::failure(parse_error(
            "collision motion policy has trailing data"));
    }
    if (auto result = policy.validate(); !result) {
        return core::Result<CollisionMotionPolicy>::failure(result.error());
    }
    return core::Result<CollisionMotionPolicy>::success(std::move(policy));
}

} // namespace carto::collision
