#include <carto/editor/numeric_entry.hpp>

#include <cmath>
#include <cstdlib>
#include <cctype>
#include <string>
#include <utility>

namespace carto::editor {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1U);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1U);
    return text;
}

double unit_scale(std::string_view suffix) {
    if (suffix.empty() || suffix == "m" || suffix == "meter" || suffix == "meters") return 1.0;
    if (suffix == "cm") return 0.01;
    if (suffix == "mm") return 0.001;
    if (suffix == "in") return 0.0254;
    if (suffix == "ft") return 0.3048;
    return 0.0;
}

class ExpressionParser final {
public:
    explicit ExpressionParser(std::string_view text) : text_(text) {}

    [[nodiscard]] core::Result<double> parse() {
        const auto value = parse_add_subtract(0U);
        if (!value) return value;
        skip_spaces();
        if (position_ != text_.size()) {
            return core::Result<double>::failure(invalid(
                "numeric entry contains an unexpected token"));
        }
        return value;
    }

private:
    static constexpr std::size_t kMaxDepth = 32U;
    static constexpr std::size_t kMaxOperations = 128U;

    void skip_spaces() noexcept {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t')) {
            ++position_;
        }
    }

    [[nodiscard]] core::Result<double> parse_add_subtract(std::size_t depth) {
        if (depth > kMaxDepth) {
            return core::Result<double>::failure(invalid(
                "numeric entry expression nesting is too deep"));
        }
        auto left = parse_multiply_divide(depth + 1U);
        if (!left) return left;
        for (;;) {
            skip_spaces();
            if (position_ >= text_.size() ||
                (text_[position_] != '+' && text_[position_] != '-')) {
                return left;
            }
            if (++operation_count_ > kMaxOperations) {
                return core::Result<double>::failure(invalid(
                    "numeric entry expression has too many operations"));
            }
            const char operation = text_[position_++];
            auto right = parse_multiply_divide(depth + 1U);
            if (!right) return right;
            const double value = operation == '+'
                ? left.value() + right.value()
                : left.value() - right.value();
            if (!std::isfinite(value)) {
                return core::Result<double>::failure(invalid(
                    "numeric entry expression is non-finite"));
            }
            left = core::Result<double>::success(value);
        }
    }

    [[nodiscard]] core::Result<double> parse_multiply_divide(std::size_t depth) {
        if (depth > kMaxDepth) {
            return core::Result<double>::failure(invalid(
                "numeric entry expression nesting is too deep"));
        }
        auto left = parse_primary(depth + 1U);
        if (!left) return left;
        for (;;) {
            skip_spaces();
            if (position_ >= text_.size() ||
                (text_[position_] != '*' && text_[position_] != '/')) {
                return left;
            }
            if (++operation_count_ > kMaxOperations) {
                return core::Result<double>::failure(invalid(
                    "numeric entry expression has too many operations"));
            }
            const char operation = text_[position_++];
            auto right = parse_primary(depth + 1U);
            if (!right) return right;
            if (operation == '/' && right.value() == 0.0) {
                return core::Result<double>::failure(invalid(
                    "numeric entry expression divides by zero"));
            }
            const double value = operation == '*'
                ? left.value() * right.value()
                : left.value() / right.value();
            if (!std::isfinite(value)) {
                return core::Result<double>::failure(invalid(
                    "numeric entry expression is non-finite"));
            }
            left = core::Result<double>::success(value);
        }
    }

    [[nodiscard]] core::Result<double> parse_primary(std::size_t depth) {
        if (depth > kMaxDepth) {
            return core::Result<double>::failure(invalid(
                "numeric entry expression nesting is too deep"));
        }
        skip_spaces();
        if (position_ >= text_.size()) {
            return core::Result<double>::failure(invalid(
                "numeric entry expression is missing a value"));
        }
        if (text_[position_] == '(') {
            ++position_;
            auto value = parse_add_subtract(depth + 1U);
            if (!value) return value;
            skip_spaces();
            if (position_ >= text_.size() || text_[position_] != ')') {
                return core::Result<double>::failure(invalid(
                    "numeric entry expression is missing a closing parenthesis"));
            }
            ++position_;
            return value;
        }

        const std::string token(text_.substr(position_));
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || !std::isfinite(parsed)) {
            return core::Result<double>::failure(invalid(
                "numeric entry expression contains a non-finite or invalid scalar"));
        }
        const std::size_t consumed = static_cast<std::size_t>(end - token.c_str());
        position_ += consumed;
        const std::size_t suffix_start = position_;
        while (position_ < text_.size() &&
               std::isalpha(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        const std::string_view suffix = text_.substr(suffix_start, position_ - suffix_start);
        const double scale = unit_scale(suffix);
        if (scale == 0.0) {
            return core::Result<double>::failure(invalid(
                "numeric entry uses an unsupported unit suffix"));
        }
        const double value = parsed * scale;
        if (!std::isfinite(value)) {
            return core::Result<double>::failure(invalid(
                "numeric entry conversion is non-finite"));
        }
        return core::Result<double>::success(value);
    }

    std::string_view text_;
    std::size_t position_ = 0U;
    std::size_t operation_count_ = 0U;
};

} // namespace

core::Result<NumericValue> parse_numeric_value(std::string_view text) {
    constexpr std::size_t kMaxBytes = 256U;
    text = trim(text);
    if (text.empty() || text.size() > kMaxBytes || text.find('\0') != std::string_view::npos) {
        return core::Result<NumericValue>::failure(invalid(
            "numeric entry is empty, oversized, or contains a NUL"));
    }

    bool relative = false;
    double sign = 1.0;
    if (text.starts_with("+=")) {
        relative = true;
        text.remove_prefix(2U);
    } else if (text.starts_with("-=")) {
        relative = true;
        sign = -1.0;
        text.remove_prefix(2U);
    }
    text = trim(text);
    if (text.empty()) {
        return core::Result<NumericValue>::failure(invalid(
            "numeric entry has no scalar after its relative prefix"));
    }

    const auto expression = ExpressionParser{text}.parse();
    if (!expression) {
        return core::Result<NumericValue>::failure(expression.error());
    }
    const double value = sign * expression.value();
    if (!std::isfinite(value)) {
        return core::Result<NumericValue>::failure(invalid(
            "numeric entry conversion is non-finite"));
    }
    return core::Result<NumericValue>::success(NumericValue{value, relative});
}

core::Result<double> resolve_numeric_value(std::string_view text, double base) {
    if (!std::isfinite(base)) {
        return core::Result<double>::failure(invalid(
            "numeric entry base must be finite"));
    }
    const auto parsed = parse_numeric_value(text);
    if (!parsed) return core::Result<double>::failure(parsed.error());
    const double resolved = parsed.value().relative
        ? base + parsed.value().value
        : parsed.value().value;
    if (!std::isfinite(resolved)) {
        return core::Result<double>::failure(invalid(
            "numeric entry resolution is non-finite"));
    }
    return core::Result<double>::success(resolved);
}

} // namespace carto::editor
