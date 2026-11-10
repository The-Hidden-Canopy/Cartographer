#pragma once

#include <carto/core/revision.hpp>
#include <carto/render/material.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace carto::studio {

struct MaterialDraftIdentity {
    std::uint64_t project_generation = 0U;
    render::MaterialId material = 0U;
    core::Revision revision{};

    bool operator==(const MaterialDraftIdentity&) const = default;
};

enum class MaterialDraftState { reload, current, stale };

// Use the complete identity and project generation for UI widget scopes.
[[nodiscard]] inline std::string material_source_widget_id(
    std::uint64_t generation, std::uint64_t source_id) {
    return std::to_string(generation) + ":" + std::to_string(source_id);
}

// Selecting a replacement source changes the asset, not its authored mapping.
// Clearing the source deliberately restores the default unbound reference.
inline void set_base_color_texture_source(
    render::TextureRef& binding, assets::TextureAssetId asset) noexcept {
    if (asset == 0U) binding = {};
    else binding.asset = asset;
}

template <typename Materials>
[[nodiscard]] inline auto find_material_source(const Materials& materials, render::MaterialId id) {
    return std::find_if(materials.begin(), materials.end(),
        [id](const auto& value) { return value.id == id; });
}

// A dirty draft must not be silently rebased onto another source or project.
[[nodiscard]] inline MaterialDraftState material_draft_state(
    bool loaded, bool dirty, MaterialDraftIdentity draft, MaterialDraftIdentity source) noexcept {
    if (!loaded) return dirty ? MaterialDraftState::stale : MaterialDraftState::reload;
    if (draft == source) return MaterialDraftState::current;
    return dirty ? MaterialDraftState::stale : MaterialDraftState::reload;
}

} // namespace carto::studio
