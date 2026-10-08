#pragma once

#include <string>
#include <utility>
#include <vector>

namespace carto::core {

enum class ErrorCode {
    invalid_argument,
    not_found,
    invalid_state,
    cycle_detected,
    validation_failed,
    io_error,
    unsupported,
    stale_data,
    version_mismatch,
};

const char* error_code_name(ErrorCode code) noexcept;

struct Diagnostic {
    ErrorCode code;
    std::string message;
    std::vector<std::string> context;

    Diagnostic(ErrorCode code_value, std::string message_value)
        : code(code_value), message(std::move(message_value)) {}

    Diagnostic with_context(std::string value) const {
        Diagnostic copy = *this;
        copy.context.push_back(std::move(value));
        return copy;
    }
};

} // namespace carto::core

