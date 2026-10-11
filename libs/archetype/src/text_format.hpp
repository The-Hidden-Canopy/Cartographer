#pragma once

// Private serialization/text helpers shared by the archetype record sources.
// They mirror the bounded, deterministic, fail-closed text idiom of
// libs/world/src/model.cpp without depending on that library's internals.

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace carto::archetype::detail {

inline core::Diagnostic invalid_text(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic malformed_text(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

inline assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

inline assets::Sha256Digest digest_bytes(const std::vector<std::uint8_t>& bytes) {
    return assets::sha256(std::span<const std::uint8_t>{bytes.data(), bytes.size()});
}

inline void append_text(std::vector<std::uint8_t>& output, std::string_view value) {
    output.insert(output.end(), value.begin(), value.end());
}

inline void append_u32be(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 24U; shift != 0U; shift -= 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

inline void append_u64be(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 56U; shift != 0U; shift -= 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

inline void append_digest(std::vector<std::uint8_t>& output, const assets::Sha256Digest& value) {
    output.insert(output.end(), value.bytes.begin(), value.bytes.end());
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

inline core::Result<assets::Sha256Digest> read_digest(
    std::istream& input,
    std::string_view record,
    std::string_view field) {
    std::string encoded;
    if (!(input >> encoded)) {
        return core::Result<assets::Sha256Digest>::failure(malformed_text(
            std::string(record) + " is missing its " + std::string(field)));
    }
    const auto parsed = assets::Sha256Digest::from_hex(encoded);
    if (!parsed) {
        return core::Result<assets::Sha256Digest>::failure(malformed_text(
            std::string(record) + " contains an invalid " + std::string(field)));
    }
    return core::Result<assets::Sha256Digest>::success(parsed.value());
}

} // namespace carto::archetype::detail
