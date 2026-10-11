#pragma once

#include <carto/core/result.hpp>
#include <carto/device/device.hpp>

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::device {

// A precision role describes why a representation exists in a kernel. It is
// intentionally separate from arithmetic precision: an int2-delta operand can
// still be decoded into an FP32 arithmetic path, and an authoritative state
// must not be inferred from the storage format alone.
enum class PrecisionRole {
    authoritative_state,
    compressed_operand,
    decode_staging,
    arithmetic_input,
    accumulator,
    output,
};

enum class KernelDirection {
    forward,
    backward,
    optimizer,
    raster,
    compute,
    presentation,
};

enum class ExecutionPath {
    cpu_reference,
    d3d12_shader,
    vulkan_shader,
    software_fp32,
    hybrid,
    fallback,
};

struct KernelContract {
    std::string kernel_id;
    std::string operation;
    PrecisionRole role = PrecisionRole::authoritative_state;
    KernelDirection direction = KernelDirection::compute;
    std::string requested_representation;
    std::string requested_arithmetic;
    std::optional<BackendKind> requested_backend;
    bool fallback_allowed = false;
};

// This is a local execution receipt, not telemetry. It contains no network
// destination, account identifier, prompt, model handle, or usage metric.
// Timing and throughput claims deliberately live outside this contract and
// must only be added by a separately measured experiment.
struct KernelExecutionReceipt {
    KernelContract contract;
    std::string actual_representation;
    std::string actual_arithmetic;
    ExecutionPath actual_path = ExecutionPath::cpu_reference;
    DeviceIdentity device;
    std::string source_digest;
    std::string binary_digest;
    std::string compiler_digest;
    std::uint64_t submission_serial = 0U;
    bool fallback_used = false;
    std::string fallback_reason;
};

[[nodiscard]] bool is_sha256_digest(std::string_view value) noexcept;
[[nodiscard]] core::Result<void> validate(const KernelContract& contract);
[[nodiscard]] core::Result<void> validate(const KernelExecutionReceipt& receipt);

// The ledger is process-local and append-only for its lifetime. It has no
// exporter by design; callers can explicitly inspect a snapshot or discard it
// when the local session ends. This makes evidence available without turning
// the public application into a tracking surface.
class KernelEvidenceLedger {
public:
    [[nodiscard]] core::Result<void> append(KernelExecutionReceipt receipt);
    [[nodiscard]] std::vector<KernelExecutionReceipt> snapshot() const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<KernelExecutionReceipt> receipts_;
};

} // namespace carto::device
