#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>

namespace carto::kinematic {

class KinematicGraph;

// Authored travel bounds for one joint. Limits are durable source truth: they
// describe how far a joint may move, never where it currently is. Current pose,
// velocity and transition state are runtime/game-owned motion state and are
// never part of this record (see require_no_motion_state).
struct JointLimit {
    JointId joint;
    bool angular_enabled = false;
    // Radians about the joint-local x/y/z axes. Disabled axes must be zero so
    // the canonical form is unambiguous.
    core::Vec3d angular_min{};
    core::Vec3d angular_max{};
    bool linear_enabled = false;
    // Meters along the joint-local x/y/z axes. Disabled axes must be zero.
    core::Vec3d linear_min{};
    core::Vec3d linear_max{};

    // Reference containment behavior, kept separate from structural
    // validation: containment is false unless the corresponding axis family
    // is enabled, and every enabled component must lie within its bounds.
    [[nodiscard]] bool contains_angular(core::Vec3d angle) const noexcept;
    [[nodiscard]] bool contains_linear(core::Vec3d offset) const noexcept;

    [[nodiscard]] core::Result<void> validate() const;
};

// Bounded set of authored joint limits, content-addressed like every other
// kinematic record. Limits are referenced from lowering descriptors by digest.
class JointLimitSet final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxLimits = 4'096U;
    // Generous hard bound for authored travel; anything wider is almost
    // certainly a units mistake rather than a real mechanism.
    static constexpr double kMaxAngularMagnitude = 64.0;
    static constexpr double kMaxLinearMagnitude = 1.0e6;

    [[nodiscard]] core::Result<void> insert(JointLimit limit);

    [[nodiscard]] const std::map<JointId, JointLimit>& limits() const noexcept {
        return limits_;
    }
    [[nodiscard]] const JointLimit* find(JointId joint) const noexcept;

    [[nodiscard]] core::Result<void> validate() const;
    // Cross-checks that every limited joint exists in the graph. Class-specific
    // validation instead of silently substituting a default.
    [[nodiscard]] core::Result<void> validate_for_graph(
        const KinematicGraph& graph) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<JointLimitSet> deserialize(
        std::string_view text);

private:
    std::map<JointId, JointLimit> limits_;
};

// Motion state is owned by VANTA and the game: current pose, velocity,
// acceleration, and animation blends are runtime state and must never appear in
// a serialized kinematic record. This negative-space check scans a serialized
// record and fails closed if it carries any motion-state tag. Quoted string
// payloads are skipped so a semantic type such as "COMPOSED" cannot collide
// with a tag by accident.
[[nodiscard]] core::Result<void> require_no_motion_state(
    std::string_view serialized_record);

} // namespace carto::kinematic
