#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/gpudef/evidence.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::gpudef {

// Performance evidence schema (spec ch17 "Performance engineering" and
// "Reproducible native acceptance"). This is the receipt shape a real device
// run fills in; headless CPU runs leave device_profile unset and are forever
// cpu_reference evidence. Wall-clock timings are measurement evidence: they
// appear in serialize() for humans and raw-receipt archival, but are excluded
// from the canonical digest so identity stays reproducible.

enum class PerfAcceptance : std::uint8_t {
    pass,
    fail,
    not_run,
};

[[nodiscard]] std::string_view perf_acceptance_name(PerfAcceptance status) noexcept;

// One named kernel-stage timing. Wall time is allowed here, and only here,
// as measurement evidence; it never enters a canonical digest.
struct KernelTimingSlot {
    static constexpr std::size_t kMaxSlotNameBytes = 128U;

    std::string slot_name;
    std::uint64_t wall_time_ns = 0U;
    std::uint32_t samples = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct MemoryStats {
    std::uint64_t resident_bytes = 0U;
    std::uint64_t peak_committed_bytes = 0U;
    std::uint64_t transient_allocation_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PerfReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxTimingSlots = 256U;
    static constexpr std::size_t kMaxIdentityBytes = 256U;

    // Named-hardware profile; UNSET for headless CPU measurements. Native
    // device evidence is impossible without it.
    std::optional<DeviceProfile> device_profile;
    // Kernel/backend version the timings were captured with.
    std::string kernel_version;
    // Sorted by slot name for a canonical, deterministic form.
    std::vector<KernelTimingSlot> timing_slots;
    MemoryStats memory{};
    std::uint32_t animated_character_count = 0U;
    std::uint64_t vertex_count = 0U;
    std::uint64_t triangle_count = 0U;
    std::uint64_t frame_first = 0U;
    std::uint64_t frame_last = 0U;
    PerfAcceptance acceptance = PerfAcceptance::not_run;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    // Digest over identity and shape only: device profile, kernel version,
    // slot names, workload counts and frame range. Timing and memory numbers
    // are measurement evidence and are deliberately excluded, so re-measuring
    // the same workload does not mint a new identity.
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Effective tier of a perf receipt: native_device only when a valid named
// profile is attached, at least one timing slot is present, and an explicit
// acceptance decision was recorded; otherwise cpu_reference. Never returns
// integrated_product.
[[nodiscard]] EvidenceTier effective_tier(const PerfReceipt& receipt) noexcept;

// Honest refusal gate: requesting native_device without named-hardware
// measurement, or any tier beyond it, fails closed with an explicit
// diagnostic instead of a silent downgrade (spec ch00 invariant 7).
[[nodiscard]] core::Result<void> require_evidence_tier(
    const PerfReceipt& receipt,
    EvidenceTier requested);

} // namespace carto::gpudef
