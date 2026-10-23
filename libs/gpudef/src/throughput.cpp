#include <carto/gpudef/throughput.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

bool slot_less(const ThroughputSlot& left, const ThroughputSlot& right) noexcept {
    if (left.deformation_case != right.deformation_case) {
        return left.deformation_case < right.deformation_case;
    }
    if (left.mode != right.mode) return left.mode < right.mode;
    return left.layout < right.layout;
}

} // namespace

std::uint32_t window_index_for_rate(double vertices_per_second) noexcept {
    if (!std::isfinite(vertices_per_second) || vertices_per_second <= 0.0) {
        return 0U;
    }
    const double scaled =
        vertices_per_second / static_cast<double>(kThroughputWindowBase);
    std::uint32_t index = 0U;
    while (index + 1U < kThroughputWindowCount && scaled >=
            std::ldexp(1.0, static_cast<int>(index + 1))) {
        ++index;
    }
    return index;
}

core::Result<void> ThroughputSlot::validate() const {
    if (window_index >= kThroughputWindowCount || repetitions == 0U ||
        vertex_count == 0U) {
        return core::Result<void>::failure(invalid(
            "throughput slot window, repetition, or vertex count is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ThroughputReceipt::validate() const {
    if (!safe_text(kernel_version, kMaxIdentityBytes) ||
        slots.empty() || slots.size() > kMaxSlots) {
        return core::Result<void>::failure(invalid(
            "throughput receipt kernel version or slot population is invalid"));
    }
    for (std::size_t index = 1U; index < slots.size(); ++index) {
        if (slot_less(slots[index], slots[index - 1U])) {
            return core::Result<void>::failure(invalid(
                "throughput slots must be sorted by case, mode, and layout"));
        }
    }
    for (const ThroughputSlot& slot : slots) {
        if (slot.repetitions > kMaxRepetitions) {
            return core::Result<void>::failure(invalid(
                "throughput slot repetitions exceed the capture bound"));
        }
        if (auto result = slot.validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
    }
    if (device_profile) {
        if (auto result = device_profile->validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
        if (tier != EvidenceTier::native_device) {
            return core::Result<void>::failure(invalid(
                "throughput receipt with a named device profile must declare "
                "native-device tier"));
        }
    } else if (tier != EvidenceTier::cpu_reference) {
        return core::Result<void>::failure(invalid(
            "throughput receipt without a named device profile must stay "
            "cpu-reference"));
    }
    return core::Result<void>::success();
}

std::string ThroughputReceipt::serialize() const {
    std::ostringstream output;
    output << "CARTO_GPUDEF_THROUGHPUT_RECEIPT 1\n";
    output << "KERNEL " << std::quoted(kernel_version) << '\n';
    output << "SLOTS " << slots.size() << '\n';
    for (const ThroughputSlot& slot : slots) {
        output << "SLOT " << deformation_case_name(slot.deformation_case) << ' '
               << quantization_mode_name(slot.mode) << ' '
               << influence_layout_name(slot.layout) << ' ' << slot.window_index
               << ' ' << slot.repetitions << ' ' << slot.vertex_count << '\n';
    }
    output << "TIER " << evidence_tier_name(tier);
    if (device_profile) {
        output << ' ' << std::quoted(device_profile->adapter_id) << ' '
               << std::quoted(device_profile->gpu_name) << ' '
               << std::quoted(device_profile->driver_version) << ' '
               << std::quoted(device_profile->os_name) << ' '
               << std::quoted(device_profile->precision_identity);
    }
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ThroughputReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    // Measurement-invariant canonical form: identity and protocol shape
    // only; window indices are measurement evidence and stay out.
    std::ostringstream canonical;
    canonical << "CARTO_GPUDEF_THROUGHPUT_RECEIPT_IDENTITY 1\n";
    if (device_profile) {
        canonical << "DEVICE " << device_profile->serialize();
    } else {
        canonical << "DEVICE unset\n";
    }
    canonical << "KERNEL " << std::quoted(kernel_version) << '\n';
    for (const ThroughputSlot& slot : slots) {
        canonical << "SLOT " << deformation_case_name(slot.deformation_case)
                  << ' ' << quantization_mode_name(slot.mode) << ' '
                  << influence_layout_name(slot.layout) << ' ' << slot.repetitions
                  << ' ' << slot.vertex_count << '\n';
    }
    canonical << "END\n";
    const std::string text = canonical.str();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}));
}

core::Result<ThroughputReceipt> ThroughputReceipt::deserialize(
    std::string_view text) {
    if (text.empty() || text.size() > 64U * 1024U) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_THROUGHPUT_RECEIPT" || version != 1U) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt framing or schema version is invalid"));
    }
    ThroughputReceipt receipt;
    std::string token;
    std::size_t slot_count = 0U;
    if (!((input >> token) && token == "KERNEL" &&
            (input >> std::quoted(receipt.kernel_version))) ||
        !((input >> token) && token == "SLOTS" && (input >> slot_count)) ||
        slot_count > kMaxSlots) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt header is malformed"));
    }
    auto read_case = [&input](DeformationCase& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const DeformationCase candidate : {
                DeformationCase::linear_blend_skinning,
                DeformationCase::dual_quaternion_skinning,
                DeformationCase::morph_stage,
                DeformationCase::corrective_driver}) {
            if (name == deformation_case_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    auto read_mode = [&input](QuantizationMode& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const QuantizationMode candidate : {
                QuantizationMode::fp32, QuantizationMode::fp16,
                QuantizationMode::unorm16}) {
            if (name == quantization_mode_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    auto read_layout = [&input](InfluenceLayout& field) -> bool {
        std::string name;
        if (!(input >> name)) return false;
        for (const InfluenceLayout candidate : {
                InfluenceLayout::influences_8, InfluenceLayout::influences_4}) {
            if (name == influence_layout_name(candidate)) {
                field = candidate;
                return true;
            }
        }
        return false;
    };
    receipt.slots.reserve(slot_count);
    for (std::size_t index = 0U; index < slot_count; ++index) {
        ThroughputSlot slot;
        if (!((input >> token) && token == "SLOT" &&
                read_case(slot.deformation_case) && read_mode(slot.mode) &&
                read_layout(slot.layout) &&
                (input >> slot.window_index >> slot.repetitions >>
                    slot.vertex_count))) {
            return core::Result<ThroughputReceipt>::failure(invalid(
                "throughput receipt slot is malformed"));
        }
        receipt.slots.push_back(slot);
    }
    std::string tier_name;
    if (!((input >> token) && token == "TIER" && (input >> tier_name))) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt tier is malformed"));
    }
    if (tier_name == "cpu-reference") {
        receipt.tier = EvidenceTier::cpu_reference;
    } else if (tier_name == "native-device") {
        receipt.tier = EvidenceTier::native_device;
        DeviceProfile profile;
        if (!(input >> std::quoted(profile.adapter_id) >>
                std::quoted(profile.gpu_name) >>
                std::quoted(profile.driver_version) >>
                std::quoted(profile.os_name) >>
                std::quoted(profile.precision_identity))) {
            return core::Result<ThroughputReceipt>::failure(invalid(
                "native-device throughput receipt requires a full device profile"));
        }
        receipt.device_profile = std::move(profile);
    } else {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt tier is unknown"));
    }
    if (!((input >> token) && token == "END")) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt is unterminated"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput receipt contains trailing unbound data"));
    }
    if (auto result = receipt.validate(); !result) {
        return core::Result<ThroughputReceipt>::failure(result.error());
    }
    return core::Result<ThroughputReceipt>::success(std::move(receipt));
}

core::Result<ThroughputReceipt> measure_case_throughput(
    const CaseParityInput& workload,
    std::uint32_t repetitions,
    std::string kernel_version) {
    if (repetitions == 0U || repetitions > ThroughputReceipt::kMaxRepetitions) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput capture repetitions must be within the declared bound"));
    }
    if (!safe_text(kernel_version, ThroughputReceipt::kMaxIdentityBytes)) {
        return core::Result<ThroughputReceipt>::failure(invalid(
            "throughput kernel version is empty or unsafe"));
    }
    if (workload.device_profile || workload.reference_is_device_measured) {
        return core::Result<ThroughputReceipt>::failure(refused(
            "packed CPU emulation capture can never mint native-device "
            "throughput evidence; attach named profiles only to real device "
            "runs"));
    }

    // One dry run validates the workload and warms the lane before timing.
    auto dry_run = run_case_emulation(workload);
    if (!dry_run) {
        return core::Result<ThroughputReceipt>::failure(dry_run.error());
    }
    const std::uint64_t vertex_count =
        static_cast<std::uint64_t>(dry_run.value().size());

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t total_vertices = 0U;
    for (std::uint32_t repetition = 0U; repetition < repetitions; ++repetition) {
        auto emulated = run_case_emulation(workload);
        if (!emulated) {
            return core::Result<ThroughputReceipt>::failure(emulated.error());
        }
        total_vertices += vertex_count;
    }
    const auto stopped = std::chrono::steady_clock::now();
    const double elapsed_seconds =
        std::chrono::duration<double>(stopped - started).count();
    // Wall time dies here: only its window index survives.
    const double rate = elapsed_seconds > 0.0
        ? static_cast<double>(total_vertices) / elapsed_seconds
        : 0.0;

    ThroughputReceipt receipt;
    receipt.kernel_version = std::move(kernel_version);
    ThroughputSlot slot;
    slot.deformation_case = workload.deformation_case;
    slot.mode = workload.mode;
    slot.layout = workload.layout;
    slot.window_index = window_index_for_rate(rate);
    slot.repetitions = repetitions;
    slot.vertex_count = vertex_count;
    receipt.slots.push_back(slot);
    receipt.tier = EvidenceTier::cpu_reference;
    if (auto result = receipt.validate(); !result) {
        return core::Result<ThroughputReceipt>::failure(result.error());
    }
    return core::Result<ThroughputReceipt>::success(std::move(receipt));
}

EvidenceTier effective_tier(const ThroughputReceipt& receipt) noexcept {
    if (receipt.device_profile && receipt.tier == EvidenceTier::native_device) {
        return EvidenceTier::native_device;
    }
    return EvidenceTier::cpu_reference;
}

core::Result<void> require_evidence_tier(
    const ThroughputReceipt& receipt,
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
            "native-device throughput evidence requires a named device profile "
            "attached to a real device run"));
    }
    return core::Result<void>::failure(refused(
        "requested evidence tier is beyond what a throughput receipt can hold"));
}

} // namespace carto::gpudef
