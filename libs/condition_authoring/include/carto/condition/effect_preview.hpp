#pragma once

#include <carto/condition/authoring.hpp>
#include <carto/spatial_field/scalar_field.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::condition {

// Ephemeral, deterministic authoring output. Positions and velocities are in
// the owning asset's local space; this is not a runtime particle pool.
struct EffectParticlePreview {
    Id emitter_id = 0U;
    std::uint32_t ordinal = 0U;
    core::Vec3d position{};
    core::Vec3d velocity{};
    double age_seconds = 0.0;
    double lifetime_seconds = 0.0;
};

struct EffectPreviewFieldReceipt {
    std::string identity;
    core::Revision revision;
    assets::Sha256Digest digest;
};

struct EffectPreviewResult {
    Id effect_source_id = 0U;
    Id asset_id = 0U;
    std::string operating_state;
    core::Revision authoring_revision;
    assets::Sha256Digest authoring_digest;
    std::vector<EffectPreviewFieldReceipt> field_receipts;
    std::vector<EffectParticlePreview> particles;
    assets::Sha256Digest preview_digest;
};

// Evaluates one authored source at a bounded time. Every field binding must
// resolve to the exact project field revision and digest; missing/stale fields
// fail closed. No authoring/project bytes are changed.
[[nodiscard]] core::Result<EffectPreviewResult> preview_effect(
    const AuthoringDocument& authoring,
    const std::map<std::string, spatial_field::ScalarField2D>& fields,
    Id effect_source_id,
    std::string_view operating_state,
    double elapsed_seconds);

} // namespace carto::condition
