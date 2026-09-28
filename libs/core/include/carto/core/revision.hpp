#pragma once

#include <cstdint>
#include <compare>

namespace carto::core {

class Revision {
public:
    constexpr Revision() = default;
    explicit constexpr Revision(std::uint64_t value) : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool operator==(const Revision&) const noexcept = default;
    [[nodiscard]] constexpr auto operator<=>(const Revision&) const noexcept = default;

    constexpr Revision next() const noexcept { return Revision(value_ + 1U); }

private:
    std::uint64_t value_ = 0;
};

} // namespace carto::core
