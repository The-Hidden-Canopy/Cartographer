#include <carto/deform/corrective.hpp>

#include <carto/core/diagnostic.hpp>
#include <carto/deform/detail/math_ops.hpp>

#include <algorithm>
#include <cmath>

namespace carto::deform {

namespace {

constexpr std::size_t kMaxStackDrivers = 256U;

[[nodiscard]] bool zero_vec(const core::Vec3d& value) noexcept {
    return value.x == 0.0 && value.y == 0.0 && value.z == 0.0;
}

[[nodiscard]] bool identity_basis(const core::Transform& basis) noexcept {
    return zero_vec(basis.translation) && basis.rotation.x == 0.0 &&
        basis.rotation.y == 0.0 && basis.rotation.z == 0.0 && basis.rotation.w == 1.0 &&
        basis.scale.x == 1.0 && basis.scale.y == 1.0 && basis.scale.z == 1.0;
}

core::Result<double> resolve_input(
    const character::DriverInput& input,
    const DriverChannelValues& channels) {
    switch (input.kind) {
    case character::DriverInput::Kind::control: {
        const auto found = channels.controls.find(input.control);
        if (found == channels.controls.end()) {
            return core::Result<double>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "corrective driver control channel is not posed"));
        }
        return core::Result<double>::success(found->second);
    }
    case character::DriverInput::Kind::pose_parameter: {
        const auto found = channels.pose_parameters.find(input.pose_parameter);
        if (found == channels.pose_parameters.end()) {
            return core::Result<double>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "corrective driver pose parameter is not posed"));
        }
        return core::Result<double>::success(found->second);
    }
    case character::DriverInput::Kind::driver: {
        const auto found = channels.drivers.find(input.driver);
        if (found == channels.drivers.end()) {
            return core::Result<double>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "chained corrective driver value is not available"));
        }
        return core::Result<double>::success(found->second);
    }
    }
    return core::Result<double>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, "unknown driver input kind"));
}

[[nodiscard]] double interpolate_factor(
    character::DriverInterpolation interpolation,
    double normalized) noexcept {
    const double clamped = std::min(1.0, std::max(0.0, normalized));
    switch (interpolation) {
    case character::DriverInterpolation::linear:
        return clamped;
    case character::DriverInterpolation::smoothstep:
        return clamped * clamped * (3.0 - 2.0 * clamped);
    }
    return clamped;
}

} // namespace

core::Result<DriverEvaluationResult> evaluate_driver(
    const character::CorrectiveDriver& driver,
    const DriverChannelValues& channels) {
    const core::Result<void> driver_valid = driver.validate();
    if (!driver_valid) {
        return core::Result<DriverEvaluationResult>::failure(driver_valid.error());
    }
    const std::size_t input_count = driver.inputs.size();
    if (driver.domain_minimum.size() != input_count ||
        driver.domain_maximum.size() != input_count) {
        return core::Result<DriverEvaluationResult>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "corrective driver domain arrays must follow the input order"));
    }
    std::vector<double> values(input_count, 0.0);
    std::uint64_t clamped_inputs = 0U;
    for (std::size_t index = 0U; index < input_count; ++index) {
        const character::DriverInput& input = driver.inputs[index];
        if (input.kind == character::DriverInput::Kind::driver &&
            input.driver == driver.id) {
            return core::Result<DriverEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::cycle_detected,
                "corrective driver may not reference itself"));
        }
        const double domain_min = driver.domain_minimum[index];
        const double domain_max = driver.domain_maximum[index];
        if (!std::isfinite(domain_min) || !std::isfinite(domain_max) ||
            !(domain_min < domain_max)) {
            return core::Result<DriverEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "corrective driver domains must be finite and ordered"));
        }
        const core::Result<double> resolved = resolve_input(input, channels);
        if (!resolved) {
            return core::Result<DriverEvaluationResult>::failure(resolved.error());
        }
        double value = resolved.value();
        if (!std::isfinite(value)) {
            return core::Result<DriverEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "corrective driver input channel must be finite"));
        }
        if (value < domain_min) {
            value = domain_min;
            ++clamped_inputs;
        } else if (value > domain_max) {
            value = domain_max;
            ++clamped_inputs;
        }
        values[index] = value;
    }
    if (input_count == 3U && !identity_basis(driver.basis)) {
        if (!zero_vec(driver.basis.translation)) {
            return core::Result<DriverEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "corrective driver basis must be a pure linear map"));
        }
        const core::Vec3d rotated = driver.basis.rotation.normalized().rotate(
            core::componentwise_multiply(
                driver.basis.scale, {values[0U], values[1U], values[2U]}));
        values[0U] = rotated.x;
        values[1U] = rotated.y;
        values[2U] = rotated.z;
    } else if (input_count != 3U && !identity_basis(driver.basis)) {
        return core::Result<DriverEvaluationResult>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "corrective driver basis transforms are honored for 3-input drivers only"));
    }
    double weight = 1.0;
    for (std::size_t index = 0U; index < input_count; ++index) {
        const double normalized =
            (values[index] - driver.domain_minimum[index]) /
            (driver.domain_maximum[index] - driver.domain_minimum[index]);
        weight *= interpolate_factor(driver.interpolation, normalized);
    }
    DriverEvaluationResult result;
    result.weight = std::min(1.0, std::max(0.0, weight));
    result.clamped_inputs = clamped_inputs;
    return core::Result<DriverEvaluationResult>::success(result);
}

core::Result<std::map<character::CorrectiveDriverId, DriverEvaluationResult>>
evaluate_driver_stack(
    std::span<const character::CorrectiveDriver* const> drivers,
    const DriverChannelValues& channels) {
    if (drivers.empty() || drivers.size() > kMaxStackDrivers) {
        return core::Result<
            std::map<character::CorrectiveDriverId, DriverEvaluationResult>>::failure(
            core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "corrective driver stack must be 1..256 drivers"));
    }
    std::vector<const character::CorrectiveDriver*> ordered(drivers.begin(), drivers.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->order_index < right->order_index;
    });
    std::map<character::CorrectiveDriverId, DriverEvaluationResult> results;
    DriverChannelValues resolved_channels = channels;
    std::uint32_t previous_order = 0U;
    bool have_previous = false;
    for (const character::CorrectiveDriver* driver : ordered) {
        if (driver == nullptr) {
            return core::Result<
                std::map<character::CorrectiveDriverId, DriverEvaluationResult>>::failure(
                core::Diagnostic(
                    core::ErrorCode::invalid_argument,
                    "corrective driver stack contains a null driver"));
        }
        if (results.find(driver->id) != results.end()) {
            return core::Result<
                std::map<character::CorrectiveDriverId, DriverEvaluationResult>>::failure(
                core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "corrective driver stack contains a duplicate driver id"));
        }
        if (have_previous && driver->order_index == previous_order) {
            return core::Result<
                std::map<character::CorrectiveDriverId, DriverEvaluationResult>>::failure(
                core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "corrective driver order indexes must be unique"));
        }
        previous_order = driver->order_index;
        have_previous = true;
        const core::Result<DriverEvaluationResult> evaluated =
            evaluate_driver(*driver, resolved_channels);
        if (!evaluated) {
            return core::Result<
                std::map<character::CorrectiveDriverId, DriverEvaluationResult>>::failure(
                evaluated.error());
        }
        resolved_channels.drivers[driver->id] = evaluated.value().weight;
        results.emplace(driver->id, evaluated.value());
    }
    return core::Result<
        std::map<character::CorrectiveDriverId, DriverEvaluationResult>>::success(
        std::move(results));
}

} // namespace carto::deform
