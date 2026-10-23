#include <carto/character/coordinate.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

int axis_magnitude(Axis axis) noexcept {
    switch (axis) {
    case Axis::positive_x: case Axis::negative_x: return 0;
    case Axis::positive_y: case Axis::negative_y: return 1;
    case Axis::positive_z: case Axis::negative_z: return 2;
    }
    return -1;
}

} // namespace

std::string_view transformed_entity_class_name(TransformedEntityClass entity_class) noexcept {
    switch (entity_class) {
    case TransformedEntityClass::mesh_positions: return "mesh-positions";
    case TransformedEntityClass::normals_tangents: return "normals-tangents";
    case TransformedEntityClass::joint_rest_transforms: return "joint-rest-transforms";
    case TransformedEntityClass::inverse_binds: return "inverse-binds";
    case TransformedEntityClass::root_motion: return "root-motion";
    case TransformedEntityClass::collision_geometry: return "collision-geometry";
    case TransformedEntityClass::garment_anchors: return "garment-anchors";
    case TransformedEntityClass::groom_anchors: return "groom-anchors";
    case TransformedEntityClass::mocap_frames: return "mocap-frames";
    }
    return "unknown";
}

bool CoordinateConversionChecklist::complete() const noexcept {
    return std::all_of(transformed.begin(), transformed.end(), [](bool marked) {
        return marked;
    });
}

core::Result<void> CoordinatePolicy::validate() const {
    if (!std::isfinite(meters_per_unit) || meters_per_unit <= 0.0) {
        return core::Result<void>::failure(invalid(
            "coordinate policy meters-per-unit must be positive and finite"));
    }
    if (axis_magnitude(up) < 0 || axis_magnitude(forward) < 0 ||
        axis_magnitude(up) == axis_magnitude(forward)) {
        return core::Result<void>::failure(validation(
            "coordinate policy up and forward axes must be distinct"));
    }
    if (handedness != Handedness::right && handedness != Handedness::left) {
        return core::Result<void>::failure(invalid(
            "coordinate policy handedness is invalid"));
    }
    if (rotation != RotationRepresentation::quaternion_normalized_positive_w) {
        return core::Result<void>::failure(invalid(
            "coordinate policy rotation representation is invalid"));
    }
    if (inverse_bind != InverseBindSpace::world_space &&
        inverse_bind != InverseBindSpace::joint_local) {
        return core::Result<void>::failure(invalid(
            "coordinate policy inverse-bind space is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> CoordinateConversion::validate() const {
    if (auto result = source.validate(); !result) return result;
    if (auto result = target.validate(); !result) return result;
    // Fail-closed checklist gate: any non-identity conversion must carry
    // explicit evidence for every transformed entity class. Identity
    // conversions take the fast path and must not apply any transform.
    if (!identity() && !checklist.complete()) {
        return core::Result<void>::failure(validation(
            "coordinate conversion checklist is incomplete; "
            "every transformed entity class must be confirmed"));
    }
    return core::Result<void>::success();
}

} // namespace carto::character
