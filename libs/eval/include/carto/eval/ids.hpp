#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace carto::eval {

struct NodeId {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const NodeId&) const noexcept = default;
};

struct NodeTypeId {
    std::string value;

    [[nodiscard]] bool operator==(const NodeTypeId&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const NodeTypeId& other) const noexcept {
        return value <=> other.value;
    }
};

struct PortId {
    std::string value;

    [[nodiscard]] bool operator==(const PortId&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const PortId& other) const noexcept {
        return value <=> other.value;
    }
};

} // namespace carto::eval
