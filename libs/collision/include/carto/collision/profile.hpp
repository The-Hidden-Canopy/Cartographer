#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/collision/family.hpp>
#include <carto/collision/metrics.hpp>
#include <carto/collision/policy.hpp>
#include <carto/collision/primitive.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::collision {

// Per-game collision profile: the game-owned contract that constrains
// candidate generation and admission for one game identity. It names the
// permitted primitive kinds, the measured-error budget, the preferred
// representation family order, motion defaults, and target capability flags.
// It is a neutral record: no engine types, no runtime handles, no game logic.
class CollisionProfile final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1024U * 1024U;
    static constexpr std::size_t kMaxPermittedPrimitiveKinds = 16U;
    static constexpr std::size_t kMaxFamilyPreference = 8U;
    static constexpr std::size_t kMaxGameIdentityBytes = 128U;

    std::string game_identity;
    std::vector<PrimitiveCollisionKind> permitted_primitive_kinds;
    // Preference order matters and is preserved; families must be unique.
    std::vector<CollisionRepresentationFamily> family_preference;
    CollisionErrorThresholdPolicy error_budget;
    CollisionMotionKind default_motion = CollisionMotionKind::stationary;
    bool convex_proxy_capability = false;
    bool triangle_mesh_capability = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CollisionProfile> deserialize(
        std::string_view text);

    [[nodiscard]] bool permits(PrimitiveCollisionKind kind) const noexcept;
    [[nodiscard]] bool prefers(
        CollisionRepresentationFamily first,
        CollisionRepresentationFamily second) const noexcept;
};

} // namespace carto::collision
