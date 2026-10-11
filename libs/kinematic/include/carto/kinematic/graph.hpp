#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace carto::kinematic {

template <typename Tag>
struct Id {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0U; }
    [[nodiscard]] constexpr auto operator<=>(const Id&) const noexcept = default;
};

struct JointTag;
using JointId = Id<JointTag>;

enum class JointKind : std::uint8_t {
    fixed,
    hinge,
    slider,
    ball,
};

enum class NodeClass : std::uint8_t {
    rigid,
    skinned,
};

[[nodiscard]] std::string_view joint_kind_name(JointKind kind) noexcept;
[[nodiscard]] std::string_view node_class_name(NodeClass node_class) noexcept;

// One moving frame in the authored kinematic hierarchy. The node carries only
// durable authoring truth: stable identity, parentage, rest transform, joint
// kind, and deformation class. Current pose, velocity and every other motion
// state are runtime/game-owned and are deliberately absent.
struct KinematicNode {
    JointId id;
    std::optional<JointId> parent;
    core::Transform rest = core::Transform::identity();
    JointKind kind = JointKind::fixed;
    NodeClass node_class = NodeClass::rigid;
    // Optional domain meaning supplied by the game adapter ("door.hinge",
    // "shoulder"). It adds meaning but never redefines correspondence.
    std::string semantic_type;

    [[nodiscard]] core::Result<void> validate() const;
};

// Provider-neutral authored kinematic hierarchy. A valid graph is a single
// rooted tree: exactly one root, every parent resolves, no cycles, finite rest
// transforms, and bounded node count. The graph contains no runtime handles,
// no engine types and no mutation authority over posed state.
class KinematicGraph final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxNodes = 65'536U;
    static constexpr std::size_t kMaxSemanticBytes = 128U;

    [[nodiscard]] core::Result<void> insert_node(KinematicNode node);

    [[nodiscard]] const std::map<JointId, KinematicNode>& nodes() const noexcept {
        return nodes_;
    }
    [[nodiscard]] const KinematicNode* find(JointId id) const noexcept;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<KinematicGraph> deserialize(std::string_view text);

private:
    std::map<JointId, KinematicNode> nodes_;
};

} // namespace carto::kinematic
