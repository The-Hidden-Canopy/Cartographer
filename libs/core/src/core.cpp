#include <carto/core/diagnostic.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

namespace carto::core {

const char* error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::not_found:
        return "not_found";
    case ErrorCode::invalid_state:
        return "invalid_state";
    case ErrorCode::cycle_detected:
        return "cycle_detected";
    case ErrorCode::validation_failed:
        return "validation_failed";
    case ErrorCode::io_error:
        return "io_error";
    case ErrorCode::unsupported:
        return "unsupported";
    case ErrorCode::stale_data:
        return "stale_data";
    case ErrorCode::version_mismatch:
        return "version_mismatch";
    }
    return "unknown";
}

} // namespace carto::core

