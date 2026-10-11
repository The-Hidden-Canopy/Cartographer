#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/navgraph/access.hpp>
#include <carto/navgraph/math_compare.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace carto::navgraph {

// Neutral connection-interface reference owned by carto::navgraph. The sibling
// carto::connection port authority may eventually become the canonical source
// of these records; until that boundary lands, link compilation consumes this
// minimal local shape so navgraph stays dependency-free. It carries a stable
// port identity, the identity of the structure that owns the port, a local
// frame, and sorted, unique compatibility tags. Connection state (open,
// locked, powered) is runtime/game state and is intentionally absent.
struct PortReference {
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::size_t kMaxOwnerBytes = 256U;
    static constexpr std::size_t kMaxTags = 32U;
    static constexpr std::size_t kMaxTagBytes = 128U;

    std::string port_identity;
    std::string owner_identity;
    core::Transform local_frame = core::Transform::identity();
    std::vector<std::string> tags;

    // Neutral traversal primitive declared by the port's tags, if any. Tags
    // are sorted, so the first recognized primitive name is deterministic.
    [[nodiscard]] std::optional<AccessPrimitive> traversal_primitive() const noexcept;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const PortReference& other) const noexcept {
        return port_identity == other.port_identity && owner_identity == other.owner_identity &&
            vec3_equal(local_frame.translation, other.local_frame.translation) &&
            local_frame.rotation.x == other.local_frame.rotation.x &&
            local_frame.rotation.y == other.local_frame.rotation.y &&
            local_frame.rotation.z == other.local_frame.rotation.z &&
            local_frame.rotation.w == other.local_frame.rotation.w &&
            vec3_equal(local_frame.scale, other.local_frame.scale) && tags == other.tags;
    }
};

} // namespace carto::navgraph
