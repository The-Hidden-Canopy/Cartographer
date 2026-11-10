#pragma once

#include <carto/core/revision.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace carto::studio {

struct MaterialGraphDraftIdentity {
    std::uint64_t project_generation = 0U;
    core::Revision project_revision{};
    std::string graph;
    core::Revision source_revision{};

    bool operator==(const MaterialGraphDraftIdentity&) const = default;
};

enum class MaterialGraphDraftState { reload, current, stale };

struct PendingMaterialGraphSelection {
    std::uint64_t project_generation = 0U;
    core::Revision previous_project_revision{};
    std::string graph;
    core::Revision source_revision{};
};

enum class PendingMaterialGraphState { waiting, confirmed, unavailable };

[[nodiscard]] inline PendingMaterialGraphState pending_material_graph_state(
    const PendingMaterialGraphSelection& pending, std::uint64_t project_generation,
    core::Revision project_revision, std::optional<core::Revision> source_revision) noexcept {
    if (project_generation != pending.project_generation) return PendingMaterialGraphState::unavailable;
    if (project_revision > pending.previous_project_revision && source_revision &&
        *source_revision == pending.source_revision)
        return PendingMaterialGraphState::confirmed;
    if (!source_revision && project_revision == pending.previous_project_revision)
        return PendingMaterialGraphState::waiting;
    return PendingMaterialGraphState::unavailable;
}

[[nodiscard]] inline MaterialGraphDraftState material_graph_draft_state(
    bool loaded, bool dirty, const MaterialGraphDraftIdentity& draft,
    const MaterialGraphDraftIdentity& source) noexcept {
    if (!loaded) return dirty ? MaterialGraphDraftState::stale : MaterialGraphDraftState::reload;
    if (draft == source) return MaterialGraphDraftState::current;
    return dirty ? MaterialGraphDraftState::stale : MaterialGraphDraftState::reload;
}

} // namespace carto::studio
