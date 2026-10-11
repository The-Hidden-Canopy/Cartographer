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

enum class SurfaceWorkKind {
    collapse_edge,
    split_edge,
    relocate_vertex,
    preserve_feature,
    add_displacement_residual,
    increase_ndf_order,
    increase_visibility_order,
    refine_silhouette,
    evaluate_sample,
};

[[nodiscard]] const char* surface_work_kind_name(SurfaceWorkKind kind) noexcept;

struct SurfaceWorkCandidate {
    std::uint64_t id = 0U;
    SurfaceDomainId domain;
    core::Revision source_revision;
    SurfaceWorkKind kind = SurfaceWorkKind::evaluate_sample;
    double uncertainty = 0.0;
    double expected_error_reduction = 0.0;
    double semantic_importance = 0.0;
    double silhouette_importance = 0.0;
    double estimated_cook_cost = 0.0;
    double estimated_runtime_cost = 0.0;
    std::uint64_t estimated_bytes = 0U;
    std::string evidence_digest;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct SurfaceCandidateOptimizerOptions {
    static constexpr std::size_t max_candidates = 65'536U;

    std::uint32_t max_iterations = 32U;
    double initial_error = 1.0;
    double target_error = 0.0;
    // Zero means unbounded for the corresponding planner budget.
    std::uint64_t max_bytes = 0U;
    double max_cook_seconds = 0.0;
    double max_runtime_ms = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] assets::Sha256Digest digest() const;
};

struct SurfaceCandidateStep {
    SurfaceWorkCandidate candidate;
    double utility = 0.0;
    double priority = 0.0;
    double remaining_error = 0.0;
    std::uint64_t cumulative_bytes = 0U;
    double cumulative_cook_seconds = 0.0;
    double cumulative_runtime_ms = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class SurfaceOptimizationStopReason {
    quality_target,
    iteration_cap,
    budget_exhausted,
    no_positive_gain,
};

[[nodiscard]] const char* surface_optimization_stop_reason_name(
    SurfaceOptimizationStopReason reason) noexcept;

struct SurfaceCandidatePlan {
    SurfaceDomainId domain;
    core::Revision source_revision;
    SurfaceCandidateOptimizerOptions options;
    double initial_error = 0.0;
    double final_error = 0.0;
    SurfaceOptimizationStopReason stop_reason = SurfaceOptimizationStopReason::no_positive_gain;
    std::vector<SurfaceCandidateStep> steps;
    std::string digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

[[nodiscard]] core::Result<double> surface_work_utility(
    const SurfaceWorkCandidate& candidate);

[[nodiscard]] core::Result<double> surface_work_priority(
    const SurfaceWorkCandidate& candidate);

// Plans work; it does not mutate authored geometry or pretend to have applied
// a candidate. The selected steps are deterministic evidence for a later
// candidate builder/evaluator.
[[nodiscard]] core::Result<SurfaceCandidatePlan> plan_surface_work(
    SurfaceDomainId domain,
    core::Revision source_revision,
    const std::vector<SurfaceWorkCandidate>& candidates,
    const SurfaceCandidateOptimizerOptions& options = {});

} // namespace carto::surface
