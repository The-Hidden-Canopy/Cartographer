#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/gpudef/deform_cases.hpp>
#include <carto/gpudef/evidence.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::gpudef {

// Bounded performance evidence (spec ch17 "Performance engineering").
// Vertex-throughput is captured into declared windows — power-of-two
// vertices-per-second bands — with an explicit repetition count. Wall time
// exists only transiently inside the capture call: it is binned into a
// window index and discarded, so no receipt ever carries a clock reading
// and re-measuring the same workload does not mint a new identity.
//
// Honest tier rule: measure_case_throughput runs the packed CPU emulation
// lane, so it can never produce native-device evidence and refuses an
// attached device profile outright. Native-device throughput receipts only
// enter through deserialize of a receipt a named device run produced.

// Declared window table: window i admits measured rates in
// [kWindowBase << i, kWindowBase << (i + 1)) vertices per second, with the
// base at 1024 vertices per second and the top window open-ended.
inline constexpr std::uint32_t kThroughputWindowBase = 1024U;
inline constexpr std::uint32_t kThroughputWindowCount = 40U;

// Bins a measured rate into the declared window table. Non-finite or
// non-positive rates land in window 0; rates above the table clamp to the
// top window. Pure function of the rate, so window assignment is
// deterministic for a given measurement.
[[nodiscard]] std::uint32_t window_index_for_rate(double vertices_per_second) noexcept;

struct ThroughputSlot {
    DeformationCase deformation_case = DeformationCase::linear_blend_skinning;
    QuantizationMode mode = QuantizationMode::fp32;
    InfluenceLayout layout = InfluenceLayout::influences_8;
    // Which declared window the measured throughput fell into. Measurement
    // evidence: excluded from the canonical digest.
    std::uint32_t window_index = 0U;
    // Repetitions of the workload aggregated into this slot, and the vertex
    // count of one repetition. Both are protocol shape, not measurement, so
    // they are part of the canonical identity.
    std::uint32_t repetitions = 0U;
    std::uint64_t vertex_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ThroughputReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSlots = 64U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;
    static constexpr std::uint32_t kMaxRepetitions = 100'000U;

    // Named-hardware profile; UNSET for CPU capture. Native device evidence
    // is impossible without it, and the capture entry point refuses one.
    std::optional<DeviceProfile> device_profile;
    // Kernel/backend version the capture ran with.
    std::string kernel_version;
    // Sorted by (case, mode, layout) for a canonical, deterministic form.
    std::vector<ThroughputSlot> slots;
    EvidenceTier tier = EvidenceTier::cpu_reference;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    // Digest over identity and protocol shape only: device profile, kernel
    // version, and per-slot case/mode/layout, vertex count, and repetition
    // count. Window indices are measurement evidence and are deliberately
    // excluded, so re-measuring the same workload does not mint a new
    // identity.
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<ThroughputReceipt> deserialize(
        std::string_view text);
};

// Captures vertex-throughput evidence for one deformation-case workload:
// the packed emulation lane is run `repetitions` times, the batch wall time
// is binned into the declared window table, and a single-slot CPU-tier
// receipt is returned. Fails closed on an invalid workload, zero
// repetitions, an unsafe kernel version, or an attached device profile
// (CPU emulation can never mint native-device evidence).
[[nodiscard]] core::Result<ThroughputReceipt> measure_case_throughput(
    const CaseParityInput& workload,
    std::uint32_t repetitions,
    std::string kernel_version);

[[nodiscard]] EvidenceTier effective_tier(
    const ThroughputReceipt& receipt) noexcept;

[[nodiscard]] core::Result<void> require_evidence_tier(
    const ThroughputReceipt& receipt,
    EvidenceTier requested);

} // namespace carto::gpudef
