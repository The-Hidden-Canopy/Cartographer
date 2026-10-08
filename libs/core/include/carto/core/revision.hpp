#pragma once

#include <cstdint>
#include <compare>
#include <limits>

namespace carto::core {

class Revision {
public:
    constexpr Revision() = default;
    explicit constexpr Revision(std::uint64_t value) : value_(value) {}

    [[nodiscard]] static constexpr std::uint64_t max_value() noexcept {
        return std::numeric_limits<std::uint64_t>::max();
    }
    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool exhausted() const noexcept { return value_ == max_value(); }
    [[nodiscard]] constexpr bool operator==(const Revision&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Revision&) const noexcept = default;

    // Revision owners must reject mutation when exhausted. Saturating here is
    // a second line of defense against wraparound in any remaining call site.
    constexpr Revision next() const noexcept {
        return exhausted() ? *this : Revision(value_ + 1U);
    }

private:
    std::uint64_t value_ = 0;
};

} // namespace carto::core
