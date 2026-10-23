#pragma once

#include <carto/core/result.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace carto::face::detail {

inline core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic version_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::version_mismatch, std::move(message));
}

// Bounded safe token: nonempty (unless allowed), byte-bounded, no control
// bytes and no DEL. Quoted payload fields use this admission everywhere.
inline bool safe_token(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU;
    });
}

inline void write_json_string(std::ostringstream& output, std::string_view value) {
    output << '"';
    for (const char raw : value) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(byte);
            }
        }
    }
    output << '"';
}

inline std::string json_string(std::string_view value) {
    std::ostringstream output;
    write_json_string(output, value);
    return output.str();
}

// Deterministic finite-double rendering: printf-style rounding at 17
// significant digits round-trips exactly through IEEE-754 parsing.
inline void write_double(std::ostringstream& output, double value) {
    output << std::setprecision(17) << value;
}

inline core::Result<double> parse_double_token(std::string_view token, const char* label) {
    if (token.empty()) {
        return core::Result<double>::failure(parse_error(
            std::string(label) + " number is empty"));
    }
    double parsed = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
        !std::isfinite(parsed)) {
        return core::Result<double>::failure(parse_error(
            std::string(label) + " number is invalid"));
    }
    return core::Result<double>::success(parsed);
}

// Framed-codec admission: the first line must be the exact magic followed by
// the exact schema version this codec writes.
inline core::Result<void> expect_magic(
    std::istringstream& input,
    std::string_view magic,
    std::uint32_t supported_version) {
    std::string token;
    std::uint32_t version = 0U;
    if (!(input >> token) || !(input >> version) ||
        token != magic || version != supported_version) {
        return core::Result<void>::failure(version_error(
            std::string(magic) + " framing or schema version is invalid"));
    }
    return core::Result<void>::success();
}

inline core::Result<std::string> read_token(std::istringstream& input, const char* label) {
    std::string token;
    if (!(input >> token)) {
        return core::Result<std::string>::failure(parse_error(
            std::string(label) + " token is missing"));
    }
    return core::Result<std::string>::success(std::move(token));
}

// Reads one quoted JSON-style string token (escapes resolved). Unterminated or
// malformed escapes fail closed; decoded content must be a bounded safe token.
inline core::Result<std::string> read_quoted(
    std::istringstream& input,
    const char* label,
    std::size_t maximum) {
    input >> std::ws;
    if (input.peek() != '"') {
        return core::Result<std::string>::failure(parse_error(
            std::string(label) + " string is missing"));
    }
    input.get();
    std::string decoded;
    for (;;) {
        const int next = input.get();
        if (next == std::char_traits<char>::eof()) {
            return core::Result<std::string>::failure(parse_error(
                std::string(label) + " string is unterminated"));
        }
        const char raw = static_cast<char>(next);
        if (raw == '"') break;
        if (raw != '\\') {
            decoded.push_back(raw);
            continue;
        }
        const int escaped_raw = input.get();
        if (escaped_raw == std::char_traits<char>::eof()) {
            return core::Result<std::string>::failure(parse_error(
                std::string(label) + " string escape is incomplete"));
        }
        switch (static_cast<char>(escaped_raw)) {
        case '"': decoded.push_back('"'); break;
        case '\\': decoded.push_back('\\'); break;
        case '/': decoded.push_back('/'); break;
        case 'b': decoded.push_back('\b'); break;
        case 'f': decoded.push_back('\f'); break;
        case 'n': decoded.push_back('\n'); break;
        case 'r': decoded.push_back('\r'); break;
        case 't': decoded.push_back('\t'); break;
        default:
            return core::Result<std::string>::failure(parse_error(
                std::string(label) + " string escape is invalid"));
        }
    }
    if (!safe_token(decoded, maximum, true)) {
        return core::Result<std::string>::failure(parse_error(
            std::string(label) + " string exceeds its byte bound"));
    }
    return core::Result<std::string>::success(std::move(decoded));
}

// The closing frame: exactly the END token and nothing after it.
inline core::Result<void> expect_end(std::istringstream& input) {
    std::string trailing;
    if (!(input >> trailing) || trailing != "END") {
        return core::Result<void>::failure(parse_error(
            "frame is missing its END token"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<void>::failure(parse_error(
            "frame contains trailing unbound data"));
    }
    return core::Result<void>::success();
}

} // namespace carto::face::detail
