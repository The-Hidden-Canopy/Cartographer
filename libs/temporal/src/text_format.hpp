#pragma once

#include <carto/core/diagnostic.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::temporal::detail {

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

template <typename T>
inline bool read_integer(std::istream& input, T& value) {
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

inline bool read_double(std::istream& input, double& value) {
    return static_cast<bool>(input >> value) && std::isfinite(value);
}

inline void write_vec3(std::ostream& output, const core::Vec3d& value) {
    output << value.x << ' ' << value.y << ' ' << value.z;
}

inline bool read_vec3(std::istream& input, core::Vec3d& value) {
    return read_double(input, value.x) && read_double(input, value.y) &&
        read_double(input, value.z);
}

inline void write_quaternion(std::ostream& output, const core::Quaternion& value) {
    output << value.x << ' ' << value.y << ' ' << value.z << ' ' << value.w;
}

inline bool read_quaternion(std::istream& input, core::Quaternion& value) {
    return read_double(input, value.x) && read_double(input, value.y) &&
        read_double(input, value.z) && read_double(input, value.w);
}

inline void write_transform(std::ostream& output, const core::Transform& value) {
    write_vec3(output, value.translation);
    output << ' ';
    write_quaternion(output, value.rotation);
    output << ' ';
    write_vec3(output, value.scale);
}

inline bool read_transform(std::istream& input, core::Transform& value) {
    return read_vec3(input, value.translation) &&
        read_quaternion(input, value.rotation) && read_vec3(input, value.scale);
}

} // namespace carto::temporal::detail
