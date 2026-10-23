#pragma once

#include <carto/character/ids.hpp>

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::character {

enum class DriverInterpolation : std::uint8_t {
    linear,
    smoothstep,
};

[[nodiscard]] std::string_view driver_interpolation_name(
    DriverInterpolation interpolation) noexcept;

// One scalar input of a corrective driver: either a control channel or a
// named pose parameter, or another corrective driver (drivers may chain).
// Exactly one alternative is set; the enumeration keeps canonical ordering
// deterministic.
struct DriverInput {
    enum class Kind : std::uint8_t {
        control,
        pose_parameter,
        driver,
    };

    Kind kind = Kind::control;
    ControlId control;
    std::string pose_parameter;
    CorrectiveDriverId driver;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const DriverInput&) const noexcept = default;
    [[nodiscard]] bool operator<(const DriverInput&) const noexcept;
};

// One corrective driver: a deterministic scalar -> activation mapping over
// declared inputs with a bounded signed domain, an explicit basis transform,
// a fixed interpolation, and a stable evaluation order. Self-reference is
// forbidden: a driver may never appear in its own input set, directly or the
// chain is rejected by graph-level validation downstream.
struct CorrectiveDriver {
    static constexpr std::size_t kMaxInputs = 16U;
    static constexpr std::size_t kMaxPoseParameterBytes = 128U;

    CorrectiveDriverId id;
    // Sorted and unique inputs; domain arrays follow the same order.
    std::vector<DriverInput> inputs;
    std::vector<double> domain_minimum;
    std::vector<double> domain_maximum;
    core::Transform basis = core::Transform::identity();
    DriverInterpolation interpolation = DriverInterpolation::linear;
    // Stable evaluation order within one corrective stack; duplicates are
    // rejected so ordering is total.
    std::uint32_t order_index = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::character
