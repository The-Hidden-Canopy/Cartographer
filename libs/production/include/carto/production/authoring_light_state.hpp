#pragma once

#include <carto/project/presentation_catalog.hpp>

namespace carto::production {

// Match saved-scene capture: do not invent a switch/failure state or ignore
// active color-temperature intent. False means the source contributes no light.
[[nodiscard]] inline core::Result<bool> authoring_light_is_active(
    const project::AuthoredLight& light) {
    using Result = core::Result<bool>;
    if (light.normal_state != project::LightNormalState::steady ||
        light.failure_state == project::LightFailureState::intermittent) {
        return Result::failure({core::ErrorCode::unsupported,
            "Authoring viewport cannot choose a switchable or intermittent light state."});
    }
    if (light.failure_state == project::LightFailureState::dark) return Result::success(false);
    const auto radiance = light.linear_color * light.intensity;
    if (!radiance.finite()) return Result::failure({core::ErrorCode::validation_failed,
        "Authored light radiance exceeds finite viewport bounds."});
    if (radiance.x == 0.0 && radiance.y == 0.0 && radiance.z == 0.0)
        return Result::success(false);
    if (light.color_temperature_kelvin) return Result::failure({core::ErrorCode::unsupported,
        "Authoring viewport cannot lower authored color-temperature intent."});
    return Result::success(true);
}

} // namespace carto::production
