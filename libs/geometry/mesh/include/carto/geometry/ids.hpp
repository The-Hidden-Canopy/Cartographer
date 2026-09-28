#pragma once

#include <cstdint>
#include <compare>

namespace carto::geometry {

struct VertexId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const VertexId&) const noexcept = default;
};

struct FaceId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const FaceId&) const noexcept = default;
};

struct HalfEdgeId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const HalfEdgeId&) const noexcept = default;
};

} // namespace carto::geometry
