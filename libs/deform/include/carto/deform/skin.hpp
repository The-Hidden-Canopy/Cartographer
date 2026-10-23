#pragma once

#include <carto/deform/affine.hpp>
#include <carto/deform/dual_quaternion.hpp>
#include <carto/deform/receipt.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace carto::deform {

enum class SkinMethod : std::uint8_t {
    linear_blend,
    dual_quaternion,
};

[[nodiscard]] std::string_view skin_method_name(SkinMethod method) noexcept;

// Runtime pose input for skinning, deliberately separate from the authored
// KinematicGraph: world rest transforms define the bind pose, world posed
// transforms define the current frame. Both are required for every joint the
// binding references; missing joints fail closed.
struct JointPoseSet {
    std::map<kinematic::JointId, core::Transform> world_rest;
    std::map<kinematic::JointId, core::Transform> world_pose;
};

struct SkinEvaluationInput {
    // Bind-space vertex positions, dense index-aligned with the binding's
    // vertex indexes.
    std::span<const core::Vec3d> bind_positions;
    // Content digest the caller attests for the passed positions. Checked
    // against the binding's mesh digest unless the caller attests the
    // correspondence out of band (DeformationGraph verifies it once at
    // graph entry and then carries stack-owned bind positions whose values
    // prior morph stages have legitimately changed).
    assets::Sha256Digest mesh_content_digest;
    bool mesh_digest_attested = true;
    const kinematic::SkinBinding* binding = nullptr;
    const JointPoseSet* poses = nullptr;
};

struct SkinEvaluationResult {
    // Deformed positions in model space (mesh bind transform applied).
    std::vector<core::Vec3d> positions;
};

// CPU reference linear blend skinning: double precision, up to
// kinematic::SkinBinding::kMaxInfluencesPerVertex influences per vertex,
// general TRS joint transforms (the declared affine subset).
[[nodiscard]] core::Result<SkinEvaluationResult> evaluate_lbs(
    const SkinEvaluationInput& input);

// CPU reference dual-quaternion skinning. Every joint delta must be rigid;
// shear or non-unit scale in a referenced joint's rest or pose transform is
// rejected as unsupported rather than approximated.
[[nodiscard]] core::Result<SkinEvaluationResult> evaluate_dqs(
    const SkinEvaluationInput& input);

// Measured LBS-vs-DQS deviation on one input; the receipt for choosing
// between methods on bending/twisting regions. DQS is not universally better
// (ch11): this reports the evidence, it does not pick a winner.
[[nodiscard]] core::Result<DeviationSummary> compare_skin_methods(
    const SkinEvaluationInput& input);

} // namespace carto::deform
