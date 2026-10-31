#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/sweep.hpp>

#include <cstdint>

namespace carto::desktop {

// New sources may opt into an owner-local frame only while the owner appears
// in the same application snapshot used to dispatch the human action.
[[nodiscard]] inline core::Result<void> bind_new_sweep_source_owner(
    geometry::SweepSource& candidate,
    std::uint64_t owner_object_id,
    bool source_identity_is_new,
    bool owner_is_current,
    std::uint64_t owner_project_generation,
    std::uint64_t current_project_generation) {
    if (!source_identity_is_new || candidate.owner_object_id != 0U ||
        (owner_object_id != 0U && (!owner_is_current ||
            owner_project_generation != current_project_generation))) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "a sweep owner can only be selected for a new source and current scene object"));
    }
    candidate.owner_object_id = owner_object_id;
    return core::Result<void>::success();
}

// A radial-editor update starts from a newly assembled candidate source. Keep
// its optional owner binding only when the exact existing source was loaded;
// new source bindings go through bind_new_sweep_source_owner instead.
[[nodiscard]] inline core::Result<void> preserve_exact_sweep_source_owner(
    geometry::SweepSource& candidate,
    const geometry::SweepSource& existing,
    bool exact_loaded_source) {
    if (!exact_loaded_source ||
        candidate.source_identity != existing.source_identity ||
        candidate.source_revision <= existing.source_revision) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep owner can only be retained from the exact loaded source revision"));
    }
    candidate.owner_object_id = existing.owner_object_id;
    return core::Result<void>::success();
}

} // namespace carto::desktop
