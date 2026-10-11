#include <carto/surface/candidate.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <span>
#include <utility>

namespace carto::surface {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_digest(std::string_view value) {
    return static_cast<bool>(assets::Sha256Digest::from_hex(value));
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

double normalized_cost(const SurfaceWorkCandidate& candidate) {
    constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
    return 1.0 + candidate.estimated_cook_cost +
        candidate.estimated_runtime_cost / 1000.0 +
        static_cast<double>(candidate.estimated_bytes) / bytes_per_megabyte;
}

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool over_budget(std::uint64_t current, std::uint64_t additional, std::uint64_t limit) {
    if (additional > std::numeric_limits<std::uint64_t>::max() - current) return true;
    return limit != 0U && current + additional > limit;
}

bool valid_surface_work_kind(SurfaceWorkKind kind) {
    switch (kind) {
    case SurfaceWorkKind::collapse_edge:
    case SurfaceWorkKind::split_edge:
    case SurfaceWorkKind::relocate_vertex:
    case SurfaceWorkKind::preserve_feature:
    case SurfaceWorkKind::add_displacement_residual:
    case SurfaceWorkKind::increase_ndf_order:
    case SurfaceWorkKind::increase_visibility_order:
    case SurfaceWorkKind::refine_silhouette:
    case SurfaceWorkKind::evaluate_sample:
        return true;
    }
    return false;
}

bool valid_stop_reason(SurfaceOptimizationStopReason reason) {
    switch (reason) {
    case SurfaceOptimizationStopReason::quality_target:
    case SurfaceOptimizationStopReason::iteration_cap:
    case SurfaceOptimizationStopReason::budget_exhausted:
    case SurfaceOptimizationStopReason::no_positive_gain:
        return true;
    }
    return false;
}

core::Result<void> validate_plan_payload(const SurfaceCandidatePlan& plan) {
    if (!plan.domain || plan.source_revision.exhausted()) {
        return core::Result<void>::failure(validation("surface candidate plan identity is invalid"));
    }
    if (!valid_stop_reason(plan.stop_reason)) {
        return core::Result<void>::failure(
            validation("surface candidate plan stop reason is invalid"));
    }
    if (auto result = plan.options.validate(); !result) return result;
    if (!finite_nonnegative(plan.initial_error) || !finite_nonnegative(plan.final_error) ||
        plan.final_error > plan.initial_error || plan.initial_error != plan.options.initial_error) {
        return core::Result<void>::failure(validation("surface candidate plan error is invalid"));
    }
    if (plan.steps.size() > plan.options.max_iterations) {
        return core::Result<void>::failure(
            validation("surface candidate plan exceeds its iteration budget"));
    }

    std::set<std::uint64_t> ids;
    std::uint64_t previous_bytes = 0U;
    double previous_error = plan.initial_error;
    double previous_cook = 0.0;
    double previous_runtime = 0.0;
    for (const SurfaceCandidateStep& step : plan.steps) {
        if (auto result = step.validate(); !result) return result;
        const auto utility = surface_work_utility(step.candidate);
        const auto priority = surface_work_priority(step.candidate);
        if (!utility || !priority || step.utility != utility.value() ||
            step.priority != priority.value()) {
            return core::Result<void>::failure(
                validation("surface candidate plan step score is not reproducible"));
        }
        if (step.candidate.domain != plan.domain ||
            step.candidate.source_revision != plan.source_revision ||
            !ids.insert(step.candidate.id).second) {
            return core::Result<void>::failure(
                validation("surface candidate plan steps are not bound to the plan"));
        }
        if (over_budget(previous_bytes, step.candidate.estimated_bytes, plan.options.max_bytes)) {
            return core::Result<void>::failure(
                validation("surface candidate plan byte budget is invalid"));
        }
        const std::uint64_t expected_bytes = previous_bytes + step.candidate.estimated_bytes;
        const double expected_cook = previous_cook + step.candidate.estimated_cook_cost;
        const double expected_runtime = previous_runtime + step.candidate.estimated_runtime_cost;
        const double expected_error = std::max(
            plan.options.target_error,
            previous_error - step.candidate.expected_error_reduction);
        if (step.cumulative_bytes != expected_bytes ||
            step.cumulative_cook_seconds != expected_cook ||
            step.cumulative_runtime_ms != expected_runtime ||
            step.remaining_error != expected_error ||
            (plan.options.max_cook_seconds != 0.0 && expected_cook > plan.options.max_cook_seconds) ||
            (plan.options.max_runtime_ms != 0.0 && expected_runtime > plan.options.max_runtime_ms)) {
            return core::Result<void>::failure(
                validation("surface candidate plan step totals are not reproducible"));
        }
        previous_bytes = expected_bytes;
        previous_error = expected_error;
        previous_cook = expected_cook;
        previous_runtime = expected_runtime;
    }
    if (plan.final_error != previous_error) {
        return core::Result<void>::failure(
            validation("surface candidate plan final error does not match its steps"));
    }
    switch (plan.stop_reason) {
    case SurfaceOptimizationStopReason::quality_target:
        if (plan.final_error > plan.options.target_error) {
            return core::Result<void>::failure(
                validation("surface candidate plan claims an unmet quality target"));
        }
        break;
    case SurfaceOptimizationStopReason::iteration_cap:
        if (plan.steps.size() != plan.options.max_iterations ||
            plan.final_error <= plan.options.target_error) {
            return core::Result<void>::failure(
                validation("surface candidate plan iteration stop is inconsistent"));
        }
        break;
    case SurfaceOptimizationStopReason::budget_exhausted:
    case SurfaceOptimizationStopReason::no_positive_gain:
        if (plan.steps.size() >= plan.options.max_iterations ||
            plan.final_error <= plan.options.target_error) {
            return core::Result<void>::failure(
                validation("surface candidate plan non-quality stop is inconsistent"));
        }
        break;
    }
    return core::Result<void>::success();
}

} // namespace

const char* surface_work_kind_name(SurfaceWorkKind kind) noexcept {
    switch (kind) {
    case SurfaceWorkKind::collapse_edge: return "collapse-edge";
    case SurfaceWorkKind::split_edge: return "split-edge";
    case SurfaceWorkKind::relocate_vertex: return "relocate-vertex";
    case SurfaceWorkKind::preserve_feature: return "preserve-feature";
    case SurfaceWorkKind::add_displacement_residual: return "add-displacement-residual";
    case SurfaceWorkKind::increase_ndf_order: return "increase-ndf-order";
    case SurfaceWorkKind::increase_visibility_order: return "increase-visibility-order";
    case SurfaceWorkKind::refine_silhouette: return "refine-silhouette";
    case SurfaceWorkKind::evaluate_sample: return "evaluate-sample";
    }
    return "unknown";
}

core::Result<void> SurfaceWorkCandidate::validate() const {
    if (id == 0U || !domain || source_revision.exhausted() || !valid_surface_work_kind(kind)) {
        return core::Result<void>::failure(invalid("surface work candidate identity is invalid"));
    }
    if (!finite_nonnegative(uncertainty) || uncertainty > 1.0 ||
        !finite_nonnegative(expected_error_reduction) ||
        !finite_nonnegative(semantic_importance) || semantic_importance > 1.0 ||
        !finite_nonnegative(silhouette_importance) || silhouette_importance > 1.0 ||
        !finite_nonnegative(estimated_cook_cost) ||
        !finite_nonnegative(estimated_runtime_cost)) {
        return core::Result<void>::failure(invalid("surface work candidate metrics are invalid"));
    }
    if ((!evidence_digest.empty() && !valid_digest(evidence_digest)) ||
        !valid_digest(digest) || digest != digest_text(canonical_without_digest()).hex()) {
        return core::Result<void>::failure(
            validation("surface work candidate digest or evidence identity is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceWorkCandidate::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-work-candidate-v1;id=" << id
           << ";domain=" << domain.value
           << ";revision=" << source_revision.value()
           << ";kind=" << surface_work_kind_name(kind)
           << ";uncertainty=" << uncertainty
           << ";reduction=" << expected_error_reduction
           << ";semantic=" << semantic_importance
           << ";silhouette=" << silhouette_importance
           << ";cook=" << estimated_cook_cost
           << ";runtime=" << estimated_runtime_cost
           << ";bytes=" << estimated_bytes
           << ";evidence=" << evidence_digest;
    return output.str();
}

core::Result<void> SurfaceWorkCandidate::refresh_digest() {
    if (id == 0U || !domain || source_revision.exhausted() || !valid_surface_work_kind(kind)) {
        return core::Result<void>::failure(invalid(
            "surface work candidate cannot refresh from invalid identity"));
    }
    if (!evidence_digest.empty() && !valid_digest(evidence_digest)) {
        return core::Result<void>::failure(invalid("surface work candidate evidence digest is invalid"));
    }
    digest = digest_text(canonical_without_digest()).hex();
    return core::Result<void>::success();
}

core::Result<void> SurfaceCandidateOptimizerOptions::validate() const {
    if (max_iterations == 0U || !finite_nonnegative(initial_error) ||
        !finite_nonnegative(target_error) || target_error > initial_error ||
        !finite_nonnegative(max_cook_seconds) || !finite_nonnegative(max_runtime_ms)) {
        return core::Result<void>::failure(invalid(
            "surface candidate optimizer options are invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceCandidateOptimizerOptions::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-candidate-options-v1;iterations=" << max_iterations
           << ";initial=" << initial_error
           << ";target=" << target_error
           << ";bytes=" << max_bytes
           << ";cook=" << max_cook_seconds
           << ";runtime=" << max_runtime_ms;
    return output.str();
}

assets::Sha256Digest SurfaceCandidateOptimizerOptions::digest() const {
    return digest_text(canonical());
}

core::Result<void> SurfaceCandidateStep::validate() const {
    if (auto result = candidate.validate(); !result) return result;
    if (!finite_nonnegative(utility) || !finite_nonnegative(priority) ||
        !finite_nonnegative(remaining_error) || !finite_nonnegative(cumulative_cook_seconds) ||
        !finite_nonnegative(cumulative_runtime_ms)) {
        return core::Result<void>::failure(validation("surface candidate step metrics are invalid"));
    }
    return core::Result<void>::success();
}

const char* surface_optimization_stop_reason_name(
    SurfaceOptimizationStopReason reason) noexcept {
    switch (reason) {
    case SurfaceOptimizationStopReason::quality_target: return "quality-target";
    case SurfaceOptimizationStopReason::iteration_cap: return "iteration-cap";
    case SurfaceOptimizationStopReason::budget_exhausted: return "budget-exhausted";
    case SurfaceOptimizationStopReason::no_positive_gain: return "no-positive-gain";
    }
    return "unknown";
}

core::Result<void> SurfaceCandidatePlan::validate() const {
    if (auto result = validate_plan_payload(*this); !result) return result;
    if (!valid_digest(digest) || digest != digest_text(canonical_without_digest()).hex()) {
        return core::Result<void>::failure(validation("surface candidate plan digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceCandidatePlan::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-candidate-plan-v1;domain=" << domain.value
           << ";revision=" << source_revision.value()
           << ";options=" << options.canonical()
           << ";initial=" << initial_error
           << ";final=" << final_error
           << ";stop=" << surface_optimization_stop_reason_name(stop_reason)
           << ";steps=";
    for (const SurfaceCandidateStep& step : steps) {
        output << step.candidate.digest << ':' << step.utility << ':' << step.priority << ':'
               << step.remaining_error << ':' << step.cumulative_bytes << ':'
               << step.cumulative_cook_seconds << ':' << step.cumulative_runtime_ms << ';';
    }
    return output.str();
}

core::Result<void> SurfaceCandidatePlan::refresh_digest() {
    if (auto result = validate_plan_payload(*this); !result) return result;
    digest = digest_text(canonical_without_digest()).hex();
    return core::Result<void>::success();
}

core::Result<double> surface_work_utility(const SurfaceWorkCandidate& candidate) {
    if (auto result = candidate.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    const double importance = std::max(candidate.semantic_importance,
                                       candidate.silhouette_importance);
    return core::Result<double>::success(
        importance * candidate.uncertainty * candidate.expected_error_reduction);
}

core::Result<double> surface_work_priority(const SurfaceWorkCandidate& candidate) {
    const auto utility = surface_work_utility(candidate);
    if (!utility) return utility;
    return core::Result<double>::success(utility.value() / normalized_cost(candidate));
}

core::Result<SurfaceCandidatePlan> plan_surface_work(
    SurfaceDomainId domain,
    core::Revision source_revision,
    const std::vector<SurfaceWorkCandidate>& candidates,
    const SurfaceCandidateOptimizerOptions& options) {
    if (!domain || source_revision.exhausted()) {
        return core::Result<SurfaceCandidatePlan>::failure(
            invalid("surface candidate planner identity is invalid"));
    }
    if (auto result = options.validate(); !result) {
        return core::Result<SurfaceCandidatePlan>::failure(result.error());
    }
    if (candidates.size() > SurfaceCandidateOptimizerOptions::max_candidates) {
        return core::Result<SurfaceCandidatePlan>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                             "surface candidate planner input reached its candidate limit"));
    }

    struct RankedCandidate {
        const SurfaceWorkCandidate* candidate = nullptr;
        double utility = 0.0;
        double priority = 0.0;
    };
    std::vector<RankedCandidate> ranked;
    ranked.reserve(candidates.size());
    std::set<std::uint64_t> ids;
    for (const SurfaceWorkCandidate& candidate : candidates) {
        if (auto result = candidate.validate(); !result) {
            return core::Result<SurfaceCandidatePlan>::failure(result.error());
        }
        if (candidate.domain != domain || candidate.source_revision != source_revision ||
            !ids.insert(candidate.id).second) {
            return core::Result<SurfaceCandidatePlan>::failure(
                validation("surface candidate does not match the planner identity"));
        }
        const auto utility = surface_work_utility(candidate);
        const auto priority = surface_work_priority(candidate);
        if (!utility || !priority) {
            return core::Result<SurfaceCandidatePlan>::failure(
                !utility ? utility.error() : priority.error());
        }
        ranked.push_back({&candidate, utility.value(), priority.value()});
    }
    std::sort(ranked.begin(), ranked.end(), [](const RankedCandidate& left,
                                               const RankedCandidate& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        if (left.utility != right.utility) return left.utility > right.utility;
        if (left.candidate->expected_error_reduction != right.candidate->expected_error_reduction) {
            return left.candidate->expected_error_reduction > right.candidate->expected_error_reduction;
        }
        if (left.candidate->kind != right.candidate->kind) {
            return static_cast<int>(left.candidate->kind) < static_cast<int>(right.candidate->kind);
        }
        return left.candidate->id < right.candidate->id;
    });

    SurfaceCandidatePlan plan;
    plan.domain = domain;
    plan.source_revision = source_revision;
    plan.options = options;
    plan.initial_error = options.initial_error;
    plan.final_error = options.initial_error;

    double remaining_error = options.initial_error;
    std::uint64_t bytes = 0U;
    double cook_seconds = 0.0;
    double runtime_ms = 0.0;
    std::size_t next = 0U;
    bool budget_blocked = false;
    while (plan.steps.size() < options.max_iterations && remaining_error > options.target_error) {
        const RankedCandidate* chosen = nullptr;
        for (; next < ranked.size(); ++next) {
            const RankedCandidate& candidate = ranked[next];
            if (candidate.utility <= 0.0 || candidate.priority <= 0.0) continue;
            const double next_cook = cook_seconds + candidate.candidate->estimated_cook_cost;
            const double next_runtime = runtime_ms + candidate.candidate->estimated_runtime_cost;
            if (over_budget(bytes, candidate.candidate->estimated_bytes, options.max_bytes) ||
                (options.max_cook_seconds != 0.0 && next_cook > options.max_cook_seconds) ||
                (options.max_runtime_ms != 0.0 && next_runtime > options.max_runtime_ms)) {
                budget_blocked = true;
                continue;
            }
            chosen = &candidate;
            ++next;
            break;
        }
        if (chosen == nullptr) {
            plan.stop_reason = budget_blocked
                ? SurfaceOptimizationStopReason::budget_exhausted
                : SurfaceOptimizationStopReason::no_positive_gain;
            break;
        }

        bytes += chosen->candidate->estimated_bytes;
        cook_seconds += chosen->candidate->estimated_cook_cost;
        runtime_ms += chosen->candidate->estimated_runtime_cost;
        remaining_error = std::max(
            options.target_error,
            remaining_error - chosen->candidate->expected_error_reduction);
        plan.steps.push_back({
            *chosen->candidate,
            chosen->utility,
            chosen->priority,
            remaining_error,
            bytes,
            cook_seconds,
            runtime_ms,
        });
    }
    plan.final_error = remaining_error;
    if (remaining_error <= options.target_error) {
        plan.stop_reason = SurfaceOptimizationStopReason::quality_target;
    } else if (plan.steps.size() >= options.max_iterations) {
        plan.stop_reason = SurfaceOptimizationStopReason::iteration_cap;
    }
    if (auto result = plan.refresh_digest(); !result) {
        return core::Result<SurfaceCandidatePlan>::failure(result.error());
    }
    return core::Result<SurfaceCandidatePlan>::success(std::move(plan));
}

} // namespace carto::surface
