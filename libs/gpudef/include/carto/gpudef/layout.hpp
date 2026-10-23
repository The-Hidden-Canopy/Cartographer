#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/character/weights_record.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::gpudef {

// Packed deformation-stream contracts (spec ch17 "GPU deformation and
// performance"). Everything in this library is CPU-tier evidence only:
// packing, a packed-buffer CPU emulation kernel, a parity harness against a
// caller-provided CPU reference, and a performance-evidence schema ready for
// named-device runs. Nothing here claims native_device or integrated_product
// without attached device evidence, and this library can never mint
// integrated-product evidence on its own.

// Number of influence slots stored per vertex. influences_8 is the canonical
// fidelity path; influences_4 is the reduced path and requires an explicit
// top-four reduction of richer source weights (see reduce_to_top_four).
enum class InfluenceLayout : std::uint8_t {
    influences_8,
    influences_4,
};

// Component storage mode for positions and weights inside the packed stream.
enum class QuantizationMode : std::uint8_t {
    fp32,
    fp16,
    unorm16,
};

[[nodiscard]] std::string_view influence_layout_name(InfluenceLayout layout) noexcept;
[[nodiscard]] std::string_view quantization_mode_name(QuantizationMode mode) noexcept;

[[nodiscard]] constexpr std::uint32_t influences_per_vertex(
    InfluenceLayout layout) noexcept {
    return layout == InfluenceLayout::influences_8 ? 8U : 4U;
}

[[nodiscard]] constexpr std::size_t component_bytes(QuantizationMode mode) noexcept {
    // fp32 stores 4 bytes; fp16 and unorm16 store 2 bytes per component.
    return mode == QuantizationMode::fp32 ? 4U : 2U;
}

// Interleaved vertex stride: 3 position components, then per influence one
// uint16 joint index plus one weight component. Joint indices are always
// uint16 regardless of the quantization mode.
[[nodiscard]] constexpr std::size_t vertex_stride_bytes(
    InfluenceLayout layout,
    QuantizationMode mode) noexcept {
    const std::size_t position = 3U * component_bytes(mode);
    const std::size_t influence = 2U + component_bytes(mode);
    return position + influence * influences_per_vertex(layout);
}

// Axis-aligned bounds that unorm16 position storage is quantized into.
// Mandatory in SkinInputView whenever the mode is unorm16; absent otherwise.
struct PositionQuantizationBounds {
    core::Vec3d minimum{};
    core::Vec3d maximum{};

    [[nodiscard]] core::Result<void> validate() const;
};

// Provider-neutral view of the skin inputs feeding a packed deformation
// buffer. Positions are bind-space mesh vertices; per-vertex influence lists
// must satisfy the shared admission contract (character::validate_influences):
// 1..8 influences, strictly positive finite weights, unique sorted joints,
// weight sum within tolerance of exactly one. A vertex with zero influences
// is rigid and is packed with an all-zero influence tail.
struct SkinInputView {
    std::span<const core::Vec3d> positions;
    // Per-vertex admitted influence count, each in 0..8. Sum must equal
    // influences.size(); sizes must match positions.size().
    std::span<const std::uint32_t> influence_counts;
    // Flattened influence payload in vertex order, each list sorted by joint
    // id and unique.
    std::span<const kinematic::JointInfluence> influences;
    // Number of distinct admissible joints; joint indices are packed as
    // uint16 so this is bounded by PackedVertexBuffer::kMaxJoints.
    std::uint32_t joint_count = 0U;
    // Mandatory when the target mode is unorm16.
    std::optional<PositionQuantizationBounds> position_bounds;
};

// Per-mode quantization evidence for one packing operation. Errors are
// measured deterministically during pack; the declared bounds are the
// documented analytic contract for the mode:
// - fp32 positions: correctly-rounded float storage; measured error is the
//   round-trip error of casting to float and back.
// - fp16: IEEE-754 binary16 round-to-nearest-even; weight error is bounded
//   by half an ulp of 1.0 (2^-11) and position error by half an ulp of the
//   largest stored-magnitude binary16 value.
// - unorm16: weights scaled by 65535 and rounded to nearest; error bounded
//   by 0.5/65535. Positions are quantized into PositionQuantizationBounds;
//   error is bounded by half the per-axis quantum (max-min)/65535.
struct QuantizationErrorReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxDigestContextBytes = 64U;

    QuantizationMode mode = QuantizationMode::fp32;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    std::uint32_t vertex_count = 0U;
    double max_position_component_error = 0.0;
    double max_weight_error = 0.0;
    double declared_position_error_bound = 0.0;
    double declared_weight_error_bound = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// One packed deformation stream: an interleaved, little-endian vertex buffer
// plus the header needed to bind it to its source (layout, mode, counts,
// unorm bounds). The digest covers the header and the exact payload bytes.
class PackedVertexBuffer {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::uint32_t kMaxVertices = 262'144U;
    static constexpr std::uint32_t kMaxJoints = 65'536U;
    static constexpr std::size_t kMaxSerializedBytes = 96U * 1024U * 1024U;

    InfluenceLayout layout = InfluenceLayout::influences_8;
    QuantizationMode mode = QuantizationMode::fp32;
    std::uint32_t vertex_count = 0U;
    std::uint32_t joint_count = 0U;
    std::optional<PositionQuantizationBounds> position_bounds;
    // Exactly vertex_count * stride_bytes() bytes, little-endian.
    std::vector<std::uint8_t> payload;

    [[nodiscard]] std::size_t stride_bytes() const noexcept {
        return vertex_stride_bytes(layout, mode);
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PackedVertexBuffer> deserialize(
        std::string_view text);
};

struct PackResult {
    PackedVertexBuffer buffer;
    QuantizationErrorReceipt error;
};

// Packs skin inputs into the declared layout/mode. Fails closed when any
// vertex exceeds the layout's influence capacity: reduction to top-four is a
// separate explicit decision (reduce_to_top_four), never a silent fallback.
[[nodiscard]] core::Result<PackResult> pack(
    const SkinInputView& skin,
    InfluenceLayout layout,
    QuantizationMode mode);

struct DecodedSkin {
    std::vector<core::Vec3d> positions;
    std::vector<std::uint32_t> influence_counts;
    std::vector<kinematic::JointInfluence> influences;
};

// Decodes a packed buffer back into host values. In fp32 mode the decoded
// positions and weights are exactly the float-cast source values; in fp16 and
// unorm16 mode they carry the quantization error recorded in the packing
// receipt.
[[nodiscard]] core::Result<DecodedSkin> unpack(const PackedVertexBuffer& buffer);

// IEEE-754 binary16 codec with round-to-nearest-even, so unorm/fp16 modes are
// exactly reproducible across toolchains. Non-finite input fails closed by
// producing NaN half bits (0x7E00) which decode to NaN.
[[nodiscard]] std::uint16_t float_to_half_bits(float value) noexcept;
[[nodiscard]] float half_bits_to_float(std::uint16_t bits) noexcept;

struct TopFourReduction {
    std::vector<std::uint32_t> influence_counts;
    std::vector<kinematic::JointInfluence> influences;
    // Largest per-vertex mass of discarded influences before renormalization,
    // so the reduction is auditable instead of silent.
    double max_discarded_weight_mass = 0.0;
};

// Explicit top-four reduction for the influences_4 layout: keeps the four
// largest influences per vertex, renormalizes their sum to one, and reports
// the discarded mass. Input lists must already satisfy the shared admission
// contract.
[[nodiscard]] core::Result<TopFourReduction> reduce_to_top_four(
    std::span<const std::uint32_t> influence_counts,
    std::span<const kinematic::JointInfluence> influences);

} // namespace carto::gpudef
