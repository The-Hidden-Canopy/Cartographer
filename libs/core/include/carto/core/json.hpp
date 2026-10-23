#pragma once

#include <carto/core/result.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace carto::core::json {

// This is deliberately a small JSON boundary, not a general-purpose DOM.
// It validates JSON grammar, retains top-level member values as views into the
// caller's bounded input, and rejects duplicate object keys.
struct Member {
    std::string key;
    std::string_view raw_value;
};

namespace detail {

inline Diagnostic malformed(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

inline bool whitespace(char value) noexcept {
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

inline int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

inline Result<std::string> decode_string_token(std::string_view token) {
    if (token.size() < 2U || token.front() != '"' || token.back() != '"') {
        return Result<std::string>::failure(malformed("JSON string token is invalid"));
    }
    std::string value;
    value.reserve(token.size() - 2U);
    const std::size_t content_end = token.size() - 1U;
    const auto read_code_unit = [token, content_end](std::size_t& cursor)
        -> std::optional<std::uint16_t> {
        if (cursor > content_end || content_end - cursor < 4U) {
            return std::nullopt;
        }
        std::uint16_t code_unit = 0U;
        for (std::size_t digit = 0U; digit < 4U; ++digit) {
            const int nibble = hex_value(token[cursor + digit]);
            if (nibble < 0) return std::nullopt;
            code_unit = static_cast<std::uint16_t>((code_unit << 4U) |
                                                    static_cast<std::uint16_t>(nibble));
        }
        cursor += 4U;
        return code_unit;
    };
    const auto append_utf8 = [&value](std::uint32_t code_point) {
        if (code_point <= 0x7fU) {
            value.push_back(static_cast<char>(code_point));
        } else if (code_point <= 0x7ffU) {
            value.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
            value.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else if (code_point <= 0xffffU) {
            value.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
            value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            value.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        } else {
            value.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
            value.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
            value.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
            value.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
        }
    };
    for (std::size_t index = 1U; index < content_end;) {
        const char byte = token[index];
        if (byte != '\\') {
            value.push_back(byte);
            ++index;
            continue;
        }
        ++index;
        if (index >= content_end) {
            return Result<std::string>::failure(malformed("JSON string escape is incomplete"));
        }
        const char escaped = token[index++];
        switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            const auto first = read_code_unit(index);
            if (!first.has_value()) {
                return Result<std::string>::failure(malformed("JSON unicode escape is invalid"));
            }
            std::uint32_t code_point = first.value();
            if (first.value() >= 0xd800U && first.value() <= 0xdbffU) {
                if (content_end - index < 6U || token[index] != '\\' || token[index + 1U] != 'u') {
                    return Result<std::string>::failure(
                        malformed("JSON unicode surrogate pair is incomplete"));
                }
                index += 2U;
                const auto second = read_code_unit(index);
                if (!second.has_value() || second.value() < 0xdc00U || second.value() > 0xdfffU) {
                    return Result<std::string>::failure(
                        malformed("JSON unicode surrogate pair is invalid"));
                }
                code_point = 0x10000U +
                    ((static_cast<std::uint32_t>(first.value()) - 0xd800U) << 10U) +
                    (static_cast<std::uint32_t>(second.value()) - 0xdc00U);
            } else if (first.value() >= 0xdc00U && first.value() <= 0xdfffU) {
                return Result<std::string>::failure(
                    malformed("JSON unicode low surrogate is unpaired"));
            }
            append_utf8(code_point);
            break;
        }
        default:
            return Result<std::string>::failure(malformed("JSON string escape is invalid"));
        }
    }
    return Result<std::string>::success(std::move(value));
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    [[nodiscard]] Result<std::vector<Member>> parse_object() {
        skip_space();
        const auto object = parse_object_body();
        if (!object) return object;
        skip_space();
        if (cursor_ != text_.size()) {
            return Result<std::vector<Member>>::failure(
                malformed("JSON object has trailing data"));
        }
        return object;
    }

private:
    static constexpr std::size_t kMaxDepth = 128U;
    static constexpr std::size_t kMaxMembers = 4096U;

    void skip_space() noexcept {
        while (cursor_ < text_.size() && whitespace(text_[cursor_])) ++cursor_;
    }

    [[nodiscard]] Result<std::string_view> parse_string_token() {
        if (cursor_ >= text_.size() || text_[cursor_] != '"') {
            return Result<std::string_view>::failure(malformed("JSON object key is not a string"));
        }
        const std::size_t start = cursor_++;
        while (cursor_ < text_.size()) {
            const char byte = text_[cursor_++];
            if (byte == '"') {
                return Result<std::string_view>::success(text_.substr(start, cursor_ - start));
            }
            if (byte == '\\') {
                if (cursor_ >= text_.size()) {
                    return Result<std::string_view>::failure(
                        malformed("JSON string escape is incomplete"));
                }
                const char escaped = text_[cursor_++];
                if (escaped == 'u') {
                    if (cursor_ + 4U > text_.size()) {
                        return Result<std::string_view>::failure(
                            malformed("JSON unicode escape is incomplete"));
                    }
                    for (std::size_t digit = 0U; digit < 4U; ++digit) {
                        if (hex_value(text_[cursor_ + digit]) < 0) {
                            return Result<std::string_view>::failure(
                                malformed("JSON unicode escape is invalid"));
                        }
                    }
                    cursor_ += 4U;
                } else if (escaped != '"' && escaped != '\\' && escaped != '/' &&
                           escaped != 'b' && escaped != 'f' && escaped != 'n' &&
                           escaped != 'r' && escaped != 't') {
                    return Result<std::string_view>::failure(
                        malformed("JSON string escape is invalid"));
                }
            } else if (static_cast<unsigned char>(byte) < 0x20U) {
                return Result<std::string_view>::failure(
                    malformed("JSON string contains a control byte"));
            }
        }
        return Result<std::string_view>::failure(malformed("JSON string is unterminated"));
    }

    [[nodiscard]] Result<void> parse_number() {
        if (text_[cursor_] == '-') ++cursor_;
        if (cursor_ >= text_.size()) {
            return Result<void>::failure(malformed("JSON number is incomplete"));
        }
        if (text_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
                return Result<void>::failure(malformed("JSON number has a leading zero"));
            }
        } else {
            if (text_[cursor_] < '1' || text_[cursor_] > '9') {
                return Result<void>::failure(malformed("JSON number is invalid"));
            }
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') ++cursor_;
        }
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            const std::size_t fraction = cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') ++cursor_;
            if (cursor_ == fraction) {
                return Result<void>::failure(malformed("JSON number fraction is incomplete"));
            }
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) ++cursor_;
            const std::size_t exponent = cursor_;
            while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') ++cursor_;
            if (cursor_ == exponent) {
                return Result<void>::failure(malformed("JSON number exponent is incomplete"));
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> parse_literal(std::string_view literal) {
        if (text_.substr(cursor_, literal.size()) != literal) {
            return Result<void>::failure(malformed("JSON literal is invalid"));
        }
        cursor_ += literal.size();
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> parse_array_body() {
        if (depth_ >= kMaxDepth) {
            return Result<void>::failure(malformed("JSON nesting exceeds the safety limit"));
        }
        ++depth_;
        struct DepthGuard {
            std::size_t& depth;
            ~DepthGuard() { --depth; }
        } guard{depth_};
        ++cursor_;
        skip_space();
        if (cursor_ < text_.size() && text_[cursor_] == ']') {
            ++cursor_;
            return Result<void>::success();
        }
        std::size_t elements = 0U;
        while (true) {
            if (++elements > kMaxMembers) {
                return Result<void>::failure(malformed("JSON array exceeds the member limit"));
            }
            if (auto result = parse_value(); !result) return result;
            skip_space();
            if (cursor_ >= text_.size()) {
                return Result<void>::failure(malformed("JSON array is unterminated"));
            }
            if (text_[cursor_] == ']') {
                ++cursor_;
                return Result<void>::success();
            }
            if (text_[cursor_] != ',') {
                return Result<void>::failure(malformed("JSON array separator is invalid"));
            }
            ++cursor_;
            skip_space();
        }
    }

    [[nodiscard]] Result<void> parse_value() {
        skip_space();
        if (cursor_ >= text_.size()) {
            return Result<void>::failure(malformed("JSON value is missing"));
        }
        switch (text_[cursor_]) {
        case '"': {
            const auto string = parse_string_token();
            return string ? Result<void>::success() : Result<void>::failure(string.error());
        }
        case '{': {
            const auto nested = parse_object_body();
            return nested ? Result<void>::success() : Result<void>::failure(nested.error());
        }
        case '[':
            return parse_array_body();
        case 't':
            return parse_literal("true");
        case 'f':
            return parse_literal("false");
        case 'n':
            return parse_literal("null");
        default:
            if (text_[cursor_] == '-' || (text_[cursor_] >= '0' && text_[cursor_] <= '9')) {
                return parse_number();
            }
            return Result<void>::failure(malformed("JSON value is invalid"));
        }
    }

    [[nodiscard]] Result<std::vector<Member>> parse_object_body() {
        if (cursor_ >= text_.size() || text_[cursor_] != '{') {
            return Result<std::vector<Member>>::failure(malformed("JSON value is not an object"));
        }
        if (depth_ >= kMaxDepth) {
            return Result<std::vector<Member>>::failure(malformed("JSON nesting exceeds the safety limit"));
        }
        ++depth_;
        struct DepthGuard {
            std::size_t& depth;
            ~DepthGuard() { --depth; }
        } guard{depth_};
        ++cursor_;
        skip_space();
        std::vector<Member> members;
        if (cursor_ < text_.size() && text_[cursor_] == '}') {
            ++cursor_;
            return Result<std::vector<Member>>::success(std::move(members));
        }
        while (true) {
            if (members.size() >= kMaxMembers) {
                return Result<std::vector<Member>>::failure(malformed("JSON object exceeds the member limit"));
            }
            const auto key_token = parse_string_token();
            if (!key_token) return Result<std::vector<Member>>::failure(key_token.error());
            const auto key = decode_string_token(key_token.value());
            if (!key) return Result<std::vector<Member>>::failure(key.error());
            skip_space();
            if (cursor_ >= text_.size() || text_[cursor_] != ':') {
                return Result<std::vector<Member>>::failure(malformed("JSON object member has no colon"));
            }
            ++cursor_;
            skip_space();
            const std::size_t value_start = cursor_;
            if (auto result = parse_value(); !result) {
                return Result<std::vector<Member>>::failure(result.error());
            }
            const std::size_t value_end = cursor_;
            if (std::any_of(members.begin(), members.end(), [&key](const Member& member) {
                    return member.key == key.value();
                })) {
                return Result<std::vector<Member>>::failure(malformed("JSON object has duplicate keys"));
            }
            members.push_back(Member{key.value(), text_.substr(value_start, value_end - value_start)});
            skip_space();
            if (cursor_ >= text_.size()) {
                return Result<std::vector<Member>>::failure(malformed("JSON object is unterminated"));
            }
            if (text_[cursor_] == '}') {
                ++cursor_;
                return Result<std::vector<Member>>::success(std::move(members));
            }
            if (text_[cursor_] != ',') {
                return Result<std::vector<Member>>::failure(malformed("JSON object separator is invalid"));
            }
            ++cursor_;
            skip_space();
        }
    }

    std::string_view text_;
    std::size_t cursor_ = 0U;
    std::size_t depth_ = 0U;
};

} // namespace detail

[[nodiscard]] inline Result<std::vector<Member>> parse_object(std::string_view text) {
    return detail::Parser(text).parse_object();
}

[[nodiscard]] inline Result<std::string> decode_string(std::string_view token) {
    return detail::decode_string_token(token);
}

[[nodiscard]] inline Result<std::uint64_t> parse_uint(std::string_view value) {
    if (value.empty() || value.front() == '-') {
        return Result<std::uint64_t>::failure(
            detail::malformed("JSON unsigned integer is invalid"));
    }
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return Result<std::uint64_t>::failure(
            detail::malformed("JSON unsigned integer is invalid"));
    }
    return Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] inline const Member* find_member(
    const std::vector<Member>& members,
    std::string_view key) noexcept {
    const auto found = std::find_if(members.begin(), members.end(), [key](const Member& member) {
        return member.key == key;
    });
    return found == members.end() ? nullptr : &*found;
}

[[nodiscard]] inline bool is_object(std::string_view text) {
    return static_cast<bool>(parse_object(text));
}

} // namespace carto::core::json
