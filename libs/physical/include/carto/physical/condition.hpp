#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/physical/types.hpp>
#include <carto/surface/core.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace carto::physical {

// Game-owned surface wear record: the current condition of a surface domain
// as observed by a game adapter (CONDITION-style wear, damage depth, moisture).
//
// This record is EXPLICITLY NON-AUTHORING. It is runtime/domain state, so it:
//   * never participates in a PhysicalTwinRecord canonical document digest;
//   * is never written by Cartographer authoring tools;
//   * carries a game namespace and an opaque game record identity so the
//     owning adapter can correlate it with its own state.
// It remains serialization-capable so game adapters can persist it beside the
// compiled world, outside the authoring document.
struct SurfaceConditionState {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    // Compile-time fence: static_assert-able marker that this record is
    // game-owned and must stay out of authoring digests.
    static constexpr bool kGameOwned = true;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMaxGameNamespaceBytes = 128U;

    surface::SurfaceDomainId domain;
    // World revision this observation applies to; adapters refuse stale ones.
    core::Revision observed_revision;
    // Owning adapter namespace, e.g. "condition". Never empty.
    std::string game_namespace;
    // Opaque game-side identity; Cartographer never interprets it.
    std::uint64_t game_record = 0U;
    // Normalized wear in [0, 1]; 1 means fully consumed.
    double wear = 0.0;
    // Accumulated damage depth in meters, >= 0.
    double damage_depth = 0.0;
    // Normalized moisture in [0, 1].
    double moisture = 0.0;
    // Accumulated weather exposure in arbitrary game-defined units, >= 0.
    double accumulated_exposure = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SurfaceConditionState> deserialize(
        std::string_view text);
};

namespace detail {
void encode_condition(std::ostream& output, const SurfaceConditionState& state);
core::Result<void> decode_condition(std::istream& input, SurfaceConditionState& state);
} // namespace detail

} // namespace carto::physical
