#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::temporal {

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct ChannelTag;
struct EventTag;
struct ClipTag;
struct SequenceTag;

using ChannelId = Id<ChannelTag>;
using EventId = Id<EventTag>;
using ClipId = Id<ClipTag>;
using SequenceId = Id<SequenceTag>;

enum class TargetBindingKind : std::uint8_t {
    none,
    rigid_object,
    mechanical_subassembly,
    skinned_mesh,
    camera,
    light,
};

[[nodiscard]] std::string_view target_binding_kind_name(
    TargetBindingKind kind) noexcept;

// Provider-neutral binding back to authoritative Cartographer authoring
// state: a binding kind plus a stable identity string. It carries no runtime
// handle and grants no mutation authority. Joint/skin definitions belong to a
// separate kinematic layer and are referenced here only by their kind.
struct TargetBinding {
    TargetBindingKind kind = TargetBindingKind::none;
    std::string stable_id;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const TargetBinding&) const noexcept = default;
};

enum class ChannelValueKind : std::uint8_t {
    scalar,
    transform,
};

[[nodiscard]] std::string_view channel_value_kind_name(
    ChannelValueKind kind) noexcept;

struct ScalarKeyframe {
    double time = 0.0;
    double value = 0.0;
};

struct TransformKeyframe {
    double time = 0.0;
    core::Transform value = core::Transform::identity();
};

// Authoring-time event marker. Game dispatch is never implied: explicit
// bindings live in EventBindingTable and unbound markers fail closed with an
// explicit diagnostic.
struct EventMarker {
    EventId id;
    double time = 0.0;
    std::string name;

    [[nodiscard]] core::Result<void> validate() const;
};

// Neutral, versioned, digest-bound record for one time-varying authoring
// channel. Keyframes and markers are stored in canonical order so the
// serialized form is unambiguous. Execution, blending, transition policy, and
// runtime state remain downstream consumer and game runtime authority.
class TemporalChannel final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxKeyframesPerChannel = 65'536U;
    static constexpr std::size_t kMaxEventMarkersPerChannel = 4'096U;
    static constexpr std::size_t kMaxChannelNameBytes = 256U;
    static constexpr std::size_t kMaxStableIdBytes = 256U;
    static constexpr std::size_t kMaxEventNameBytes = 256U;

    ChannelId id;
    std::string channel_name;
    core::Revision revision = core::Revision{1U};
    TargetBinding target;
    ChannelValueKind value_kind = ChannelValueKind::scalar;
    double duration = 0.0;
    std::vector<ScalarKeyframe> scalar_keyframes;
    std::vector<TransformKeyframe> transform_keyframes;
    std::vector<EventMarker> event_markers;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<TemporalChannel> deserialize(
        std::string_view text);
};

} // namespace carto::temporal
