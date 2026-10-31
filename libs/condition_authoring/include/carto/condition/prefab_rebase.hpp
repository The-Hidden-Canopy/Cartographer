#pragma once

#include <carto/condition/authoring.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::condition {

enum class PrefabRebaseChangeKind : std::uint8_t {
    member_added = 0U,
    member_removed = 1U,
    source_binding_changed = 2U,
    source_evidence_stale = 3U,
};

struct PrefabRebaseChange final {
    PrefabRebaseChangeKind kind = PrefabRebaseChangeKind::member_added;
    Id asset_id = 0U;
    std::uint64_t instance_object_id = 0U;
    std::uint64_t previous_source_object_id = 0U;
    std::uint64_t current_source_object_id = 0U;
    std::string previous_source_revision;
    std::string current_source_revision;
    assets::Sha256Digest previous_source_digest;
    assets::Sha256Digest current_source_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const PrefabRebaseChange&) const noexcept = default;
};

// Read-only authoring-level reconciliation. It enumerates membership and
// source-evidence changes but has no project-mesh authority and cannot rebase.
struct PrefabRebaseReview final {
    Id instance_id = 0U;
    Id prefab_id = 0U;
    core::Revision captured_authoring_revision;
    assets::Sha256Digest captured_authoring_digest;
    core::Revision instance_prefab_revision;
    core::Revision current_prefab_revision;
    std::vector<PrefabRebaseChange> changes;
    bool project_mesh_review_required = true;

    [[nodiscard]] bool has_changes() const noexcept;
    [[nodiscard]] core::Result<void> validate(
        const AuthoringDocument& authoring) const;
    [[nodiscard]] static core::Result<PrefabRebaseReview> between(
        const AuthoringDocument& authoring,
        Id instance_id);
    // Validates and digests the authoring snapshot once for all instance rows.
    [[nodiscard]] static core::Result<std::vector<PrefabRebaseReview>> all(
        const AuthoringDocument& authoring);
    [[nodiscard]] bool operator==(const PrefabRebaseReview&) const noexcept = default;
};

} // namespace carto::condition
