#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
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

// Declared capability profile of the backend under test. For the headless
// CPU emulation harness this describes the emulated stream contract; for a
// real device run it must describe the actual device capabilities.
struct DeviceCaps {
    std::uint32_t max_influences_per_vertex = 8U;
    bool fp16_storage = true;
    bool unorm_storage = true;
    std::uint32_t max_joints = PackedVertexBuffer::kMaxJoints;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ParityInput {
    SkinInputView skin;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    QuantizationMode mode = QuantizationMode::fp32;
    // skin_matrices[joint_index] = joint global pose composed with the
    // inverse bind transform, in the Mat34 convention.
    std::span<const Mat34> skin_matrices;
    // Caller-provided CPU reference deformed positions, one per skin vertex.
    // This is the oracle; the harness never invents it.
    std::span<const core::Vec3d> reference_positions;
    DeviceCaps caps{};
    // Named-hardware profile. Must be absent for headless CPU runs; attaching
    // one while claiming device-measured reference positions is only valid
    // with reference_is_device_measured set and a fully valid profile.
    std::optional<DeviceProfile> device_profile;
    // Set true only when reference_positions came from a real device run
    // named by device_profile. Claiming this without a valid profile fails
    // closed (honest refusal, never silent).
    bool reference_is_device_measured = false;
};

// Parity evidence for one {skin inputs, packed layout, mode} evaluation.
// device_profile is explicitly UNSET and tier stays cpu_reference for CPU
// tier runs; native_device is only assigned when a fully valid named-hardware
// profile is attached and the reference is declared device-measured.
struct ParityReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    QuantizationMode mode = QuantizationMode::fp32;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    std::uint32_t vertex_count = 0U;
    std::uint32_t joint_count = 0U;
    // Absolute position error of the emulation against the caller oracle.
    double max_position_error = 0.0;
    double mean_position_error = 0.0;
    double rms_position_error = 0.0;
    // Reference bounding diagonal used to relativize errors (spec ch17
    // numerical parity: errors relative to the mesh bounding diagonal).
    double bounding_diagonal = 0.0;
    double max_error_over_diagonal = 0.0;
    DeviceCaps caps{};
    std::optional<DeviceProfile> device_profile;
    EvidenceTier tier = EvidenceTier::cpu_reference;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Runs the headless parity evaluation: pack -> packed CPU emulation ->
// compare against the caller oracle. Fails closed on invalid skin inputs,
// mismatched oracle size, or a device-measured claim without a valid named
// profile. Deterministic: fixed evaluation order, no clock anywhere.
[[nodiscard]] core::Result<ParityReceipt> run_parity(const ParityInput& input);

// Effective tier of a finished parity run: native_device only when a valid
// named-hardware profile is attached and the reference was declared
// device-measured; otherwise cpu_reference. This function can never return
// integrated_product.
[[nodiscard]] EvidenceTier effective_tier(const ParityReceipt& receipt) noexcept;

// Honest refusal gate (spec ch00 "explicit unsupported"): requesting
// native_device evidence without attached named-hardware measurement fails
// with an explicit diagnostic; requesting anything beyond native_device
// (integrated product) is refused outright by this library.
[[nodiscard]] core::Result<void> require_evidence_tier(
    const ParityReceipt& receipt,
    EvidenceTier requested);

} // namespace carto::gpudef
