#pragma once

#include <carto/core/math.hpp>

#include <cstddef>
#include <vector>

namespace carto::navgraph {

// Memberwise equality for core math values. The core math types deliberately
// do not define operator==, so neutral record equality spells the comparison
// out; this keeps double comparison semantics explicit (bit-wise value
// equality on the same deterministic computation) on every record.
[[nodiscard]] inline bool vec3_equal(const core::Vec3d& left, const core::Vec3d& right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

[[nodiscard]] inline bool vec3_span_equal(
    const std::vector<core::Vec3d>& left,
    const std::vector<core::Vec3d>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (!vec3_equal(left[index], right[index])) return false;
    }
    return true;
}

[[nodiscard]] inline bool bounds_equal(
    const core::Bounds3d& left,
    const core::Bounds3d& right) noexcept {
    return vec3_equal(left.minimum, right.minimum) &&
        vec3_equal(left.maximum, right.maximum);
}

} // namespace carto::navgraph
