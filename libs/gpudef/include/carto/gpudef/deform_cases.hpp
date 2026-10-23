#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/drivers.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/deform/corrective.hpp>
#include <carto/gpudef/emulation.hpp>
#include <carto/gpudef/evidence.hpp>
#include <carto/gpudef/layout.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::gpudef {

// Per-deformation-case parity evidence (spec ch17 "GPU deformation and
// performance"). The packed stream in layout.hpp is a skinning stream, so
// only the linear-blend case deforms it directly; the other cases bind the
// packed layout's decoded position/influence path against the same case
// math evaluated over the source view, and the corrective case additionally
// binds the driver policy through its canonical channel encoding. Every
// case declares an admission envelope up front — exact-match when the pack
// receipt measures zero quantization error, otherwise an analytic
// bounded-error envelope derived from the declared quantization bounds
// propagated through the case's math — and a run whose measured error
// violates its declared envelope fails closed instead of minting a
// receipt that pretends otherwise.

enum class DeformationCase : std::uint8_t {
    linear_blend_skinning,
    dual_quaternion_skinning,
    morph_stage,
    corrective_driver,
};

[[nodiscard]] std::string_view deformation_case_name(
    DeformationCase deformation_case) noexcept;

// One caller-supplied morph displacement binding the morph-stage case
// applies on top of the (possibly quantized) packed positions.
struct MorphDeltaBinding {
    std::uint32_t vertex_index = 0U;
    core::Vec3d delta{};
};

// Declared admission envelope for one {case, layout, mode} evaluation.
// exact_match is declared (never inferred from the measurement): it holds
// exactly when the pack receipt measured zero quantization error, which is
// the documented fp32/fp16-representable contract of the stream.
struct CaseEnvelope {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    DeformationCase deformation_case = DeformationCase::linear_blend_skinning;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    QuantizationMode mode = QuantizationMode::fp32;
    bool exact_match = false;
    // Worst-case bound on the Euclidean position error between the packed
    // lane and the source-view lane, propagated analytically from the mode's
    // declared quantization bounds through the case's math.
    double declared_position_error_bound = 0.0;
    // The mode's declared per-component weight bound, recorded for audit.
    double declared_weight_error_bound = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CaseEnvelope> deserialize(
        std::string_view text);
};

// Data-dependent propagation terms measured from the actual evaluation, so
// the declared bound is computed from the workload and never hand-waved.
struct EnvelopeContext {
    // Largest 3x3 row-sum (infinity) norm over the referenced skin
    // matrices; unused by cases without a skinning step.
    double max_skin_scale = 1.0;
    // Largest absolute position component over the source view; matches the
    // magnitude basis of QuantizationErrorReceipt's declared position bound.
    double max_source_magnitude = 0.0;
    // Corrective case: the evaluated driver activation applied to the
    // position path.
    double driver_weight = 1.0;
};

// Declares the admission envelope for one case evaluation. Fails closed on
// an invalid pack receipt or non-finite context. The bound is conservative:
// lbs/dqs propagate the position bound through the skin matrices and the
// weight bound through the influence count; morph binds the position path
// only; corrective scales the position bound by the evaluated activation.
[[nodiscard]] core::Result<CaseEnvelope> declare_case_envelope(
    DeformationCase deformation_case,
    InfluenceLayout layout,
    QuantizationMode mode,
    const QuantizationErrorReceipt& pack_error,
    const EnvelopeContext& context);

struct CaseParityInput {
    DeformationCase deformation_case =
        DeformationCase::linear_blend_skinning;
    SkinInputView skin;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    QuantizationMode mode = QuantizationMode::fp32;
    // lbs/dqs: skin_matrices[joint.value] in the Mat34 convention.
    std::span<const Mat34> skin_matrices{};
    // morph_stage: displacements applied on top of the position path with
    // morph_weight in [0, 1].
    std::span<const MorphDeltaBinding> morph_deltas{};
    double morph_weight = 0.0;
    // corrective_driver: the driver policy and its posed channels, evaluated
    // through the canonical channel encoding (layout lane) and directly
    // (source lane).
    const character::CorrectiveDriver* corrective_driver = nullptr;
    const deform::DriverChannelValues* corrective_channels = nullptr;
    // Same honest tier rules as ParityInput: a device profile is only valid
    // with reference_is_device_measured, and claiming device measurement
    // without a profile is refused outright.
    std::optional<DeviceProfile> device_profile;
    bool reference_is_device_measured = false;
};

// Measured parity receipt for one deformation case. The digest of the exact
// packed stream bytes is embedded, so the receipt is bound to the layout
// payload it was measured over; the declared envelope and its verdict are
// part of the canonical form.
struct CaseParityReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    DeformationCase deformation_case =
        DeformationCase::linear_blend_skinning;
    QuantizationMode mode = QuantizationMode::fp32;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    std::uint32_t vertex_count = 0U;
    std::uint32_t joint_count = 0U;
    double max_position_error = 0.0;
    double bounding_diagonal = 0.0;
    double declared_position_error_bound = 0.0;
    bool exact_match_declared = false;
    // Verdict of the declared envelope against the measured error. A
    // receipt only validates when this is true: envelope rejection is a
    // fail-closed state, never a mintable artifact.
    bool envelope_admitted = false;
    assets::Sha256Digest packed_buffer_digest{};
    std::optional<DeviceProfile> device_profile;
    EvidenceTier tier = EvidenceTier::cpu_reference;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<CaseParityReceipt> deserialize(
        std::string_view text);
};

// Pure admission verdicts. admitted_by_envelope fails closed (false) on
// any case/layout/mode mismatch between envelope and receipt.
[[nodiscard]] bool admitted_by_envelope(
    const CaseParityReceipt& receipt) noexcept;
[[nodiscard]] bool admitted_by_envelope(
    const CaseEnvelope& envelope,
    const CaseParityReceipt& receipt) noexcept;

// Packed-lane emulation for one deformation case: pack the source view,
// decode the packed stream, and evaluate the case kernel over the decoded
// data. Shared by the parity harness and the throughput capture. Fails
// closed on invalid inputs, non-rigid skin matrices for the
// dual-quaternion case, unposed driver channels, and unbounded references.
[[nodiscard]] core::Result<std::vector<core::Vec3d>> run_case_emulation(
    const CaseParityInput& input);

// Source-lane reference for the same case kernel over the caller's source
// view. In fp32 mode with representable data the two lanes are bit-identical
// by construction; the parity receipt measures the actual difference.
[[nodiscard]] core::Result<std::vector<core::Vec3d>> run_case_reference(
    const CaseParityInput& input);

// Runs the per-case parity evaluation: declared envelope -> packed lane vs
// source lane -> measured error -> admission verdict. Fails closed with an
// explicit diagnostic when the workload is invalid for the case or the
// measured error violates the declared envelope.
[[nodiscard]] core::Result<CaseParityReceipt> run_case_parity(
    const CaseParityInput& input);

[[nodiscard]] EvidenceTier effective_tier(
    const CaseParityReceipt& receipt) noexcept;

[[nodiscard]] core::Result<void> require_evidence_tier(
    const CaseParityReceipt& receipt,
    EvidenceTier requested);

} // namespace carto::gpudef
