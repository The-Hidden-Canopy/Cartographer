#pragma once

#include <cstddef>
#include <string_view>

namespace carto::sim {

// Declared capacity ceilings for the public groom reference contract.
// Requests exceeding these limits fail closed before a bake begins.
struct SimCaps {
    static constexpr std::size_t kMaxGuides = 256U;
    static constexpr std::size_t kMaxGuidePoints = 64U;
    static constexpr std::size_t kMaxFrames = 240U;
    static constexpr std::size_t kMaxSubsteps = 16U;
    static constexpr std::size_t kMaxCollisionFilters = 64U;
};

inline constexpr std::string_view kGroomSpringBackendIdentity =
    "carto-sim-cpu-guide-spring-reference";
inline constexpr std::string_view kFloat64PrecisionIdentity = "float64";

} // namespace carto::sim
