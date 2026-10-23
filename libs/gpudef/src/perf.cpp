#include <carto/gpudef/perf.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace carto::gpudef {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic refused(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

} // namespace

std::string_view perf_acceptance_name(PerfAcceptance status) noexcept {
    switch (status) {
    case PerfAcceptance::pass: return "pass";
    case PerfAcceptance::fail: return "fail";
    case PerfAcceptance::not_run: return "not-run";
    }
    return "unknown";
}

core::Result<void> KernelTimingSlot::validate() const {
    if (!safe_text(slot_name, kMaxSlotNameBytes) || samples == 0U) {
        return core::Result<void>::failure(invalid(
            "kernel timing slot name or sample count is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> MemoryStats::validate() const {
    if (resident_bytes > peak_committed_bytes) {
        return core::Result<void>::failure(invalid(
            "resident bytes must not exceed peak committed bytes"));
    }
    return core::Result<void>::success();
}

core::Result<void> PerfReceipt::validate() const {
    if (!safe_text(kernel_version, kMaxIdentityBytes) ||
        timing_slots.size() > kMaxTimingSlots || timing_slots.empty() ||
        frame_last < frame_first) {
        return core::Result<void>::failure(invalid(
            "perf receipt kernel version, timing slots, or frame range is invalid"));
    }
    if (animated_character_count == 0U || vertex_count == 0U) {
        return core::Result<void>::failure(invalid(
            "perf receipt workload counts must be non-zero"));
    }
    if (auto result = memory.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    for (const KernelTimingSlot& slot : timing_slots) {
        if (auto result = slot.validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
    }
    if (device_profile) {
        if (auto result = device_profile->validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
    } else if (acceptance != PerfAcceptance::not_run) {
        // An acceptance decision on unnamed hardware is not native evidence;
        // fail closed rather than let it read as one.
        return core::Result<void>::failure(invalid(
            "perf receipt acceptance without a named device profile is invalid"));
    }
    return core::Result<void>::success();
}

std::string PerfReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTO_GPUDEF_PERF_RECEIPT 1\n";
    output << "KERNEL " << std::quoted(kernel_version) << '\n';
    output << "WORKLOAD " << animated_character_count << ' ' << vertex_count
           << ' ' << triangle_count << ' ' << frame_first << ' ' << frame_last
           << '\n';
    output << "MEMORY " << memory.resident_bytes << ' '
           << memory.peak_committed_bytes << ' '
           << memory.transient_allocation_count << '\n';
    output << "SLOTS " << timing_slots.size() << '\n';
    for (const KernelTimingSlot& slot : timing_slots) {
        output << "SLOT " << std::quoted(slot.slot_name) << ' '
               << slot.wall_time_ns << ' ' << slot.samples << '\n';
    }
    output << "ACCEPTANCE " << perf_acceptance_name(acceptance) << '\n';
    output << "DEVICE ";
    if (device_profile) {
        output << std::quoted(device_profile->adapter_id) << ' '
               << std::quoted(device_profile->gpu_name);
    } else {
        output << "unset";
    }
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PerfReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    // Measurement-invariant canonical form: identity and workload shape only.
    // Wall-clock timings, memory counters and the acceptance decision are
    // measurement evidence, so re-measuring the same workload does not mint a
    // new identity.
    std::ostringstream canonical;
    canonical << "CARTO_GPUDEF_PERF_RECEIPT_IDENTITY 1\n";
    if (device_profile) {
        canonical << "DEVICE " << device_profile->serialize();
    } else {
        canonical << "DEVICE unset\n";
    }
    canonical << "KERNEL " << std::quoted(kernel_version) << '\n';
    canonical << "WORKLOAD " << animated_character_count << ' ' << vertex_count
              << ' ' << triangle_count << ' ' << frame_first << ' ' << frame_last
              << '\n';
    for (const KernelTimingSlot& slot : timing_slots) {
        canonical << "SLOT " << std::quoted(slot.slot_name) << '\n';
    }
    canonical << "END\n";
    const std::string text = canonical.str();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}));
}

EvidenceTier effective_tier(const PerfReceipt& receipt) noexcept {
    if (receipt.device_profile && !receipt.timing_slots.empty() &&
        receipt.acceptance != PerfAcceptance::not_run) {
        return EvidenceTier::native_device;
    }
    return EvidenceTier::cpu_reference;
}

core::Result<void> require_evidence_tier(
    const PerfReceipt& receipt,
    EvidenceTier requested) {
    switch (requested) {
    case EvidenceTier::structural:
    case EvidenceTier::cpu_reference:
        return core::Result<void>::success();
    case EvidenceTier::native_device:
        if (effective_tier(receipt) == EvidenceTier::native_device) {
            return core::Result<void>::success();
        }
        return core::Result<void>::failure(refused(
            "native-device perf evidence requires a named device profile, at "
            "least one kernel timing slot, and an explicit acceptance decision"));
    }
    return core::Result<void>::failure(refused(
        "requested evidence tier is beyond what a perf receipt can hold"));
}

} // namespace carto::gpudef
