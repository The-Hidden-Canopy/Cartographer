#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace carto::deform {

// Deviation of one evaluated point stream against a reference stream, in
// model-space Euclidean distance. This is measured evidence, never a pass/fail
// verdict: consumers decide what magnitude is acceptable for their tier.
struct DeviationSummary {
    double max_deviation = 0.0;
    double mean_deviation = 0.0;
    std::uint64_t compared_points = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<DeviationSummary> compute_deviation(
    std::span<const core::Vec3d> evaluated,
    std::span<const core::Vec3d> reference);

// Content digest over a canonical point stream: the little-endian IEEE-754
// bytes of every component in index order. Equal streams digest equally.
[[nodiscard]] assets::Sha256Digest positions_digest(
    std::span<const core::Vec3d> positions);

// One evaluated stage of a deformation stack: a deterministic label, the
// digest of the point stream the stage produced, and the digest of the policy
// record(s) that governed it (binding, morph target, driver).
struct StageReceipt {
    std::string label;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest policy_digest;

    [[nodiscard]] core::Result<void> validate() const;
};

// Receipt for one full deformation graph evaluation. Deviation against a
// reference evaluation is populated exactly when the caller supplied one;
// otherwise the evaluation is reported without a deviation claim.
struct DeformationReceipt {
    static constexpr std::size_t kMaxStages = 64U;

    std::vector<StageReceipt> stages;
    bool has_reference_deviation = false;
    DeviationSummary reference_deviation{};

    [[nodiscard]] core::Result<void> validate() const;
};

} // namespace carto::deform
