#include <carto/editor/numeric_entry.hpp>

#include <cmath>
#include <cstdlib>
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

    const std::string token(text);
    char* end = nullptr;
    const double parsed = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || !std::isfinite(parsed)) {
        return core::Result<NumericValue>::failure(invalid(
            "numeric entry is not a finite scalar"));
    }
    const std::size_t suffix_offset = static_cast<std::size_t>(end - token.c_str());
    const std::string_view suffix = std::string_view(token).substr(suffix_offset);
    const double scale = unit_scale(suffix);
    if (scale == 0.0) {
        return core::Result<NumericValue>::failure(invalid(
            "numeric entry uses an unsupported unit suffix"));
    }
    const double value = sign * parsed * scale;
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
