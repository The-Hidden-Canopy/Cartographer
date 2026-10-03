#pragma once

#include <carto/core/result.hpp>

#include <string_view>

namespace carto::editor {

struct NumericValue {
    double value = 0.0;
    bool relative = false;

    [[nodiscard]] constexpr bool operator==(const NumericValue&) const noexcept = default;
};

// Parses the bounded scalar syntax shared by future modal authoring tools.
// Values are expressed in meters; supported suffixes are m, cm, mm, in, and
// ft. A leading += or -= marks a relative delta, while an unsuffixed value is
// absolute. Expressions and axis locks are intentionally not accepted here.
[[nodiscard]] core::Result<NumericValue> parse_numeric_value(std::string_view text);

// Resolves an absolute value or relative delta against a finite base value.
[[nodiscard]] core::Result<double> resolve_numeric_value(
    std::string_view text,
    double base);

} // namespace carto::editor
