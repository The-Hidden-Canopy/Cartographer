#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace carto::collision {

// Hierarchical motion authored once per node identity. This record is
// intentionally decoupled from the kinematic library: identities and runtime
// mappings are loose strings, so the collision policy stays a neutral record
// with no engine or runtime dependency.
enum class CollisionMotionKind : std::uint8_t {
    // C++ reserves `static`; `stationary` is the static-body motion kind.
    stationary,
    dynamic,
    kinematic,
};

[[nodiscard]] std::string_view collision_motion_kind_name(
    CollisionMotionKind kind) noexcept;

struct CollisionMotionBinding {
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxMappingBytes = 256U;

    std::string node_identity;
    CollisionMotionKind motion = CollisionMotionKind::stationary;
    // Kinematic nodes must name a parent identity, carry their rest transform,
    // and declare the runtime mapping string the consumer resolves. Every
    // other motion kind must leave all three unset.
    std::optional<std::string> parent_identity;
    core::Transform rest_transform = core::Transform::identity();
    std::optional<std::string> runtime_mapping;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// Per-node collision motion policy. Node identities are unique; kinematic
// parents must resolve to a node present in the policy. Runtime embodiment,
// current pose, and destruction state are explicitly fenced out.
class CollisionMotionPolicy final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxNodes = 65'536U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    [[nodiscard]] core::Result<void> insert(CollisionMotionBinding binding);
    [[nodiscard]] const CollisionMotionBinding* find(
        std::string_view node_identity) const noexcept;
    [[nodiscard]] const std::map<std::string, CollisionMotionBinding>& nodes()
        const noexcept {
        return nodes_;
    }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CollisionMotionPolicy> deserialize(
        std::string_view text);

private:
    std::map<std::string, CollisionMotionBinding> nodes_;
};

} // namespace carto::collision
