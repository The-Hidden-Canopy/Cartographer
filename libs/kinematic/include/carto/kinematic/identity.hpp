#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::kinematic {

class KinematicGraph;

// Where one joint was observed against one mesh revision. Bindings are
// per-revision because a topology edit renumbers mesh elements: the stable
// JointId survives, the per-revision anchor and rest transform are re-recorded.
struct MeshRevisionBinding {
    core::Revision mesh_revision;
    // Digest of the mesh content at this revision. Zero digests are invalid:
    // a binding must name the exact content it was observed against.
    assets::Sha256Digest mesh_digest;
    // Representative mesh element (authoring-domain element id) in this
    // revision's numbering. Zero means the joint is not anchored to a surface
    // element for this revision.
    std::uint64_t surface_anchor = 0U;
    // Rest transform observed at this revision.
    core::Transform binding_rest = core::Transform::identity();

    [[nodiscard]] core::Result<void> validate() const;
};

// Durable identity for one authored joint. The stable joint id and stable name
// survive mesh revisions, reimports and topology changes; display names are
// never identity. The binding history is the lineage that proves survival.
struct JointIdentityRecord {
    JointId joint;
    // Authored stable name. Games may add meaning through it but cannot use it
    // to redefine correspondence.
    std::string stable_name;
    std::optional<JointId> parent;
    core::Transform authored_rest = core::Transform::identity();
    // Ascending mesh revision order, unique per revision.
    std::vector<MeshRevisionBinding> bindings;

    [[nodiscard]] core::Result<void> validate() const;
};

// Stable joint identity table for one bound mesh asset. Maps each stable joint
// id to its per-revision bindings so joint identity survives topology change:
// the record is re-anchored per mesh revision without ever renumbering joints.
// Runtime pose state is fenced out by construction; this table names durable
// identity only.
class JointCorrespondence final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxJoints = 65'536U;
    static constexpr std::size_t kMaxBindingsPerJoint = 256U;
    static constexpr std::size_t kMaxStableNameBytes = 256U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;

    std::string source_namespace;
    std::uint64_t mesh_asset = 0U;
    // The mesh revision this correspondence currently reflects.
    core::Revision mesh_revision;
    assets::Sha256Digest mesh_digest;

    [[nodiscard]] core::Result<void> insert(JointIdentityRecord record);
    [[nodiscard]] const JointIdentityRecord* find(JointId joint) const noexcept;
    [[nodiscard]] const std::map<JointId, JointIdentityRecord>& joints()
        const noexcept {
        return joints_;
    }
    // The binding for an exact mesh revision. An unknown joint is not_found;
    // a known joint without a binding at the requested revision is stale_data.
    [[nodiscard]] core::Result<const MeshRevisionBinding*> resolve(
        JointId joint,
        core::Revision mesh_revision) const;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<JointCorrespondence> deserialize(
        std::string_view text);

private:
    std::map<JointId, JointIdentityRecord> joints_;
};

// Evidence that a kinematic reimport preserved stable joint identity while
// leaving runtime/game-owned pose state untouched. The receipt carries no pose,
// no handles and no mutation authority: it classifies every stable joint id as
// preserved, rebound, removed, or added across one mesh revision step.
struct KinematicReimportReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxJoints = 65'536U;

    core::Revision revision_before;
    core::Revision revision_after;
    std::uint64_t mesh_asset = 0U;
    assets::Sha256Digest mesh_digest_before;
    assets::Sha256Digest mesh_digest_after;
    // Sorted, unique, pairwise disjoint.
    std::vector<JointId> preserved_joints;
    std::vector<JointId> rebound_joints;
    std::vector<JointId> removed_joints;
    std::vector<JointId> added_joints;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<KinematicReimportReceipt> deserialize(
        std::string_view text);
};

// The result of one reimport step: the receipt plus the correspondence table
// advanced to the new mesh revision. Joints keep their stable ids throughout.
struct ReimportReconciliation {
    KinematicReimportReceipt receipt;
    JointCorrespondence correspondence;
};

// Three-way reimport merge, neutral and deterministic. Classifies joints from
// the previous correspondence against the current authored graph, records a
// binding for the new mesh revision for every surviving joint, and emits the
// evidence receipt. The new mesh revision must not regress; a no-op reimport
// (same revision and digest) is valid and preserves every joint. Joints newly
// added by the current graph must carry a non-empty semantic type so a stable
// name exists; the call fails closed otherwise. Nothing here reads or writes
// runtime pose state.
[[nodiscard]] core::Result<ReimportReconciliation> reconcile_reimport(
    const JointCorrespondence& before,
    const KinematicGraph& current,
    core::Revision new_mesh_revision,
    const assets::Sha256Digest& new_mesh_digest);

} // namespace carto::kinematic
