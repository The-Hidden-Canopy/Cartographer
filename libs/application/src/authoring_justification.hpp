#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace carto::application::detail {
inline bool justification_valid(std::string_view value) noexcept {
    if (value.empty() || value.size() > 512U) return false;
    bool has_text = false;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        std::uint32_t scalar = first;
        std::uint32_t minimum = 0U;
        std::size_t following = 0U;
        if (first >= 0xc2U && first <= 0xdfU) {
            scalar = first & 0x1fU; minimum = 0x80U; following = 1U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            scalar = first & 0x0fU; minimum = 0x800U; following = 2U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            scalar = first & 0x07U; minimum = 0x10000U; following = 3U;
        } else if (first >= 0x80U) {
            return false;
        }
        if (following > value.size() - i) return false;
        for (std::size_t n = 0; n < following; ++n) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            scalar = (scalar << 6U) | (next & 0x3fU);
        }
        if (scalar < minimum || scalar > 0x10ffffU ||
            (scalar >= 0xd800U && scalar <= 0xdfffU) || scalar < 0x20U || scalar == 0x7fU)
            return false;
        // Unicode White_Space does not by itself supply a justification.
        // Preserve valid UTF-8 text and its exact audit bytes without normalization.
        const bool whitespace = scalar == 0x20U || scalar == 0x85U || scalar == 0xa0U ||
            scalar == 0x1680U || (scalar >= 0x2000U && scalar <= 0x200aU) ||
            scalar == 0x2028U || scalar == 0x2029U || scalar == 0x202fU ||
            scalar == 0x205fU || scalar == 0x3000U;
        has_text = has_text || !whitespace;
    }
    return has_text;
}
} // namespace carto::application::detail
