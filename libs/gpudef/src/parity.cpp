#include <carto/gpudef/parity.hpp>

#include <algorithm>
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

} // namespace

core::Result<void> DeviceCaps::validate() const {
    if (max_influences_per_vertex == 0U || max_influences_per_vertex > 8U ||
        max_joints == 0U || max_joints > PackedVertexBuffer::kMaxJoints) {
        return core::Result<void>::failure(invalid(
            "device caps influence or joint bounds are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ParityReceipt::validate() const {
    if (vertex_count == 0U || joint_count == 0U ||
        joint_count > PackedVertexBuffer::kMaxJoints ||
        max_position_error < 0.0 || mean_position_error < 0.0 ||
        rms_position_error < 0.0 || bounding_diagonal <= 0.0 ||
        max_error_over_diagonal < 0.0 ||
        !std::isfinite(max_position_error) || !std::isfinite(mean_position_error) ||
        !std::isfinite(rms_position_error) || !std::isfinite(bounding_diagonal) ||
        !std::isfinite(max_error_over_diagonal)) {
        return core::Result<void>::failure(invalid(
            "parity receipt counts or error statistics are invalid"));
    }
    if (auto result = caps.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (device_profile) {
        if (auto result = device_profile->validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
        if (tier != EvidenceTier::native_device) {
            return core::Result<void>::failure(invalid(
                "parity receipt with a named device profile must declare native-device tier"));
        }
    } else if (tier != EvidenceTier::cpu_reference) {
        return core::Result<void>::failure(invalid(
            "parity receipt without a named device profile must stay cpu-reference"));
    }
    return core::Result<void>::success();
}

std::string ParityReceipt::serialize() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "CARTO_GPUDEF_PARITY_RECEIPT 1\n";
    output << "MODE " << quantization_mode_name(mode) << ' '
           << influence_layout_name(layout) << ' ' << vertex_count << ' '
           << joint_count << '\n';
    output << "ERROR " << max_position_error << ' ' << mean_position_error << ' '
           << rms_position_error << ' ' << bounding_diagonal << ' '
           << max_error_over_diagonal << '\n';
    output << "CAPS " << caps.max_influences_per_vertex << ' '
           << (caps.fp16_storage ? 1U : 0U) << ' '
           << (caps.unorm_storage ? 1U : 0U) << ' ' << caps.max_joints << '\n';
    output << "TIER " << evidence_tier_name(tier);
    if (device_profile) {
        output << ' ' << std::quoted(device_profile->adapter_id);
    }
    output << '\n';
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> ParityReceipt::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<ParityReceipt> run_parity(const ParityInput& input) {
    if (input.reference_positions.size() != input.skin.positions.size()) {
        return core::Result<ParityReceipt>::failure(invalid(
            "parity oracle position count must match the skin vertex count"));
    }
    if (input.reference_is_device_measured && !input.device_profile) {
        return core::Result<ParityReceipt>::failure(refused(
            "device-measured reference positions require an attached named device profile"));
    }
    if (input.device_profile) {
        if (auto result = input.device_profile->validate(); !result) {
            return core::Result<ParityReceipt>::failure(result.error());
        }
    }
    if (auto result = input.caps.validate(); !result) {
        return core::Result<ParityReceipt>::failure(result.error());
    }
    if (input.caps.max_influences_per_vertex <
        influences_per_vertex(input.layout)) {
        return core::Result<ParityReceipt>::failure(refused(
            "declared device caps cannot hold the requested influence layout"));
    }

    auto packed = pack(input.skin, input.layout, input.mode);
    if (!packed) return core::Result<ParityReceipt>::failure(packed.error());
    auto emulated = emulate_packed_deform(
        packed.value().buffer, input.skin_matrices);
    if (!emulated) return core::Result<ParityReceipt>::failure(emulated.error());

    ParityReceipt receipt;
    receipt.mode = input.mode;
    receipt.layout = input.layout;
    receipt.vertex_count = static_cast<std::uint32_t>(input.skin.positions.size());
    receipt.joint_count = input.skin.joint_count;
    receipt.caps = input.caps;

    core::Bounds3d bounds;
    for (const core::Vec3d& position : input.reference_positions) {
        bounds.include(position);
    }
    if (!bounds.valid()) {
        return core::Result<ParityReceipt>::failure(invalid(
            "parity oracle positions must be finite and non-empty"));
    }
    const double dx = bounds.maximum.x - bounds.minimum.x;
    const double dy = bounds.maximum.y - bounds.minimum.y;
    const double dz = bounds.maximum.z - bounds.minimum.z;
    receipt.bounding_diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);

    double sum = 0.0;
    double sum_squares = 0.0;
    for (std::size_t vertex = 0U; vertex < emulated.value().size(); ++vertex) {
        const double error =
            (emulated.value()[vertex] - input.reference_positions[vertex]).length();
        receipt.max_position_error = std::max(receipt.max_position_error, error);
        sum += error;
        sum_squares += error * error;
    }
    const double count = static_cast<double>(emulated.value().size());
    receipt.mean_position_error = sum / count;
    receipt.rms_position_error = std::sqrt(sum_squares / count);
    receipt.max_error_over_diagonal = receipt.max_position_error /
        receipt.bounding_diagonal;
    receipt.device_profile = input.device_profile;
    receipt.tier = (input.device_profile && input.reference_is_device_measured)
        ? EvidenceTier::native_device
        : EvidenceTier::cpu_reference;
    if (auto result = receipt.validate(); !result) {
        return core::Result<ParityReceipt>::failure(result.error());
    }
    return core::Result<ParityReceipt>::success(std::move(receipt));
}

EvidenceTier effective_tier(const ParityReceipt& receipt) noexcept {
    if (receipt.device_profile && receipt.tier == EvidenceTier::native_device) {
        return EvidenceTier::native_device;
    }
    return EvidenceTier::cpu_reference;
}

core::Result<void> require_evidence_tier(
    const ParityReceipt& receipt,
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
            "native-device evidence requires an attached named-hardware profile "
            "and device-measured reference positions"));
    }
    return core::Result<void>::failure(refused(
        "requested evidence tier is beyond what a parity receipt can hold"));
}

} // namespace carto::gpudef
