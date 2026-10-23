#pragma once

#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace carto::character {

// A source reference pinned to an exact revision and content digest.
// Reimports with altered topology may preserve subsets; anything that cannot
// be transferred against this pinned digest is explicitly invalidated, never
// silently rebound.
struct PinnedSourceRef {
    std::uint64_t source_id = 0U;
    core::Revision pinned_revision;
    assets::Sha256Digest digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// What a seam or guide attaches to: one joint or one control.
struct BindingAttachment {
    kinematic::JointId joint;
    ControlId control;

    [[nodiscard]] bool is_joint() const noexcept { return joint.value != 0U; }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const BindingAttachment&) const noexcept = default;
    [[nodiscard]] bool operator<(const BindingAttachment&) const noexcept;
};

// One authored seam. `attachment_index` addresses the binding's attachment
// table; an index outside that table is an orphan seam and fails validation.
struct SeamRef {
    std::uint64_t seam_id = 0U;
    std::uint32_t attachment_index = 0U;
};

// One authored groom guide, with the same no-orphan rule as seams.
struct GuideRef {
    std::uint64_t guide_id = 0U;
    std::uint32_t attachment_index = 0U;
};

struct GarmentBinding {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxAttachments = 1'024U;
    static constexpr std::size_t kMaxSeams = 65'536U;
    static constexpr std::size_t kMaxCollisionFilters = 256U;
    static constexpr std::uint32_t kMaxLod = 16U;

    GarmentBindingId id;
    PinnedSourceRef source_mesh;
    // Sorted and unique attachment set; never empty for a live binding.
    std::vector<BindingAttachment> attachments;
    std::vector<SeamRef> seams;
    // Collision filter references, sorted and unique by digest.
    std::vector<assets::Sha256Digest> collision_filter_refs;
    assets::Sha256Digest material_intent_ref;
    // LOD index -> product digest, sorted and unique by LOD.
    std::map<std::uint32_t, assets::Sha256Digest> lod_mapping;

    [[nodiscard]] core::Result<void> validate() const;
};

struct GroomBinding {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxAttachments = 1'024U;
    static constexpr std::size_t kMaxGuides = 65'536U;
    static constexpr std::size_t kMaxCollisionFilters = 256U;
    static constexpr std::uint32_t kMaxLod = 16U;

    GroomBindingId id;
    PinnedSourceRef source_guides;
    std::vector<BindingAttachment> attachments;
    std::vector<GuideRef> guides;
    std::vector<assets::Sha256Digest> collision_filter_refs;
    assets::Sha256Digest material_intent_ref;
    std::map<std::uint32_t, assets::Sha256Digest> lod_mapping;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
