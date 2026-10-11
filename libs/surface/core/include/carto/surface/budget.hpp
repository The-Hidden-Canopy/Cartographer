#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/surface/core.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::surface {

struct FidelityBudget {
    // Zero means unbounded for the corresponding budget dimension.
    std::uint64_t max_package_bytes = 0U;
    std::uint64_t max_geometry_bytes = 0U;
    std::uint64_t max_closure_bytes = 0U;
    double max_estimated_runtime_ms = 0.0;
    double max_cook_seconds = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct FidelityOption {
    std::uint64_t option_id = 0U;
    SurfaceDomainId domain;
    RepresentationId representation;
    core::Revision source_revision;
    double expected_quality = 0.0;
    double estimated_runtime_ms = 0.0;
    double estimated_cook_seconds = 0.0;
    std::uint64_t bytes = 0U;
    std::uint64_t geometry_bytes = 0U;
    std::uint64_t closure_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct FidelityBudgetSelection {
    static constexpr std::size_t max_options = 65'536U;

    FidelityBudget budget;
    core::Revision source_revision;
    std::vector<FidelityOption> selected;
    double total_quality = 0.0;
    double total_estimated_runtime_ms = 0.0;
    double total_cook_seconds = 0.0;
    std::uint64_t total_bytes = 0U;
    std::uint64_t total_geometry_bytes = 0U;
    std::uint64_t total_closure_bytes = 0U;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

// Deterministic greedy marginal-quality selection. At most one option is
// selected for a domain; all estimates remain estimates and are never
// reported as measured runtime or rendered quality.
[[nodiscard]] core::Result<FidelityBudgetSelection> select_fidelity_options(
    const FidelityBudget& budget,
    const std::vector<FidelityOption>& options);

} // namespace carto::surface
