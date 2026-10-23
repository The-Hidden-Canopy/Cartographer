#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/ids.hpp>

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace carto::character {

enum class ControlShape : std::uint8_t {
    sphere,
    box,
    curve,
    locator,
};

[[nodiscard]] std::string_view control_shape_name(ControlShape shape) noexcept;

// Bitmask over the six allowed motion axes: translation X/Y/Z then rotation
// X/Y/Z. A control with no allowed axes is legal (display-only) but any set
// bit outside kAllowedAxesMask is invalid.
inline constexpr std::uint8_t kAxisTranslateX = 1U << 0U;
inline constexpr std::uint8_t kAxisTranslateY = 1U << 1U;
inline constexpr std::uint8_t kAxisTranslateZ = 1U << 2U;
inline constexpr std::uint8_t kAxisRotateX = 1U << 3U;
inline constexpr std::uint8_t kAxisRotateY = 1U << 4U;
inline constexpr std::uint8_t kAxisRotateZ = 1U << 5U;
inline constexpr std::uint8_t kAllowedAxesMask =
    kAxisTranslateX | kAxisTranslateY | kAxisTranslateZ |
    kAxisRotateX | kAxisRotateY | kAxisRotateZ;
inline constexpr std::size_t kAxisCount = 6U;

// One user-facing rig control. It is durable authoring truth only: shape,
// rest transform, which joints it drives, and what motion it admits. Current
// control offsets and evaluated poses are runtime-owned and absent here.
struct ControlDefinition {
    ControlId id;
    ControlShape shape = ControlShape::locator;
    core::Transform transform = core::Transform::identity();
    // Joints this control affects, sorted and unique by stable JointId.
    std::vector<kinematic::JointId> affected_joints;
    std::uint8_t allowed_axes = 0U;
    // Per-axis value limits in the same axis order as the bitmask; a control
    // may only restrict axes it allows.
    std::array<double, kAxisCount> axis_minimum{};
    std::array<double, kAxisCount> axis_maximum{};
    // Optional follow-space: this control inherits motion from another
    // control in the same graph. Must not introduce a dependency cycle.
    std::optional<ControlId> follow_space;
    // Control-set membership ("face.primary", "body.core"), a bounded safe
    // token; empty means unassigned.
    std::string control_set;

    [[nodiscard]] core::Result<void> validate() const;
};

// One dependency edge in the control DAG: `follower` follows `driver`.
struct ControlEdge {
    ControlId driver;
    ControlId follower;

    [[nodiscard]] bool operator==(const ControlEdge&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const ControlEdge&) const noexcept = default;
};

// The authored control graph for one rig. Together with the kinematic
// hierarchy (which is already a validated tree in carto::kinematic) it forms
// the control+rig dependency DAG named by ch04; this class validates the
// control side: referents exist, follow-space edges resolve, and the combined
// control edges are acyclic.
class ControlGraph final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxControls = 4'096U;
    static constexpr std::size_t kMaxEdges = 16'384U;
    static constexpr std::size_t kMaxAffectedJointsPerControl = 256U;
    static constexpr std::size_t kMaxControlSetBytes = 128U;

    [[nodiscard]] core::Result<void> insert(ControlDefinition control);
    [[nodiscard]] core::Result<void> insert_edge(ControlEdge edge);

    [[nodiscard]] const std::map<ControlId, ControlDefinition>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const std::vector<ControlEdge>& edges() const noexcept { return edges_; }
    [[nodiscard]] const ControlDefinition* find(ControlId id) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;
    // Deterministic canonical digest over the validated graph; fails closed
    // when the graph does not validate.
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<ControlId, ControlDefinition> nodes_;
    std::vector<ControlEdge> edges_;
};

} // namespace carto::character
