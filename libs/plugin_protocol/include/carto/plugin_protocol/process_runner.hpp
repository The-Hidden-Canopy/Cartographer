#pragma once

#include <carto/core/result.hpp>
#include <carto/plugin_protocol/protocol.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace carto::plugin_protocol {

struct ProcessRequest {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::span<const std::uint8_t> input;
    ExecutionBudget budget;
};

struct ProcessOutput {
    std::vector<std::uint8_t> bytes;
    std::uint32_t exit_code = 0U;
};

// Runs one executable in a child process with bounded stdin/stdout, wall time,
// and aggregate job user-CPU time. This is crash/resource containment only;
// it does not sandbox filesystem, network, or user-token authority.
[[nodiscard]] core::Result<ProcessOutput> run_isolated_process(
    const ProcessRequest& request);

} // namespace carto::plugin_protocol
