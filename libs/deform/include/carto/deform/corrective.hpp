#pragma once

#include <carto/character/drivers.hpp>
#include <carto/character/ids.hpp>

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace carto::deform {

// Runtime channel values for corrective driver evaluation. Controls and pose
// parameters come from the posed character; chained driver values must have
// been evaluated earlier in the same stack pass.
struct DriverChannelValues {
    std::map<character::ControlId, double> controls;
    std::map<std::string, double> pose_parameters;
    std::map<character::CorrectiveDriverId, double> drivers;
};

struct DriverEvaluationResult {
    // Activation in [0, 1] after domain clamping, basis transform and the
    // declared interpolation; multi-input drivers combine as a product of
    // per-input factors.
    double weight = 0.0;
    std::uint64_t clamped_inputs = 0U;
};

// Evaluate one corrective driver over its declared signed bounded domain.
// Inputs resolve from the supplied channels, are clamped into the declared
// domain (clamps are counted, never silent), and self-reference is rejected
// as a cycle. A non-identity basis transform is honored exactly for
// three-input drivers; anything else is an explicit unsupported refusal.
[[nodiscard]] core::Result<DriverEvaluationResult> evaluate_driver(
    const character::CorrectiveDriver& driver,
    const DriverChannelValues& channels);

// Evaluate a corrective stack in declared order. Ids and order indexes must
// be unique (total ordering), chained DriverInput::driver references resolve
// against results already computed in this pass, and missing channels fail
// closed. The returned map iterates in evaluation order.
[[nodiscard]] core::Result<std::map<character::CorrectiveDriverId, DriverEvaluationResult>>
evaluate_driver_stack(
    std::span<const character::CorrectiveDriver* const> drivers,
    const DriverChannelValues& channels);

} // namespace carto::deform
