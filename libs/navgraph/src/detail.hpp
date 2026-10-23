#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/diagnostic.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/agent_profile.hpp>
#include <carto/navgraph/links.hpp>
#include <carto/navgraph/region.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace carto::navgraph::detail {

inline constexpr std::size_t kMaxSemanticTypeBytes = 128U;
inline constexpr std::size_t kMaxIdentityStringBytes = 256U;
inline constexpr std::size_t kMaxLabelBytes = 128U;

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    for (const char element : value) {
        const unsigned char byte = static_cast<unsigned char>(element);
        if (byte < 0x20U || byte == 0x7fU || byte == '\t' || byte == '\r' || byte == '\n') {
            return false;
        }
    }
    return true;
}

inline bool valid_plane(NavigationPlane plane) {
    switch (plane) {
    case NavigationPlane::xy:
    case NavigationPlane::xz:
    case NavigationPlane::yz:
        return true;
    }
    return false;
}

inline bool valid_medium(TraversalMedium medium) {
    switch (medium) {
    case TraversalMedium::land:
    case TraversalMedium::water:
    case TraversalMedium::vessel:
    case TraversalMedium::building:
        return true;
    }
    return false;
}

inline bool valid_access_primitive(AccessPrimitive primitive) {
    switch (primitive) {
    case AccessPrimitive::door:
    case AccessPrimitive::portal:
    case AccessPrimitive::ladder:
    case AccessPrimitive::hatch:
        return true;
    }
    return false;
}

inline bool valid_traversal_primitive(TraversalPrimitive primitive) {
    switch (primitive) {
    case TraversalPrimitive::ground:
    case TraversalPrimitive::water:
    case TraversalPrimitive::vessel_deck:
    case TraversalPrimitive::ladder:
    case TraversalPrimitive::hatch:
    case TraversalPrimitive::portal:
        return true;
    }
    return false;
}

inline bool valid_locomotion(LocomotionClass kind) {
    switch (kind) {
    case LocomotionClass::pedestrian:
    case LocomotionClass::animal:
    case LocomotionClass::vehicle:
    case LocomotionClass::vessel:
        return true;
    }
    return false;
}

inline bool valid_traversal_primitive_set(
    const std::vector<TraversalPrimitive>& primitives) {
    return std::all_of(primitives.begin(), primitives.end(), valid_traversal_primitive);
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

inline bool read_uint64(std::istream& input, std::uint64_t& value) {
    return read_integer(input, value);
}

inline bool read_int64(std::istream& input, std::int64_t& value) {
    std::string token;
    if (!(input >> token)) return false;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

inline bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_integer(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

inline bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_integer(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

inline bool read_double(std::istream& input, double& value) {
    return static_cast<bool>(input >> value) && std::isfinite(value);
}

inline void write_vec3(std::ostream& output, core::Vec3d value) {
    output << value.x << ' ' << value.y << ' ' << value.z;
}

inline bool read_vec3(std::istream& input, core::Vec3d& value) {
    return static_cast<bool>(input >> value.x >> value.y >> value.z) && value.finite();
}

inline void write_bounds(std::ostream& output, const core::Bounds3d& bounds) {
    write_vec3(output, bounds.minimum);
    output << ' ';
    write_vec3(output, bounds.maximum);
}

inline bool read_bounds(std::istream& input, core::Bounds3d& bounds) {
    return read_vec3(input, bounds.minimum) && read_vec3(input, bounds.maximum);
}

template <typename Id>
void write_optional_id(std::ostream& output, const std::optional<Id>& value) {
    output << (value.has_value() ? value->value : 0U);
}

template <typename Id>
bool read_optional_id(std::istream& input, std::optional<Id>& value) {
    std::uint64_t encoded = 0U;
    if (!read_uint64(input, encoded)) return false;
    value = encoded == 0U ? std::nullopt : std::optional<Id>{Id{encoded}};
    return true;
}

inline void write_digest(std::ostream& output, const assets::Sha256Digest& digest) {
    output << digest.hex();
}

inline bool read_digest(std::istream& input, assets::Sha256Digest& digest) {
    std::string encoded;
    if (!(input >> encoded)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(encoded);
    if (!parsed) return false;
    digest = parsed.value();
    return true;
}

} // namespace carto::navgraph::detail
