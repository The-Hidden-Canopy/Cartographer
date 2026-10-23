#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/kinematic/skin.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace carto::kinematic {

class KinematicGraph;

// Neutral lowering vocabulary. These descriptors are serialization-ready
// records for the private downstream consumer lowering stage: they name which authored
// content becomes which native product class and bind every product to stable
// source digests. They contain no engine types, no runtime handles and no
// mutation authority. Instance identity for repeated assemblies is preserved
// by the scene/instance-field products, not expanded here.

// Target capability vocabulary. A lowering proposal fails closed when a
// required capability is unsupported by the target profile instead of
// silently substituting a default.
enum class KinematicCapability : std::uint8_t {
    skeletal_deformation,
    rigid_articulation,
    angular_limits,
    linear_limits,
};

enum class KinematicCapabilityDisposition : std::uint8_t {
    supported,
    unsupported,
};

struct KinematicCapabilityCheck {
    KinematicCapability feature = KinematicCapability::skeletal_deformation;
    bool required = false;
    KinematicCapabilityDisposition disposition =
        KinematicCapabilityDisposition::supported;
    std::string detail;

    [[nodiscard]] core::Result<void> validate() const;
};

// Fail-closed admission: any required capability marked unsupported fails with
// ErrorCode::unsupported. Optional unsupported capabilities are reported, not
// fatal.
[[nodiscard]] core::Result<void> require_capabilities(
    std::span<const KinematicCapabilityCheck> checks);

// One joint in a deforming subgraph, destined for a downstream consumer skeletal asset. The
// stable joint id is the correspondence anchor back to authoring truth.
struct SkeletalJointDescriptor {
    JointId joint;
    // Parent within the descriptor; unset for the descriptor root. Ancestors
    // of skinned joints are always included so the chain to the root is whole.
    std::optional<JointId> parent;
    core::Transform rest = core::Transform::identity();
    JointKind kind = JointKind::fixed;
    // True when at least one mesh vertex weights to this joint.
    bool has_skin = false;
};

// A deforming subgraph lowered to one skeletal-asset product. Every digest is
// a stable source binding: the graph, the skin, and the authored limits.
struct SkeletalAssetDescriptor {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxJoints = 65'536U;

    std::uint64_t mesh_asset = 0U;
    assets::Sha256Digest graph_digest;
    assets::Sha256Digest skin_digest;
    // Digest of the JointLimitSet applied to this asset; zero when the target
    // profile admits unlimited travel.
    assets::Sha256Digest limit_digest;
    std::map<JointId, SkeletalJointDescriptor> joints;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<SkeletalAssetDescriptor> deserialize(
        std::string_view text);
};

// One rigid articulated node, destined for the downstream consumer archetype/entity
// hierarchy. Rigid subgraphs lower to archetype nodes instead of a skeleton.
struct ArchetypeNodeDescriptor {
    JointId joint;
    // Traces the node to its authored scene object identity. Zero when the
    // caller has not bound it to a scene object yet.
    scene::ObjectId source_object;
    std::optional<JointId> parent;
    core::Transform rest = core::Transform::identity();
    JointKind kind = JointKind::fixed;
    // Optional dedicated rigid mesh per node; zero when the archetype carries
    // no per-node mesh.
    std::uint64_t mesh_asset = 0U;
};

// A rigid subgraph lowered to one reusable archetype. The archetype identity
// is derived from the structural subgraph digest so identical mechanisms share
// one archetype instead of being re-expanded per instance.
struct ArchetypeDescriptor {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxNodes = 65'536U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;

    assets::Sha256Digest graph_digest;
    std::string archetype_identity;
    std::map<JointId, ArchetypeNodeDescriptor> nodes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ArchetypeDescriptor> deserialize(
        std::string_view text);
};

// Builds the skeletal-asset descriptor for the deforming part of a validated
// graph: every skinned node plus its ancestors to the root. Fails with
// stale_data when the skin binding was authored against a different graph
// revision, and fails closed when the graph has no deforming subgraph.
[[nodiscard]] core::Result<SkeletalAssetDescriptor> describe_deforming_subgraph(
    std::uint64_t mesh_asset,
    const KinematicGraph& graph,
    const SkinBinding& skin,
    const JointLimitSet& limits);

// Builds the archetype descriptor for the rigid complement of a validated
// graph. Fails closed when every node deforms.
[[nodiscard]] core::Result<ArchetypeDescriptor> describe_rigid_subgraph(
    const KinematicGraph& graph);

} // namespace carto::kinematic
