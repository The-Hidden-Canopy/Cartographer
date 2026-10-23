#pragma once

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::gpudef {

// Evidence tiers follow the spec's release levels restricted to what device
// measurement can support: structural verification, CPU reference, and native
// device. Integrated product is deliberately absent: it requires whole-product
// end-to-end acceptance that no single subsystem library can mint.
enum class EvidenceTier : std::uint8_t {
    structural,
    cpu_reference,
    native_device,
};

[[nodiscard]] std::string_view evidence_tier_name(EvidenceTier tier) noexcept;

// Named-hardware identity attached to device-measured evidence (spec ch17
// "Reproducible native acceptance": adapter id, GPU name, driver, OS,
// precision profile). Every field is mandatory and bounded so an anonymous
// or partial profile can never launder CPU-tier results into native_device.
struct DeviceProfile {
    static constexpr std::size_t kMaxIdentityBytes = 256U;

    std::string adapter_id;
    std::string gpu_name;
    std::string driver_version;
    std::string os_name;
    // Precision profile the device evidence was captured under, e.g.
    // "fp32-lbs" or "fp16-lbs-unorm16-weights".
    std::string precision_identity;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<DeviceProfile> deserialize(
        std::string_view text);
};

// Honest non-claim (spec ch17, ch00 invariant "explicit unsupported"): this
// library holds packing, CPU emulation, parity and perf-schema evidence only.
// It can never produce integrated-product evidence, so the claim always fails
// closed with an explicit refusal instead of a silent downgrade.
[[nodiscard]] core::Result<void> claim_integrated_product() noexcept;

} // namespace carto::gpudef
