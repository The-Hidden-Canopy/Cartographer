#include <carto/character/drivers.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_token(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

} // namespace

std::string_view driver_interpolation_name(DriverInterpolation interpolation) noexcept {
    switch (interpolation) {
    case DriverInterpolation::linear: return "linear";
    case DriverInterpolation::smoothstep: return "smoothstep";
    }
    return "unknown";
}

core::Result<void> DriverInput::validate() const {
    switch (kind) {
    case Kind::control:
        if (!control || driver || !pose_parameter.empty()) {
            return core::Result<void>::failure(invalid(
                "driver control input must set exactly one alternative"));
        }
        break;
    case Kind::pose_parameter:
        if (control || driver || !safe_token(pose_parameter, CorrectiveDriver::kMaxPoseParameterBytes)) {
            return core::Result<void>::failure(invalid(
                "driver pose-parameter input must set exactly one valid alternative"));
        }
        break;
    case Kind::driver:
        if (!driver || control || !pose_parameter.empty()) {
            return core::Result<void>::failure(invalid(
                "driver reference input must set exactly one alternative"));
        }
        break;
    default:
        return core::Result<void>::failure(invalid(
            "driver input kind is invalid"));
    }
    return core::Result<void>::success();
}

bool DriverInput::operator<(const DriverInput& other) const noexcept {
    return std::tuple{static_cast<std::uint8_t>(kind), control.value, pose_parameter,
            driver.value} <
        std::tuple{static_cast<std::uint8_t>(other.kind), other.control.value,
            other.pose_parameter, other.driver.value};
}

core::Result<void> CorrectiveDriver::validate() const {
    if (!id || inputs.empty() || inputs.size() > kMaxInputs ||
        domain_minimum.size() != inputs.size() || domain_maximum.size() != inputs.size() ||
        !basis.finite() ||
        (interpolation != DriverInterpolation::linear &&
         interpolation != DriverInterpolation::smoothstep)) {
        return core::Result<void>::failure(invalid(
            "corrective driver identity, inputs, domains, basis, or interpolation is invalid"));
    }
    if (!std::is_sorted(inputs.begin(), inputs.end()) ||
        std::adjacent_find(inputs.begin(), inputs.end()) != inputs.end()) {
        return core::Result<void>::failure(validation(
            "corrective driver inputs must be sorted and unique"));
    }
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
        if (auto result = inputs[index].validate(); !result) return result;
        // Fail-closed self-reference: a driver may never feed itself.
        if (inputs[index].kind == DriverInput::Kind::driver &&
            inputs[index].driver == id) {
            return core::Result<void>::failure(validation(
                "corrective driver may not reference itself as an input"));
        }
        if (!std::isfinite(domain_minimum[index]) ||
            !std::isfinite(domain_maximum[index]) ||
            domain_minimum[index] >= domain_maximum[index]) {
            return core::Result<void>::failure(validation(
                "corrective driver domains must be finite and strictly ordered"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
