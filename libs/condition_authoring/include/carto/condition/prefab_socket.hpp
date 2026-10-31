#pragma once

#include <carto/condition/authoring.hpp>
#include <carto/connection/graph.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::condition {

// Read-only resolution of one named prefab socket through the existing typed
// connection-port contract. The returned frame is local to the prefab root;
// callers compose it with the live instance root only after checking current
// project/source bindings. This receipt does not connect ports or mutate state.
struct PrefabSocketResolution final {
    static constexpr std::size_t kMaxFrameDepth = 64U;

    Id prefab_id = 0U;
    Id instance_id = 0U;
    Id member_asset_id = 0U;
    std::string socket_name;
    std::uint64_t source_object_id = 0U;
    std::uint64_t instance_object_id = 0U;
    std::uint64_t source_port_id = 0U;
    assets::Sha256Digest source_port_digest;
    // Ordered from the named socket port through its parent frames to the
    // object-local root, so the composed frame is bound to every source record.
    std::vector<std::uint64_t> frame_chain_port_ids;
    std::vector<assets::Sha256Digest> frame_chain_port_digests;
    core::Transform local_to_prefab_root = core::Transform::identity();

    [[nodiscard]] core::Result<void> validate() const;
};

// Resolves `socket_name` only when it is declared by `prefab`, exactly one
// source member exposes a valid port frame with that identity, and the graph's
// parent-frame chain is unambiguous and acyclic. Legacy placements without a
// recorded isolated snapshot binding are not resolvable. The graph is treated
// as immutable caller-supplied source evidence.
[[nodiscard]] core::Result<PrefabSocketResolution> resolve_prefab_socket(
    const PrefabRecord& prefab,
    const PrefabInstanceRecord& instance,
    const connection::ConnectionPortGraph& ports,
    std::string_view socket_name);

} // namespace carto::condition
