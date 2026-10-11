#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/scene/scene.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::connection {

// Connection ports are neutral, durable authoring records: stable identity,
// explicit owner binding, a local reference frame, and compatibility metadata.
// Connection *state* (what is attached right now) is runtime/game state and is
// deliberately absent here; see attachment.hpp for its fenced-out record.

struct PortTag;
using PortId = world::Id<PortTag>;

inline constexpr std::uint32_t kConnectionPortVersion = 1U;
inline constexpr std::uint32_t kPortCompatibilityEvidenceVersion = 1U;
inline constexpr std::size_t kMaxConnectionPortBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t kMaxCompatibilityEvidenceBytes = 256U * 1024U;
inline constexpr std::size_t kMaxCompatibilityTagsPerPort = 32U;
inline constexpr std::size_t kMaxTagSpaceBytes = 64U;
inline constexpr std::size_t kMaxTagTermBytes = 96U;
inline constexpr std::size_t kMaxCapabilitiesPerContract = 32U;
inline constexpr std::size_t kMaxCapabilityBytes = 96U;
inline constexpr std::size_t kMaxFrameIdentityBytes = 128U;
inline constexpr std::size_t kMaxConventionBytes = 256U;

// A named local coordinate frame. `parent_frame` names the frame the local
// transform is expressed against; the empty optional marks a root frame.
// `coordinate_convention` is a stable reference to a shared convention record,
// never an inline convention payload.
struct ReferenceFrame {
    std::string frame_identity;
    std::optional<std::string> parent_frame;
    core::Transform transform_to_parent = core::Transform::identity();
    std::string coordinate_convention;

    [[nodiscard]] core::Result<void> validate() const;
};

// A namespaced compatibility vocabulary term. Two ports can only join when
// they share at least one identical tag; terms are data, not behavior.
struct PortCompatibilityTag {
    std::string space;
    std::string term;

    [[nodiscard]] bool operator==(const PortCompatibilityTag&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const PortCompatibilityTag&) const noexcept = default;

    [[nodiscard]] core::Result<void> validate() const;
};

// Data-only domain vocabularies. These are the complete well-known term sets
// for each namespace; unknown terms remain representable but are not promoted
// to vocabulary constants.
namespace compatibility {

namespace mechanical_hitch {
inline const PortCompatibilityTag three_point_category_0{"mechanical-hitch", "three-point-category-0"};
inline const PortCompatibilityTag three_point_category_1{"mechanical-hitch", "three-point-category-1"};
inline const PortCompatibilityTag three_point_category_2{"mechanical-hitch", "three-point-category-2"};
inline const PortCompatibilityTag three_point_category_3{"mechanical-hitch", "three-point-category-3"};
inline const PortCompatibilityTag drawbar_eye_40mm{"mechanical-hitch", "drawbar-eye-40mm"};
inline const PortCompatibilityTag drawbar_eye_50mm{"mechanical-hitch", "drawbar-eye-50mm"};
inline const PortCompatibilityTag drawbar_eye_80mm{"mechanical-hitch", "drawbar-eye-80mm"};
inline const PortCompatibilityTag pin_hitch_19mm{"mechanical-hitch", "pin-hitch-19mm"};
inline const PortCompatibilityTag pin_hitch_25mm{"mechanical-hitch", "pin-hitch-25mm"};
inline const PortCompatibilityTag pin_hitch_32mm{"mechanical-hitch", "pin-hitch-32mm"};
inline const PortCompatibilityTag fifth_wheel_2inch{"mechanical-hitch", "fifth-wheel-2inch"};
inline const PortCompatibilityTag fifth_wheel_3_5inch{"mechanical-hitch", "fifth-wheel-3.5inch"};
inline const PortCompatibilityTag pto_540rpm{"mechanical-hitch", "pto-540rpm"};
inline const PortCompatibilityTag pto_1000rpm{"mechanical-hitch", "pto-1000rpm"};
} // namespace mechanical_hitch

namespace equipment {
inline const PortCompatibilityTag hand_grip{"equipment", "hand-grip"};
inline const PortCompatibilityTag hand_trigger_guard{"equipment", "hand-trigger-guard"};
inline const PortCompatibilityTag back_mount{"equipment", "back-mount"};
inline const PortCompatibilityTag hip_holster{"equipment", "hip-holster"};
inline const PortCompatibilityTag shoulder_sling{"equipment", "shoulder-sling"};
inline const PortCompatibilityTag chest_rig{"equipment", "chest-rig"};
inline const PortCompatibilityTag head_rail{"equipment", "head-rail"};
inline const PortCompatibilityTag foot_mount{"equipment", "foot-mount"};
} // namespace equipment

namespace door_hinge {
inline const PortCompatibilityTag leaf_pair_8mm{"door-hinge", "leaf-pair-8mm"};
inline const PortCompatibilityTag leaf_pair_10mm{"door-hinge", "leaf-pair-10mm"};
inline const PortCompatibilityTag leaf_pair_12mm{"door-hinge", "leaf-pair-12mm"};
inline const PortCompatibilityTag pin_8mm{"door-hinge", "pin-8mm"};
inline const PortCompatibilityTag pin_10mm{"door-hinge", "pin-10mm"};
inline const PortCompatibilityTag handle_lever{"door-hinge", "handle-lever"};
inline const PortCompatibilityTag handle_knob{"door-hinge", "handle-knob"};
inline const PortCompatibilityTag handle_wheel{"door-hinge", "handle-wheel"};
inline const PortCompatibilityTag sliding_track_25mm{"door-hinge", "sliding-track-25mm"};
inline const PortCompatibilityTag sliding_track_50mm{"door-hinge", "sliding-track-50mm"};
inline const PortCompatibilityTag latch_strike{"door-hinge", "latch-strike"};
inline const PortCompatibilityTag bolt_throw{"door-hinge", "bolt-throw"};
} // namespace door_hinge

namespace maritime {
inline const PortCompatibilityTag mooring_bollard{"maritime", "mooring-bollard"};
inline const PortCompatibilityTag mooring_cleat{"maritime", "mooring-cleat"};
inline const PortCompatibilityTag mooring_bitts{"maritime", "mooring-bitts"};
inline const PortCompatibilityTag mooring_tire{"maritime", "mooring-tire"};
inline const PortCompatibilityTag gangway_1m{"maritime", "gangway-1m"};
inline const PortCompatibilityTag gangway_2m{"maritime", "gangway-2m"};
inline const PortCompatibilityTag gangway_accommodation{"maritime", "gangway-accommodation"};
inline const PortCompatibilityTag cargo_spreader_20ft{"maritime", "cargo-spreader-20ft"};
inline const PortCompatibilityTag cargo_spreader_40ft{"maritime", "cargo-spreader-40ft"};
inline const PortCompatibilityTag deck_twistlock{"maritime", "deck-twistlock"};
inline const PortCompatibilityTag deck_lashing_point{"maritime", "deck-lashing-point"};
inline const PortCompatibilityTag deck_crane_pad{"maritime", "deck-crane-pad"};
inline const PortCompatibilityTag hull_fender_cone{"maritime", "hull-fender-cone"};
inline const PortCompatibilityTag hull_breeching_notch{"maritime", "hull-breeching-notch"};
} // namespace maritime

namespace utility {
inline const PortCompatibilityTag electrical_16a{"utility", "electrical-16a"};
inline const PortCompatibilityTag electrical_32a{"utility", "electrical-32a"};
inline const PortCompatibilityTag electrical_63a{"utility", "electrical-63a"};
inline const PortCompatibilityTag electrical_400v{"utility", "electrical-400v"};
inline const PortCompatibilityTag water_25mm{"utility", "water-25mm"};
inline const PortCompatibilityTag water_50mm{"utility", "water-50mm"};
inline const PortCompatibilityTag water_100mm{"utility", "water-100mm"};
inline const PortCompatibilityTag pneumatic_8mm{"utility", "pneumatic-8mm"};
inline const PortCompatibilityTag pneumatic_12mm{"utility", "pneumatic-12mm"};
inline const PortCompatibilityTag hydraulic_19mm{"utility", "hydraulic-19mm"};
inline const PortCompatibilityTag hydraulic_32mm{"utility", "hydraulic-32mm"};
inline const PortCompatibilityTag fuel_diesel_50mm{"utility", "fuel-diesel-50mm"};
inline const PortCompatibilityTag fuel_gasoline_40mm{"utility", "fuel-gasoline-40mm"};
inline const PortCompatibilityTag data_rj45{"utility", "data-rj45"};
inline const PortCompatibilityTag data_fiber_lc{"utility", "data-fiber-lc"};
inline const PortCompatibilityTag data_coax{"utility", "data-coax"};
} // namespace utility

} // namespace compatibility

// Well-known capability terms for PortCapabilityContract. Capabilities are
// plain data strings; contracts carry no executable semantics.
namespace capabilities {
inline constexpr std::string_view load_bearing = "load-bearing";
inline constexpr std::string_view power_transfer = "power-transfer";
inline constexpr std::string_view signal_transfer = "signal-transfer";
inline constexpr std::string_view fluid_transfer = "fluid-transfer";
inline constexpr std::string_view data_transfer = "data-transfer";
inline constexpr std::string_view kinematic_articulation = "kinematic-articulation";
inline constexpr std::string_view latching = "latching";
inline constexpr std::string_view guided_insert = "guided-insert";
inline constexpr std::string_view locking = "locking";
inline constexpr std::string_view hot_swap = "hot-swap";
} // namespace capabilities

// Required/offered capability sets. A join is only admissible when every
// required capability on one side is offered by the other.
struct PortCapabilityContract {
    std::vector<std::string> required;
    std::vector<std::string> offered;

    [[nodiscard]] core::Result<void> validate() const;
};

// Deterministic evidence that a specific endpoint pair was admissible: the
// exact shared tags plus the requirement names each side satisfied.
struct PortCompatibilityEvidence {
    std::vector<PortCompatibilityTag> shared_tags;
    std::vector<std::string> first_satisfied_requirements;
    std::vector<std::string> second_satisfied_requirements;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PortCompatibilityEvidence> deserialize(
        std::string_view text);
};

// One authored connection interface. `owner` binds the port to a durable
// scene object identity; `authoring` optionally carries the provider-neutral
// world binding. Neither grants runtime mutation authority.
struct ConnectionPort {
    PortId id;
    scene::ObjectId owner;
    world::AuthoringBinding authoring;
    ReferenceFrame local_frame;
    std::vector<PortCompatibilityTag> compatibility_tags;
    PortCapabilityContract capability_contract;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ConnectionPort> deserialize(std::string_view text);
};

// Fail-closed compatibility gate. Incompatible endpoints are refused before
// any state change; the check is pure and mutates nothing.
[[nodiscard]] core::Result<PortCompatibilityEvidence> check_port_compatibility(
    const ConnectionPort& first,
    const ConnectionPort& second);

} // namespace carto::connection
