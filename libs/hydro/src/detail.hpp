#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <istream>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace carto::hydro::detail {

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

inline bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

inline bool finite_bounded(double value, double maximum) {
    return std::isfinite(value) && value >= 0.0 && value <= maximum;
}

inline std::string number(double value) {
    std::ostringstream output;
    output << std::setprecision(17) << value;
    return output.str();
}

inline assets::Sha256Digest digest_of(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

inline bool read_double(std::istream& input, double& value) {
    std::string token;
    if (!(input >> token) || token.empty()) return false;
    char* end = nullptr;
    value = std::strtod(token.c_str(), &end);
    return end == token.c_str() + token.size();
}

inline bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

inline bool read_revision(std::istream& input, core::Revision& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded)) return false;
    value = core::Revision{encoded};
    return true;
}

inline bool read_digest(std::istream& input, assets::Sha256Digest& value) {
    std::string encoded;
    if (!(input >> encoded)) return false;
    const auto parsed = assets::Sha256Digest::from_hex(encoded);
    if (!parsed) return false;
    value = parsed.value();
    return true;
}

inline bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

inline bool at_end(std::istream& input) {
    std::string trailing;
    return !(input >> trailing);
}

inline bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

} // namespace carto::hydro::detail
