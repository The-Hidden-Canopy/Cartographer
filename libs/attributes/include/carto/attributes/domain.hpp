#pragma once

namespace carto::attributes {

enum class AttributeDomain {
    object,
    vertex,
    edge,
    face,
    corner,
    curve_point,
    spline,
    joint,
    control_point,
    brep_face,
    brep_edge,
};

[[nodiscard]] const char* domain_name(AttributeDomain domain) noexcept;
[[nodiscard]] bool valid_domain(AttributeDomain domain) noexcept;

} // namespace carto::attributes
