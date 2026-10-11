#include <carto/device/device.hpp>
#include <carto/device/kernel_evidence.hpp>

#include <cmath>
#include <utility>

namespace carto::device {

core::Result<void> validate(const ShaderBinary& binary) {
    if (binary.debug_name.empty() || binary.entry_point.empty() || binary.bytes.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "shader binaries require a name, entry point, and non-empty bytecode"));
    }
    const auto stage = static_cast<std::uint32_t>(binary.stage);
    if (stage != static_cast<std::uint32_t>(gpu::ShaderStage::vertex) &&
        stage != static_cast<std::uint32_t>(gpu::ShaderStage::fragment) &&
        stage != static_cast<std::uint32_t>(gpu::ShaderStage::compute)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "shader binary must identify exactly one supported stage"));
    }
    if (!binary.binary_digest.empty() && !is_sha256_digest(binary.binary_digest)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "shader binary digest must be a SHA-256 hex value"));
    }
    return core::Result<void>::success();
}

} // namespace carto::device
