#pragma once

// Internal line-format readers and writers shared by the connection record
// serializers. Not public API; see include/carto/connection/*.hpp.

#include <carto/connection/port.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace carto::connection::detail {

inline core::Diagnostic invalid_field(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

inline core::Diagnostic validation_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline core::Diagnostic parse_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

inline bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' && byte != '\n';
    });
}

template <typename T>
inline bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
    value = parsed;
    return true;
}

inline bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
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

inline void write_quoted(std::ostream& output, std::string_view value) {
    output << ' ' << std::quoted(std::string(value));
}

inline bool read_quoted(std::istream& input, std::string& value) {
    return static_cast<bool>(input >> std::quoted(value));
}

inline void write_transform(std::ostream& output, const core::Transform& transform) {
    output << ' ' << transform.translation.x << ' ' << transform.translation.y << ' '
           << transform.translation.z << ' ' << transform.rotation.x << ' '
           << transform.rotation.y << ' ' << transform.rotation.z << ' '
           << transform.rotation.w << ' ' << transform.scale.x << ' ' << transform.scale.y << ' '
           << transform.scale.z;
}

inline bool read_transform(std::istream& input, core::Transform& transform) {
    return static_cast<bool>(
        input >> transform.translation.x >> transform.translation.y >> transform.translation.z >>
        transform.rotation.x >> transform.rotation.y >> transform.rotation.z >>
        transform.rotation.w >> transform.scale.x >> transform.scale.y >> transform.scale.z);
}

inline void write_tag(std::ostream& output, const PortCompatibilityTag& tag) {
    write_quoted(output, tag.space);
    write_quoted(output, tag.term);
}

inline bool read_tag(std::istream& input, PortCompatibilityTag& tag) {
    return read_quoted(input, tag.space) && read_quoted(input, tag.term);
}

inline bool read_tag_list(
    std::istream& input,
    std::vector<PortCompatibilityTag>& tags,
    std::size_t maximum) {
    std::size_t count = 0U;
    if (!read_count(input, maximum, count)) return false;
    tags.clear();
    tags.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PortCompatibilityTag tag;
        if (!read_tag(input, tag)) return false;
        tags.push_back(std::move(tag));
    }
    return true;
}

inline bool read_string_list(
    std::istream& input,
    std::vector<std::string>& values,
    std::size_t maximum) {
    std::size_t count = 0U;
    if (!read_count(input, maximum, count)) return false;
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::string value;
        if (!read_quoted(input, value)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

inline void write_string_list(
    std::ostream& output,
    std::string_view label,
    const std::vector<std::string>& values) {
    output << ' ' << label << ' ' << values.size();
    for (const auto& value : values) write_quoted(output, value);
}

// One full port record, shared by the standalone ConnectionPort format and
// the ConnectionPortGraph port section.
inline void write_port_record(std::ostream& output, const ConnectionPort& port) {
    output << "PORT " << port.id.value << ' ' << port.owner.value << ' '
           << static_cast<unsigned>(port.authoring.kind) << ' ' << port.authoring.identity;
    output << " FRAME";
    write_quoted(output, port.local_frame.frame_identity);
    write_quoted(output, port.local_frame.parent_frame.value_or(std::string{}));
    write_transform(output, port.local_frame.transform_to_parent);
    write_quoted(output, port.local_frame.coordinate_convention);
    output << " TAGS " << port.compatibility_tags.size();
    for (const auto& tag : port.compatibility_tags) write_tag(output, tag);
    write_string_list(output, "REQUIRED", port.capability_contract.required);
    write_string_list(output, "OFFERED", port.capability_contract.offered);
    output << '\n';
}

inline bool read_port_record(std::istream& input, ConnectionPort& port) {
    std::uint64_t binding_kind = 0U;
    std::string parent_frame;
    if (!require_token(input, "PORT") || !read_unsigned(input, port.id.value) ||
        !read_unsigned(input, port.owner.value) || !read_unsigned(input, binding_kind) ||
        !read_unsigned(input, port.authoring.identity)) {
        return false;
    }
    port.authoring.kind = static_cast<world::AuthoringBindingKind>(binding_kind);
    if (!require_token(input, "FRAME") ||
        !read_quoted(input, port.local_frame.frame_identity) ||
        !read_quoted(input, parent_frame) ||
        !read_transform(input, port.local_frame.transform_to_parent) ||
        !read_quoted(input, port.local_frame.coordinate_convention)) {
        return false;
    }
    port.local_frame.parent_frame = parent_frame.empty()
        ? std::nullopt
        : std::optional<std::string>{std::move(parent_frame)};
    if (!require_token(input, "TAGS") ||
        !read_tag_list(input, port.compatibility_tags, kMaxCompatibilityTagsPerPort) ||
        !require_token(input, "REQUIRED") ||
        !read_string_list(
            input, port.capability_contract.required, kMaxCapabilitiesPerContract) ||
        !require_token(input, "OFFERED") ||
        !read_string_list(
            input, port.capability_contract.offered, kMaxCapabilitiesPerContract)) {
        return false;
    }
    return true;
}

// Evidence payload used both inline (inside an EDGE record) and standalone.
inline void write_evidence_inline(std::ostream& output, const PortCompatibilityEvidence& evidence) {
    output << " SHARED " << evidence.shared_tags.size();
    for (const auto& tag : evidence.shared_tags) write_tag(output, tag);
    write_string_list(output, "FIRST", evidence.first_satisfied_requirements);
    write_string_list(output, "SECOND", evidence.second_satisfied_requirements);
}

inline bool read_evidence_inline(std::istream& input, PortCompatibilityEvidence& evidence) {
    return require_token(input, "SHARED") &&
        read_tag_list(input, evidence.shared_tags, kMaxCompatibilityTagsPerPort) &&
        require_token(input, "FIRST") &&
        read_string_list(
            input, evidence.first_satisfied_requirements, kMaxCapabilitiesPerContract) &&
        require_token(input, "SECOND") &&
        read_string_list(
            input, evidence.second_satisfied_requirements, kMaxCapabilitiesPerContract);
}

} // namespace carto::connection::detail
