#include <carto/kinematic/limits.hpp>

#include <carto/kinematic/graph.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace carto::kinematic {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_JOINT_LIMITS";

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

template <typename T>
bool read_integer(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
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

void write_vec3(std::ostream& output, const core::Vec3d& value) {
    output << value.x << ' ' << value.y << ' ' << value.z;
}

bool read_vec3(std::istream& input, core::Vec3d& value) {
    return static_cast<bool>(
        input >> value.x >> value.y >> value.z);
}

bool zero_vec3(const core::Vec3d& value) noexcept {
    return value.x == 0.0 && value.y == 0.0 && value.z == 0.0;
}

bool bounded_axis_range(
    const core::Vec3d& minimum,
    const core::Vec3d& maximum,
    double magnitude) noexcept {
    return minimum.finite() && maximum.finite() &&
        minimum.x <= maximum.x && minimum.y <= maximum.y &&
        minimum.z <= maximum.z &&
        std::abs(minimum.x) <= magnitude && std::abs(minimum.y) <= magnitude &&
        std::abs(minimum.z) <= magnitude &&
        std::abs(maximum.x) <= magnitude && std::abs(maximum.y) <= magnitude &&
        std::abs(maximum.z) <= magnitude;
}

bool contains_vec3(
    const core::Vec3d& value,
    const core::Vec3d& minimum,
    const core::Vec3d& maximum) noexcept {
    return value.x >= minimum.x && value.x <= maximum.x &&
        value.y >= minimum.y && value.y <= maximum.y &&
        value.z >= minimum.z && value.z <= maximum.z;
}

bool is_motion_tag(std::string_view token) noexcept {
    static constexpr std::array<std::string_view, 6> kForbidden{
        "POSE", "MOTION", "MOTION_STATE", "VELOCITY", "ACCELERATION",
        "CURRENT_ANGLE"};
    return std::find(kForbidden.begin(), kForbidden.end(), token) !=
        kForbidden.end();
}

} // namespace

bool JointLimit::contains_angular(core::Vec3d angle) const noexcept {
    return angular_enabled && angle.finite() &&
        contains_vec3(angle, angular_min, angular_max);
}

bool JointLimit::contains_linear(core::Vec3d offset) const noexcept {
    return linear_enabled && offset.finite() &&
        contains_vec3(offset, linear_min, linear_max);
}

core::Result<void> JointLimit::validate() const {
    if (!joint) {
        return core::Result<void>::failure(invalid(
            "joint limit identity is invalid"));
    }
    if (angular_enabled) {
        if (!bounded_axis_range(
                angular_min, angular_max, JointLimitSet::kMaxAngularMagnitude)) {
            return core::Result<void>::failure(invalid(
                "joint angular limit is not finite, ordered, or bounded"));
        }
    } else if (!zero_vec3(angular_min) || !zero_vec3(angular_max)) {
        return core::Result<void>::failure(validation(
            "disabled angular limits must keep zero bounds"));
    }
    if (linear_enabled) {
        if (!bounded_axis_range(
                linear_min, linear_max, JointLimitSet::kMaxLinearMagnitude)) {
            return core::Result<void>::failure(invalid(
                "joint linear limit is not finite, ordered, or bounded"));
        }
    } else if (!zero_vec3(linear_min) || !zero_vec3(linear_max)) {
        return core::Result<void>::failure(validation(
            "disabled linear limits must keep zero bounds"));
    }
    if (!angular_enabled && !linear_enabled) {
        return core::Result<void>::failure(validation(
            "joint limit must enable at least one axis family"));
    }
    return core::Result<void>::success();
}

core::Result<void> JointLimitSet::insert(JointLimit limit) {
    if (limits_.size() >= kMaxLimits) {
        return core::Result<void>::failure(validation(
            "joint limit set has reached its bounded capacity"));
    }
    if (auto result = limit.validate(); !result) return result;
    const auto [iterator, inserted] =
        limits_.emplace(limit.joint, std::move(limit));
    static_cast<void>(iterator);
    if (!inserted) {
        return core::Result<void>::failure(invalid(
            "joint limit identity is duplicated"));
    }
    return core::Result<void>::success();
}

const JointLimit* JointLimitSet::find(JointId joint) const noexcept {
    const auto found = limits_.find(joint);
    return found == limits_.end() ? nullptr : &found->second;
}

core::Result<void> JointLimitSet::validate() const {
    if (limits_.size() > kMaxLimits) {
        return core::Result<void>::failure(validation(
            "joint limit set exceeds its bounded capacity"));
    }
    for (const auto& [joint, limit] : limits_) {
        if (joint != limit.joint) {
            return core::Result<void>::failure(validation(
                "joint limit map identity does not match its record"));
        }
        if (auto result = limit.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> JointLimitSet::validate_for_graph(
    const KinematicGraph& graph) const {
    for (const auto& [joint, limit] : limits_) {
        static_cast<void>(limit);
        if (!graph.find(joint)) {
            return core::Result<void>::failure(validation(
                "joint limit references a joint missing from the graph"));
        }
    }
    return core::Result<void>::success();
}

std::string JointLimitSet::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "LIMITS " << limits_.size() << '\n';
    for (const auto& [joint, limit] : limits_) {
        output << "LIMIT " << joint.value << ' '
               << (limit.angular_enabled ? 1 : 0) << ' ';
        write_vec3(output, limit.angular_min);
        output << ' ';
        write_vec3(output, limit.angular_max);
        output << ' ' << (limit.linear_enabled ? 1 : 0) << ' ';
        write_vec3(output, limit.linear_min);
        output << ' ';
        write_vec3(output, limit.linear_max);
        output << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> JointLimitSet::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<JointLimitSet> JointLimitSet::deserialize(std::string_view text) {
    if (text.empty() || text.size() > kMaxSerializedBytes) {
        return core::Result<JointLimitSet>::failure(parse_error(
            "joint limit set text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || magic != kMagic) {
        return core::Result<JointLimitSet>::failure(parse_error(
            "joint limit set framing is invalid"));
    }
    if (!read_integer(input, version)) {
        return core::Result<JointLimitSet>::failure(parse_error(
            "joint limit set schema version is missing"));
    }
    if (version != kSchemaVersion) {
        return core::Result<JointLimitSet>::failure(version_error(
            "joint limit set schema version is not supported"));
    }

    JointLimitSet set;
    std::size_t limit_count = 0U;
    if (!require_token(input, "LIMITS") ||
        !read_count(input, kMaxLimits, limit_count)) {
        return core::Result<JointLimitSet>::failure(parse_error(
            "joint limit set header is invalid"));
    }
    for (std::size_t index = 0U; index < limit_count; ++index) {
        JointLimit limit;
        if (!require_token(input, "LIMIT") ||
            !read_integer(input, limit.joint.value) ||
            !read_bool(input, limit.angular_enabled) ||
            !read_vec3(input, limit.angular_min) ||
            !read_vec3(input, limit.angular_max) ||
            !read_bool(input, limit.linear_enabled) ||
            !read_vec3(input, limit.linear_min) ||
            !read_vec3(input, limit.linear_max)) {
            return core::Result<JointLimitSet>::failure(parse_error(
                "joint limit record is malformed"));
        }
        if (auto result = set.insert(std::move(limit)); !result) {
            return core::Result<JointLimitSet>::failure(result.error());
        }
    }

    if (!require_token(input, "END")) {
        return core::Result<JointLimitSet>::failure(parse_error(
            "joint limit set is missing its END record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<JointLimitSet>::failure(parse_error(
            "joint limit set contains trailing unbound data"));
    }
    if (auto result = set.validate(); !result) {
        return core::Result<JointLimitSet>::failure(result.error());
    }
    return core::Result<JointLimitSet>::success(std::move(set));
}

core::Result<void> require_no_motion_state(std::string_view serialized_record) {
    std::istringstream input{std::string(serialized_record)};
    std::string token;
    while (input >> token) {
        if (!token.empty() && token.front() == '"') continue;
        if (is_motion_tag(token)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "serialized kinematic record carries runtime-owned motion state"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::kinematic
