#pragma once

#include <carto/character/controls.hpp>
#include <carto/character/coordinate.hpp>
#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace carto::character {

// Digest-bound references carry both the stable identity and the exact
// content digest, so a reference can never dangle onto silently changed
// content: any drift is detectable before use.
struct MeshAssetRef {
    std::uint64_t asset_id = 0U;
    assets::Sha256Digest content_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const MeshAssetRef&) const noexcept = default;
    [[nodiscard]] bool operator<(const MeshAssetRef& other) const noexcept {
        return asset_id < other.asset_id;
    }
};

struct RigRef {
    RigId rig;
    assets::Sha256Digest graph_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct JointLimitSetRef {
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ControlGraphRef {
    RigId owner;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// One authored character assembly: stable identity, project provenance, root
// scene object, source meshes, rig and control graph references, locale-
// neutral labels, and the coordinate policy the definition was authored
// against. Evaluated poses, GPU buffers and playback state are derived, not
// source, and are absent here.
struct CharacterDefinition {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxMeshRefs = 256U;
    static constexpr std::size_t kMaxNamespaceBytes = 256U;
    static constexpr std::size_t kMaxLabels = 4'096U;
    static constexpr std::size_t kMaxLabelKeyBytes = 256U;
    static constexpr std::size_t kMaxLabelValueBytes = 1'024U;

    CharacterId id;
    std::string project_namespace;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    scene::ObjectId root;
    // Sorted and unique by asset id.
    std::vector<MeshAssetRef> mesh_refs;
    RigRef rig;
    ControlGraphRef control_graph;
    // Locale-neutral authoring labels keyed by stable token, sorted by the
    // map's key ordering and bounded in count and size.
    std::map<std::string, std::string> locale_neutral_labels;
    assets::Sha256Digest coordinate_policy_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SymmetryPair {
    kinematic::JointId left_joint;
    kinematic::JointId right_joint;

    [[nodiscard]] core::Result<void> validate() const;
};

// Declared pairing of rig controls across the same symmetry: mirroring
// copies one side's control definition onto the other, remapping joint and
// control references through the joint/control mirror maps. Like joint
// symmetry it is declared by the author and never inferred.
struct ControlSymmetryPair {
    ControlId left_control;
    ControlId right_control;

    [[nodiscard]] core::Result<void> validate() const;
};

// One authored rig: digest-bound kinematic graph and joint limit references,
// the rest-basis coordinate policy, semantic tags, the control graph, and a
// symmetry map over stable JointIds. The kinematic hierarchy itself is owned
// by carto::kinematic and referenced by digest.
struct RigDefinition {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSemanticTags = 128U;
    static constexpr std::size_t kMaxSemanticTagBytes = 128U;
    static constexpr std::size_t kMaxSymmetryPairs = 4'096U;

    RigId id;
    assets::Sha256Digest kinematic_graph_digest;
    JointLimitSetRef joint_limits;
    CoordinatePolicy rest_basis;
    // Sorted, unique, bounded semantic tags ("arm.left", "tail").
    std::vector<std::string> semantic_tags;
    ControlGraph control_graph;
    // Sorted by left joint, unique on both sides.
    std::vector<SymmetryPair> symmetry;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
