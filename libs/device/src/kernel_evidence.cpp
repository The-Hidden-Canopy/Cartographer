#include <carto/device/kernel_evidence.hpp>

#include <cctype>
#include <utility>

namespace carto::device {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

bool valid_precision_role(PrecisionRole role) noexcept {
    switch (role) {
    case PrecisionRole::authoritative_state:
    case PrecisionRole::compressed_operand:
    case PrecisionRole::decode_staging:
    case PrecisionRole::arithmetic_input:
    case PrecisionRole::accumulator:
    case PrecisionRole::output:
        return true;
    }
    return false;
}

bool valid_direction(KernelDirection direction) noexcept {
    switch (direction) {
    case KernelDirection::forward:
    case KernelDirection::backward:
    case KernelDirection::optimizer:
    case KernelDirection::raster:
    case KernelDirection::compute:
    case KernelDirection::presentation:
        return true;
    }
    return false;
}

bool valid_path(ExecutionPath path) noexcept {
    switch (path) {
    case ExecutionPath::cpu_reference:
    case ExecutionPath::d3d12_shader:
    case ExecutionPath::vulkan_shader:
    case ExecutionPath::software_fp32:
    case ExecutionPath::hybrid:
    case ExecutionPath::fallback:
        return true;
    }
    return false;
}

bool valid_backend(BackendKind backend) noexcept {
    switch (backend) {
    case BackendKind::cpu:
    case BackendKind::d3d12:
    case BackendKind::vulkan:
        return true;
    }
    return false;
}

bool requires_compiled_identity(ExecutionPath path) noexcept {
    return path == ExecutionPath::d3d12_shader || path == ExecutionPath::vulkan_shader;
}

} // namespace

bool is_sha256_digest(std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const char character : value) {
        if (std::isxdigit(static_cast<unsigned char>(character)) == 0) return false;
    }
    return true;
}

core::Result<void> validate(const KernelContract& contract) {
    if (contract.kernel_id.empty() || contract.operation.empty()) {
        return invalid("kernel contracts require a stable id and operation");
    }
    if (!valid_precision_role(contract.role)) {
        return invalid("kernel contract contains an unknown precision role");
    }
    if (!valid_direction(contract.direction)) {
        return invalid("kernel contract contains an unknown direction");
    }
    if (contract.requested_representation.empty() || contract.requested_arithmetic.empty()) {
        return invalid("kernel contracts require requested representation and arithmetic");
    }
    if (contract.requested_backend && !valid_backend(*contract.requested_backend)) {
        return invalid("kernel contract contains an unknown requested backend");
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const KernelExecutionReceipt& receipt) {
    if (auto result = validate(receipt.contract); !result) return result;
    if (!valid_backend(receipt.device.backend)) {
        return invalid("kernel execution receipt contains an unknown device backend");
    }
    if (receipt.actual_representation.empty() || receipt.actual_arithmetic.empty()) {
        return invalid("kernel execution receipts require actual representation and arithmetic");
    }
    if (!valid_path(receipt.actual_path)) {
        return invalid("kernel execution receipt contains an unknown execution path");
    }
    if (receipt.fallback_used && !receipt.contract.fallback_allowed) {
        return invalid("kernel fallback was recorded although the contract disallows fallback");
    }
    if (receipt.fallback_used && receipt.fallback_reason.empty()) {
        return invalid("kernel fallback receipts require a reason");
    }
    if (!receipt.fallback_used && !receipt.fallback_reason.empty()) {
        return invalid("fallback reason is present without a recorded fallback");
    }
    if (receipt.actual_path == ExecutionPath::fallback && !receipt.fallback_used) {
        return invalid("fallback execution path requires fallback_used");
    }
    if (receipt.actual_representation != receipt.contract.requested_representation &&
        !receipt.fallback_used) {
        return invalid("a representation change must be recorded as a fallback");
    }
    if (receipt.actual_path == ExecutionPath::software_fp32 &&
        receipt.actual_arithmetic != "fp32") {
        return invalid("software_fp32 receipts must identify FP32 arithmetic");
    }
    if (receipt.actual_path == ExecutionPath::cpu_reference &&
        receipt.device.backend != BackendKind::cpu) {
        return invalid("CPU reference receipts require a CPU device identity");
    }
    if (receipt.actual_path == ExecutionPath::d3d12_shader &&
        receipt.device.backend != BackendKind::d3d12) {
        return invalid("D3D12 shader receipts require a D3D12 device identity");
    }
    if (receipt.actual_path == ExecutionPath::vulkan_shader &&
        receipt.device.backend != BackendKind::vulkan) {
        return invalid("Vulkan shader receipts require a Vulkan device identity");
    }
    if (receipt.contract.requested_backend && !receipt.fallback_used &&
        receipt.device.backend != *receipt.contract.requested_backend) {
        return invalid("receipt device does not match the requested backend");
    }
    if (receipt.binary_digest.empty() && requires_compiled_identity(receipt.actual_path)) {
        return invalid("native shader receipts require a binary digest");
    }
    if (!receipt.binary_digest.empty() && !is_sha256_digest(receipt.binary_digest)) {
        return invalid("binary digest must be a SHA-256 hex value");
    }
    if (requires_compiled_identity(receipt.actual_path) &&
        !is_sha256_digest(receipt.source_digest)) {
        return invalid("native shader receipts require a SHA-256 source digest");
    }
    if (requires_compiled_identity(receipt.actual_path) && receipt.compiler_digest.empty()) {
        return invalid("native shader receipts require compiler recipe identity");
    }
    if (requires_compiled_identity(receipt.actual_path) &&
        !is_sha256_digest(receipt.compiler_digest)) {
        return invalid("native shader receipts require a SHA-256 compiler recipe digest");
    }
    return core::Result<void>::success();
}

core::Result<void> KernelEvidenceLedger::append(KernelExecutionReceipt receipt) {
    if (auto result = validate(receipt); !result) return result;
    std::scoped_lock lock(mutex_);
    receipts_.push_back(std::move(receipt));
    return core::Result<void>::success();
}

std::vector<KernelExecutionReceipt> KernelEvidenceLedger::snapshot() const {
    std::scoped_lock lock(mutex_);
    return receipts_;
}

std::size_t KernelEvidenceLedger::size() const noexcept {
    std::scoped_lock lock(mutex_);
    return receipts_.size();
}

} // namespace carto::device
