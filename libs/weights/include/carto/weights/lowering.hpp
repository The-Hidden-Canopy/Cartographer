#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::weights {

// Eight-to-four influence lowering for a VANTA four-influence runtime profile
// (Chapter 08 "Eight to four influences"). Every vertex with more than four
// influences is reduced by an exhaustive search over the bounded set of
// four-subsets, choosing the subset whose renormalized deformation stays
// closest to the full eight-influence deformation across representative
// evaluation poses. When the evaluated maximum vertex displacement exceeds
// the asset's declared bound, publication is refused with a named
// INFLUENCE_LOSS_EXCEEDS_BUDGET diagnostic; truncation is never silent.
// cpu_reference evidence only: no GPU profile claim is made here.

// One representative evaluation pose: joint id -> posed transform, applied
// about the joint's rest anchor with the shared deformation rule documented in
// weights_internal.hpp.
struct LoweringPose {
    static constexpr std::size_t kMaxJoints = 65'536U;

    std::map<kinematic::JointId, core::Transform> joint_transforms;

    [[nodiscard]] core::Result<void> validate() const;
};

struct VertexLoweringError {
    std::uint32_t vertex = 0U;
    // Maximum displacement of the lowered deformation from the full
    // deformation at this vertex across all evaluation poses.
    double max_displacement = 0.0;
    // Root-mean-square of that displacement across evaluation poses.
    double rms_displacement = 0.0;
};

struct LowerEightToFourReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxRecordedVertices = 1'048'576U;

    assets::Sha256Digest input_binding_digest;
    assets::Sha256Digest output_binding_digest;
    std::uint32_t max_influences = 4U;
    // The asset's declared quality envelope.
    double error_bound = 0.0;
    // Evaluated worst-case and aggregate error actually measured.
    double evaluated_max_error = 0.0;
    double evaluated_rms_error = 0.0;
    std::uint64_t evaluation_pose_count = 0U;
    // Sorted by vertex index; bounded by kMaxRecordedVertices, oldest-first
    // when the population exceeds the bound.
    std::vector<VertexLoweringError> per_vertex;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<LowerEightToFourReceipt> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct LowerEightToFourOutput {
    kinematic::SkinBinding binding;
    LowerEightToFourReceipt receipt;
};

// Lowers `binding` to at most four influences per vertex. `positions` are the
// canonical rest positions of the bound mesh vertices (array index ==
// binding influence key). `graph` supplies joint rest anchors. At least one
// evaluation pose is required: without representative poses the influence
// loss cannot be measured and the operation refuses rather than truncating
// silently. Fails closed (returning the named diagnostic) when the evaluated
// maximum displacement exceeds `error_bound`.
[[nodiscard]] core::Result<LowerEightToFourOutput> lower_eight_to_four(
    const kinematic::SkinBinding& binding,
    const kinematic::KinematicGraph& graph,
    std::span<const core::Vec3d> positions,
    double error_bound,
    std::span<const LoweringPose> evaluation_poses);

// Reconciliation for the reverse direction (runtime four-influence asset
// reimported against an eight-influence source): verifies that `lowered` is
// exactly what lowering `full` would produce, by checking digest identity and
// that every lowered influence survives in `full` within `weight_tolerance`.
// This is validation only; no weights are synthesized.
[[nodiscard]] core::Result<void> validate_upcast_reconciliation(
    const kinematic::SkinBinding& lowered,
    const kinematic::SkinBinding& full,
    double weight_tolerance);

} // namespace carto::weights
