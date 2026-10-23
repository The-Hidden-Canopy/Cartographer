// Private serialization helpers for the workspace layer. Emission is a fixed
// member order, deterministic JSON surface; parsing reuses the bounded
// carto::core::json grammar boundary and fails closed on any deviation.
// This header is internal to carto_workspace and is never installed.

#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/json.hpp>
#include <carto/core/result.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace carto::workspace::codec {

inline core::Diagnostic malformed(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic missing(std::string_view key) {
    return malformed("workspace record is missing member '" + std::string(key) + "'");
}

inline std::string quote(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2U);
    out.push_back('"');
    for (const char raw : value) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[byte >> 4U]);
                out.push_back(kHex[byte & 0x0fU]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
        }
    }
    out.push_back('"');
    return out;
}

inline std::string number(double value) {
    // Deterministic double emission: shortest round-trip is not required, but
    // the emitted text must parse back to the identical double and must never
    // be locale-sensitive or non-finite.
    if (!std::isfinite(value)) return "null";
    std::string out(32U, '\0');
    const auto converted = std::to_chars(
        out.data(), out.data() + out.size(), value,
        std::chars_format::general, std::numeric_limits<double>::max_digits10);
    out.resize(static_cast<std::size_t>(converted.ptr - out.data()));
    return out;
}

inline std::string u64(std::uint64_t value) {
    std::string out(20U, '\0');
    const auto converted = std::to_chars(out.data(), out.data() + out.size(), value);
    out.resize(static_cast<std::size_t>(converted.ptr - out.data()));
    return out;
}

inline std::string i64(std::int64_t value) {
    std::string out(20U, '\0');
    const auto converted = std::to_chars(out.data(), out.data() + out.size(), value);
    out.resize(static_cast<std::size_t>(converted.ptr - out.data()));
    return out;
}

inline std::string boolean(bool value) { return value ? "true" : "false"; }

inline std::string digest_hex(const assets::Sha256Digest& digest) {
    return digest.hex();
}

inline core::Result<double> parse_double(std::string_view raw) {
    if (raw.empty() || raw == "null") {
        return core::Result<double>::failure(malformed(
            "workspace value is not a finite number"));
    }
    double value = 0.0;
    const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (converted.ec != std::errc() || converted.ptr != raw.data() + raw.size() ||
        !std::isfinite(value)) {
        return core::Result<double>::failure(malformed(
            "workspace value is not a finite number"));
    }
    return core::Result<double>::success(value);
}

inline core::Result<std::uint64_t> parse_uint(std::string_view raw) {
    return core::json::parse_uint(raw);
}

// Accumulates one JSON object with fixed member order.
class ObjectWriter final {
public:
    ObjectWriter() { out_.push_back('{'); }
    void add_raw(std::string_view key, std::string_view raw_value) {
        if (!first_) out_.push_back(',');
        first_ = false;
        out_ += quote(key);
        out_.push_back(':');
        out_.append(raw_value);
    }
    void add(std::string_view key, std::string_view value) { add_raw(key, quote(value)); }
    void add(std::string_view key, const std::string& value) { add(key, std::string_view(value)); }
    void add(std::string_view key, std::uint64_t value) { add_raw(key, u64(value)); }
    void add(std::string_view key, std::int64_t value) { add_raw(key, i64(value)); }
    void add(std::string_view key, double value) { add_raw(key, number(value)); }
    void add(std::string_view key, bool value) { add_raw(key, boolean(value)); }
    void add(std::string_view key, const assets::Sha256Digest& value) {
        add(key, digest_hex(value));
    }
    [[nodiscard]] std::string finish() && {
        out_.push_back('}');
        return std::move(out_);
    }

private:
    std::string out_;
    bool first_ = true;
};

inline const core::json::Member* find(
    const std::vector<core::json::Member>& members, std::string_view key) noexcept {
    return core::json::find_member(members, key);
}

inline core::Result<std::string> string_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::string>::failure(missing(key));
    }
    return core::json::decode_string(member->raw_value);
}

inline core::Result<std::string> optional_string_member(
    const std::vector<core::json::Member>& members,
    std::string_view key,
    std::string fallback = {}) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) return core::Result<std::string>::success(std::move(fallback));
    return core::json::decode_string(member->raw_value);
}

inline core::Result<std::uint64_t> uint_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::uint64_t>::failure(missing(key));
    }
    return core::json::parse_uint(member->raw_value);
}

inline core::Result<std::uint32_t> u32_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const auto value = uint_member(members, key);
    if (!value) return core::Result<std::uint32_t>::failure(value.error());
    if (value.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(malformed(
            "workspace record member exceeds the 32-bit range"));
    }
    return core::Result<std::uint32_t>::success(
        static_cast<std::uint32_t>(value.value()));
}

inline core::Result<std::int64_t> int_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::int64_t>::failure(missing(key));
    }
    const std::string_view raw = member->raw_value;
    if (raw.empty()) {
        return core::Result<std::int64_t>::failure(malformed("workspace integer is empty"));
    }
    std::int64_t value = 0;
    const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (converted.ec != std::errc() || converted.ptr != raw.data() + raw.size()) {
        return core::Result<std::int64_t>::failure(malformed(
            "workspace record member is not an exact integer"));
    }
    return core::Result<std::int64_t>::success(value);
}

inline core::Result<double> double_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<double>::failure(missing(key));
    }
    const std::string_view raw = member->raw_value;
    if (raw.empty() || raw == "null") {
        return core::Result<double>::failure(malformed(
            "workspace record member is not a finite number"));
    }
    double value = 0.0;
    const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), value);
    if (converted.ec != std::errc() || converted.ptr != raw.data() + raw.size() ||
        !std::isfinite(value)) {
        return core::Result<double>::failure(malformed(
            "workspace record member is not a finite number"));
    }
    return core::Result<double>::success(value);
}

inline core::Result<bool> bool_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<bool>::failure(missing(key));
    }
    const std::string_view raw = member->raw_value;
    if (raw == "true") return core::Result<bool>::success(true);
    if (raw == "false") return core::Result<bool>::success(false);
    return core::Result<bool>::failure(malformed("workspace record member is not a boolean"));
}

inline core::Result<assets::Sha256Digest> digest_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<assets::Sha256Digest>::failure(missing(key));
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) {
        return core::Result<assets::Sha256Digest>::failure(decoded.error());
    }
    return assets::Sha256Digest::from_hex(decoded.value());
}

// Splits an already grammar-validated JSON array body into raw element views.
// The input must be the exact array text including brackets.
inline core::Result<std::vector<std::string_view>> split_array(std::string_view raw) {
    std::vector<std::string_view> elements;
    if (raw.size() < 2U || raw.front() != '[' || raw.back() != ']') {
        return core::Result<std::vector<std::string_view>>::failure(malformed(
            "workspace record member is not an array"));
    }
    std::size_t cursor = 1U;
    const std::size_t end = raw.size() - 1U;
    auto skip_space = [raw, end](std::size_t& at) {
        while (at < end && (raw[at] == ' ' || raw[at] == '\n' ||
                            raw[at] == '\r' || raw[at] == '\t')) ++at;
    };
    skip_space(cursor);
    if (cursor < end && raw[cursor] == ']') {
        return core::Result<std::vector<std::string_view>>::success(std::move(elements));
    }
    while (cursor < end) {
        const std::size_t start = cursor;
        std::size_t depth = 0U;
        bool in_string = false;
        bool escaped = false;
        while (cursor < end) {
            const char byte = raw[cursor];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (byte == '\\') {
                    escaped = true;
                } else if (byte == '"') {
                    in_string = false;
                }
            } else if (byte == '"') {
                in_string = true;
            } else if (byte == '{' || byte == '[') {
                ++depth;
            } else if (byte == '}' || byte == ']') {
                if (depth == 0U) break;
                --depth;
            } else if (byte == ',' && depth == 0U) {
                break;
            }
            ++cursor;
        }
        elements.push_back(raw.substr(start, cursor - start));
        skip_space(cursor);
        if (cursor < end && raw[cursor] == ',') {
            ++cursor;
            skip_space(cursor);
            continue;
        }
        break;
    }
    return core::Result<std::vector<std::string_view>>::success(std::move(elements));
}

inline core::Result<std::vector<std::string>> string_array_member(
    const std::vector<core::json::Member>& members,
    std::string_view key,
    std::size_t max_entries) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::vector<std::string>>::failure(missing(key));
    }
    const auto elements = split_array(member->raw_value);
    if (!elements) return core::Result<std::vector<std::string>>::failure(elements.error());
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<std::string>>::failure(malformed(
            "workspace string array exceeds its bounded capacity"));
    }
    std::vector<std::string> decoded;
    decoded.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto value = core::json::decode_string(element);
        if (!value) return core::Result<std::vector<std::string>>::failure(value.error());
        decoded.push_back(value.value());
    }
    return core::Result<std::vector<std::string>>::success(std::move(decoded));
}

inline core::Result<std::vector<std::uint64_t>> uint_array_member(
    const std::vector<core::json::Member>& members,
    std::string_view key,
    std::size_t max_entries) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::vector<std::uint64_t>>::failure(missing(key));
    }
    const auto elements = split_array(member->raw_value);
    if (!elements) return core::Result<std::vector<std::uint64_t>>::failure(elements.error());
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<std::uint64_t>>::failure(malformed(
            "workspace integer array exceeds its bounded capacity"));
    }
    std::vector<std::uint64_t> decoded;
    decoded.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto value = core::json::parse_uint(element);
        if (!value) return core::Result<std::vector<std::uint64_t>>::failure(value.error());
        decoded.push_back(value.value());
    }
    return core::Result<std::vector<std::uint64_t>>::success(std::move(decoded));
}

inline core::Result<std::vector<std::string_view>> array_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::vector<std::string_view>>::failure(missing(key));
    }
    return split_array(member->raw_value);
}

inline core::Result<std::vector<core::json::Member>> object_member(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = find(members, key);
    if (member == nullptr) {
        return core::Result<std::vector<core::json::Member>>::failure(missing(key));
    }
    return core::json::parse_object(member->raw_value);
}


inline std::string digest_array_json(const std::vector<assets::Sha256Digest>& digests) {
    std::string out = "[";
    bool first = true;
    for (const assets::Sha256Digest& digest : digests) {
        if (!first) out.push_back(',');
        first = false;
        out += codec::quote(digest.hex());
    }
    out.push_back(']');
    return out;
}

inline core::Result<std::vector<assets::Sha256Digest>> digest_array_from(
    std::string_view raw, std::size_t max_entries) {
    const auto elements = codec::split_array(raw);
    if (!elements) {
        return core::Result<std::vector<assets::Sha256Digest>>::failure(elements.error());
    }
    if (elements.value().size() > max_entries) {
        return core::Result<std::vector<assets::Sha256Digest>>::failure(codec::malformed(
            "workspace digest array exceeds its bounded capacity"));
    }
    std::vector<assets::Sha256Digest> digests;
    digests.reserve(elements.value().size());
    for (const std::string_view element : elements.value()) {
        const auto decoded = core::json::decode_string(element);
        if (!decoded) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(decoded.error());
        }
        const auto digest = assets::Sha256Digest::from_hex(decoded.value());
        if (!digest) {
            return core::Result<std::vector<assets::Sha256Digest>>::failure(digest.error());
        }
        digests.push_back(digest.value());
    }
    return core::Result<std::vector<assets::Sha256Digest>>::success(std::move(digests));
}

inline std::string nullable_digest_json(const std::optional<assets::Sha256Digest>& digest) {
    if (!digest.has_value()) return "null";
    return codec::quote(digest->hex());
}

inline core::Result<std::optional<assets::Sha256Digest>> nullable_digest_from(
    const std::vector<core::json::Member>& members, std::string_view key) {
    const core::json::Member* member = codec::find(members, key);
    if (member == nullptr) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(
            codec::missing(key));
    }
    if (member->raw_value == "null") {
        return core::Result<std::optional<assets::Sha256Digest>>::success(std::nullopt);
    }
    const auto decoded = core::json::decode_string(member->raw_value);
    if (!decoded) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(decoded.error());
    }
    const auto digest = assets::Sha256Digest::from_hex(decoded.value());
    if (!digest) {
        return core::Result<std::optional<assets::Sha256Digest>>::failure(digest.error());
    }
    return core::Result<std::optional<assets::Sha256Digest>>::success(digest.value());
}

} // namespace carto::workspace::codec
