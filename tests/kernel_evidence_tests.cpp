#include <carto/device/kernel_evidence.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

std::string digest(char value) {
    return std::string(64U, value);
}

carto::device::KernelExecutionReceipt native_receipt() {
    using namespace carto;
    device::KernelExecutionReceipt receipt;
    receipt.contract.kernel_id = "cartographer.pbr.opaque";
    receipt.contract.operation = "opaque material resolve";
    receipt.contract.role = device::PrecisionRole::compressed_operand;
    receipt.contract.direction = device::KernelDirection::raster;
    receipt.contract.requested_representation = "mxfp2";
    receipt.contract.requested_arithmetic = "fp32";
    receipt.contract.requested_backend = device::BackendKind::d3d12;
    receipt.contract.fallback_allowed = false;
    receipt.actual_representation = "mxfp2";
    receipt.actual_arithmetic = "fp32";
    receipt.actual_path = device::ExecutionPath::d3d12_shader;
    receipt.device.backend = device::BackendKind::d3d12;
    receipt.device.adapter_name = "test adapter";
    receipt.source_digest = digest('a');
    receipt.binary_digest = digest('b');
    receipt.compiler_digest = digest('c');
    receipt.submission_serial = 3U;
    return receipt;
}

void native_identity_and_local_ledger_are_explicit() {
    using namespace carto;
    const auto receipt = native_receipt();
    REQUIRE(device::validate(receipt));

    device::KernelEvidenceLedger ledger;
    REQUIRE(ledger.append(receipt));
    REQUIRE(ledger.size() == 1U);
    const auto snapshot = ledger.snapshot();
    REQUIRE(snapshot.size() == 1U);
    REQUIRE(snapshot.front().binary_digest == digest('b'));
}

void mismatched_device_and_silent_representation_change_are_rejected() {
    using namespace carto;
    auto receipt = native_receipt();
    receipt.device.backend = device::BackendKind::cpu;
    const auto wrong_device = device::validate(receipt);
    REQUIRE(!wrong_device);

    receipt = native_receipt();
    receipt.actual_representation = "fp32";
    const auto silent_fallback = device::validate(receipt);
    REQUIRE(!silent_fallback);
    REQUIRE(silent_fallback.error().code == core::ErrorCode::invalid_argument);

    receipt = native_receipt();
    receipt.contract.requested_backend = device::BackendKind::vulkan;
    const auto wrong_requested_backend = device::validate(receipt);
    REQUIRE(!wrong_requested_backend);
}

void fallback_requires_contract_permission_and_reason() {
    using namespace carto;
    auto receipt = native_receipt();
    receipt.contract.fallback_allowed = true;
    receipt.actual_path = device::ExecutionPath::software_fp32;
    receipt.actual_representation = "fp32";
    receipt.device.backend = device::BackendKind::cpu;
    receipt.binary_digest.clear();
    receipt.compiler_digest.clear();
    const auto missing_reason = device::validate(receipt);
    REQUIRE(!missing_reason);

    receipt.fallback_used = true;
    receipt.fallback_reason = "native shader unavailable";
    REQUIRE(device::validate(receipt));
}

void cpu_reference_receipts_do_not_fabricate_gpu_identity() {
    using namespace carto;
    device::KernelExecutionReceipt receipt;
    receipt.contract.kernel_id = "cartographer.reference.sample";
    receipt.contract.operation = "texture sample";
    receipt.contract.role = device::PrecisionRole::arithmetic_input;
    receipt.contract.direction = device::KernelDirection::compute;
    receipt.contract.requested_representation = "rgba32f";
    receipt.contract.requested_arithmetic = "fp32";
    receipt.actual_representation = "rgba32f";
    receipt.actual_arithmetic = "fp32";
    receipt.actual_path = device::ExecutionPath::cpu_reference;
    receipt.device.backend = device::BackendKind::cpu;
    receipt.source_digest = "cpu-reference-v1";
    REQUIRE(device::validate(receipt));
}

} // namespace

int main() {
    try {
        native_identity_and_local_ledger_are_explicit();
        mismatched_device_and_silent_representation_change_are_rejected();
        fallback_requires_contract_permission_and_reason();
        cpu_reference_receipts_do_not_fabricate_gpu_identity();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
