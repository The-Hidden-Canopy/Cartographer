#include <carto/gpudef/evidence.hpp>

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

std::string_view evidence_tier_name(EvidenceTier tier) noexcept {
    switch (tier) {
    case EvidenceTier::structural: return "structural";
    case EvidenceTier::cpu_reference: return "cpu-reference";
    case EvidenceTier::native_device: return "native-device";
    }
    return "unknown";
}

core::Result<void> DeviceProfile::validate() const {
    if (!safe_text(adapter_id, kMaxIdentityBytes) ||
        !safe_text(gpu_name, kMaxIdentityBytes) ||
        !safe_text(driver_version, kMaxIdentityBytes) ||
        !safe_text(os_name, kMaxIdentityBytes) ||
        !safe_text(precision_identity, kMaxIdentityBytes)) {
        return core::Result<void>::failure(invalid(
            "device profile adapter, GPU, driver, OS, or precision identity is invalid"));
    }
    return core::Result<void>::success();
}

std::string DeviceProfile::serialize() const {
    std::ostringstream output;
    output << "CARTO_GPUDEF_DEVICE_PROFILE 1\n";
    output << "ADAPTER " << std::quoted(adapter_id) << '\n';
    output << "GPU " << std::quoted(gpu_name) << '\n';
    output << "DRIVER " << std::quoted(driver_version) << '\n';
    output << "OS " << std::quoted(os_name) << '\n';
    output << "PRECISION " << std::quoted(precision_identity) << '\n';
    output << "END\n";
    return output.str();
}

core::Result<DeviceProfile> DeviceProfile::deserialize(std::string_view text) {
    if (text.empty() || text.size() > 8U * 1024U) {
        return core::Result<DeviceProfile>::failure(invalid(
            "device profile text is empty or exceeds its byte bound"));
    }
    std::istringstream input{std::string(text)};
    std::string magic;
    std::uint32_t version = 0U;
    if (!(input >> magic) || !(input >> version) ||
        magic != "CARTO_GPUDEF_DEVICE_PROFILE" || version != 1U) {
        return core::Result<DeviceProfile>::failure(invalid(
            "device profile framing or schema version is invalid"));
    }
    DeviceProfile profile;
    std::string token;
    auto read_field = [&input, &token](const char* key, std::string& field)
        -> bool {
        return (input >> token) && token == key && (input >> std::quoted(field));
    };
    if (!read_field("ADAPTER", profile.adapter_id) ||
        !read_field("GPU", profile.gpu_name) ||
        !read_field("DRIVER", profile.driver_version) ||
        !read_field("OS", profile.os_name) ||
        !read_field("PRECISION", profile.precision_identity) ||
        !(input >> token) || token != "END") {
        return core::Result<DeviceProfile>::failure(invalid(
            "device profile payload is malformed"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<DeviceProfile>::failure(invalid(
            "device profile text contains trailing unbound data"));
    }
    if (auto result = profile.validate(); !result) {
        return core::Result<DeviceProfile>::failure(result.error());
    }
    return core::Result<DeviceProfile>::success(std::move(profile));
}

core::Result<void> claim_integrated_product() noexcept {
    return core::Result<void>::failure(refused(
        "gpudef holds packing, CPU emulation, parity and perf-schema evidence "
        "only; integrated-product evidence requires whole-product acceptance "
        "outside this library"));
}

} // namespace carto::gpudef
