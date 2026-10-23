#include <carto/deform/receipt.hpp>

#include <carto/core/diagnostic.hpp>

#include <cmath>
#include <cstring>

namespace carto::deform {

namespace {

template <typename T>
void append_value(std::vector<std::uint8_t>& bytes, const T& value) {
    const auto* raw = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

} // namespace

core::Result<void> DeviationSummary::validate() const {
    if (!std::isfinite(max_deviation) || !std::isfinite(mean_deviation) ||
        max_deviation < 0.0 || mean_deviation < 0.0) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "deviation summary carries non-finite or negative magnitudes"));
    }
    if (compared_points == 0U && (max_deviation != 0.0 || mean_deviation != 0.0)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "deviation summary reports magnitudes without compared points"));
    }
    return core::Result<void>::success();
}

core::Result<DeviationSummary> compute_deviation(
    std::span<const core::Vec3d> evaluated,
    std::span<const core::Vec3d> reference) {
    if (evaluated.size() != reference.size()) {
        return core::Result<DeviationSummary>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "deviation requires equal-length evaluated and reference streams"));
    }
    DeviationSummary summary;
    summary.compared_points = static_cast<std::uint64_t>(evaluated.size());
    long double accumulated = 0.0L;
    for (std::size_t index = 0U; index < evaluated.size(); ++index) {
        const core::Vec3d delta = evaluated[index] - reference[index];
        if (!delta.finite()) {
            return core::Result<DeviationSummary>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "deviation inputs must be finite"));
        }
        const double distance = delta.length();
        accumulated += static_cast<long double>(distance);
        if (distance > summary.max_deviation) {
            summary.max_deviation = distance;
        }
    }
    if (!evaluated.empty()) {
        summary.mean_deviation =
            static_cast<double>(accumulated / static_cast<long double>(evaluated.size()));
    }
    return core::Result<DeviationSummary>::success(summary);
}

assets::Sha256Digest positions_digest(std::span<const core::Vec3d> positions) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(positions.size() * 3U * sizeof(double));
    for (const core::Vec3d& position : positions) {
        append_value(bytes, position.x);
        append_value(bytes, position.y);
        append_value(bytes, position.z);
    }
    return assets::sha256(bytes);
}

core::Result<void> StageReceipt::validate() const {
    if (label.empty() || label.size() > 256U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "stage receipt label must be 1..256 bytes"));
    }
    if (policy_digest.is_zero()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "stage receipt requires a policy digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> DeformationReceipt::validate() const {
    if (stages.size() > kMaxStages) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "deformation receipt exceeds the stage bound"));
    }
    for (const StageReceipt& stage : stages) {
        const core::Result<void> valid = stage.validate();
        if (!valid) {
            return valid;
        }
    }
    if (has_reference_deviation) {
        return reference_deviation.validate();
    }
    return core::Result<void>::success();
}

} // namespace carto::deform
