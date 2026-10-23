#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstdint>
#include <variant>

namespace carto::attributes {

enum class AttributeType {
    boolean,
    int32,
    uint32,
    float32,
    float64,
    vec2,
    vec3,
    vec4,
    color4,
    string_id,
    object_id,
    vertex_id,
};

using AttributeValue = std::variant<
    bool,
    std::int32_t,
    std::uint32_t,
    float,
    double,
    core::Vec2d,
    core::Vec3d,
    core::Vec4d,
    std::uint64_t>;

[[nodiscard]] bool valid_type(AttributeType type) noexcept;
[[nodiscard]] AttributeValue default_value(AttributeType type);
[[nodiscard]] core::Result<void> validate_value(
    AttributeType type,
    const AttributeValue& value);

} // namespace carto::attributes
