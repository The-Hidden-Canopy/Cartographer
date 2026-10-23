#pragma once

#include <carto/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace carto::character {

// Declared coordinate convention at every interchange boundary (ch04
// "Coordinate policy"). Cartographer source remains canonical in its own
// convention; a producer targeting a profile with a different policy must
// build an exact transformed derived product and record conversion evidence,
// or fail before admission. No hidden runtime axis flip.
enum class Handedness : std::uint8_t {
    right,
    left,
};

enum class Axis : std::uint8_t {
    positive_x,
    negative_x,
    positive_y,
    negative_y,
    positive_z,
    negative_z,
};

enum class WindingOrder : std::uint8_t {
    counter_clockwise,
    clockwise,
};

// Orientation transport is normalized quaternions under a documented sign
// rule: the canonical hemisphere is w >= 0 (real part non-negative); every
// stored or exchanged quaternion is normalized to that hemisphere, and
// non-finite or zero-length rotations are forbidden (enforced by core math
// validation at the record level).
enum class RotationRepresentation : std::uint8_t {
    quaternion_normalized_positive_w,
};

// The space inverse bind matrices are expressed in at the interchange
// boundary. World-space binds and joint-local binds differ by one rest
// transform and silently mixing them flips skinning.
enum class InverseBindSpace : std::uint8_t {
    world_space,
    joint_local,
};

struct CoordinatePolicy {
    Handedness handedness = Handedness::right;
    Axis up = Axis::positive_y;
    Axis forward = Axis::positive_z;
    double meters_per_unit = 1.0;
    WindingOrder winding = WindingOrder::counter_clockwise;
    RotationRepresentation rotation = RotationRepresentation::quaternion_normalized_positive_w;
    InverseBindSpace inverse_bind = InverseBindSpace::world_space;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const CoordinatePolicy&) const noexcept = default;
};

// Every entity class a transform conversion must consistently transform
// (ch04 coordinate checklist). A conversion is not admissible until the
// producer has explicitly confirmed each class; the checklist is the
// fail-closed gate.
enum class TransformedEntityClass : std::uint8_t {
    mesh_positions,
    normals_tangents,
    joint_rest_transforms,
    inverse_binds,
    root_motion,
    collision_geometry,
    garment_anchors,
    groom_anchors,
    mocap_frames,
};

inline constexpr std::size_t kTransformedEntityClassCount = 9U;

[[nodiscard]] std::string_view transformed_entity_class_name(
    TransformedEntityClass entity_class) noexcept;

struct CoordinateConversionChecklist {
    std::array<bool, kTransformedEntityClassCount> transformed{};

    [[nodiscard]] bool marked(TransformedEntityClass entity_class) const noexcept {
        return transformed[static_cast<std::size_t>(entity_class)];
    }
    void mark(TransformedEntityClass entity_class) noexcept {
        transformed[static_cast<std::size_t>(entity_class)] = true;
    }
    [[nodiscard]] bool complete() const noexcept;
};

// Declared conversion between two coordinate policies. Identity conversions
// (source == target) take the fast path: no checklist is required and no
// transformation may be applied. Any non-identity conversion with an
// incomplete checklist fails closed at validate().
struct CoordinateConversion {
    CoordinatePolicy source;
    CoordinatePolicy target;
    CoordinateConversionChecklist checklist;

    [[nodiscard]] bool identity() const noexcept { return source == target; }
    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
