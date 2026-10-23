#pragma once

#include <carto/deform/corrective.hpp>
#include <carto/deform/morph.hpp>
#include <carto/deform/receipt.hpp>
#include <carto/deform/skin.hpp>

#include <carto/character/drivers.hpp>
#include <carto/character/ids.hpp>
#include <carto/character/morph.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace carto::deform {

// Declared evaluation space of a morph stage. Bind-space deltas may only run
// before the skin stage; post-skin deltas are applied to skinned model-space
// positions. The author declares the space explicitly; mixing spaces without
// a declaration is a validation failure, never an implicit conversion.
enum class MorphSpace : std::uint8_t {
    bind_pre_skin,
    post_skin_model,
};

[[nodiscard]] std::string_view morph_space_name(MorphSpace space) noexcept;

struct DeformationStage {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    enum class Kind : std::uint8_t {
        skin,
        morph,
    };

    Kind kind = Kind::skin;
    // Kind::skin fields.
    SkinMethod method = SkinMethod::linear_blend;
    // Kind::morph fields.
    MorphSpace space = MorphSpace::bind_pre_skin;
    character::MorphTargetId morph_target;
    double morph_weight = 0.0; // finite, within [0, 1]
    // Optional corrective driver modulating this stage's weight: the applied
    // weight is morph_weight * driver_activation.
    std::optional<character::CorrectiveDriverId> corrective_driver;

    [[nodiscard]] core::Result<void> validate() const;
};

// Ordered, source-owned deformation stack evaluated from the canonical bind
// mesh: exactly one skin stage, morph stages in declared space order, at most
// kMaxStages entries. The vector order is the canonical execution order.
struct DeformationGraph {
    static constexpr std::size_t kMaxStages = 64U;

    std::vector<DeformationStage> stages;

    [[nodiscard]] core::Result<void> validate() const;
};

// Decoded-side source for one morph stage: the immutable record plus the
// content-addressed delta blob bytes and the caller-attested topology
// correspondence digest.
struct MorphSource {
    const character::MorphTarget* target = nullptr;
    std::span<const std::uint8_t> delta_blob;
    assets::Sha256Digest topology_correspondence_digest;
};

struct DeformationEvaluationInput {
    // Canonical bind-space vertex positions, dense and index-aligned with the
    // skin binding and every morph target's correspondence layout.
    std::span<const core::Vec3d> bind_positions;
    assets::Sha256Digest mesh_content_digest;
    const kinematic::SkinBinding* binding = nullptr;
    const JointPoseSet* poses = nullptr;
    std::span<const character::CorrectiveDriver* const> corrective_drivers;
    DriverChannelValues driver_channels;
    std::map<character::MorphTargetId, MorphSource> morphs;
    // Optional reference evaluation for the deviation receipt; empty means no
    // deviation is claimed.
    std::span<const core::Vec3d> reference_positions;
};

struct DeformationEvaluationResult {
    std::vector<core::Vec3d> positions;
    DeformationReceipt receipt;
};

// Deterministic CPU reference evaluation of one deformation graph. Every
// stage is digest-bound and fail-closed: a stale binding, morph record,
// correspondence digest or missing driver refuses the whole evaluation.
[[nodiscard]] core::Result<DeformationEvaluationResult> evaluate(
    const DeformationGraph& graph,
    const DeformationEvaluationInput& input);

} // namespace carto::deform
