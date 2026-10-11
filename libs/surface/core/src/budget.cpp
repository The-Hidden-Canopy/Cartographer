#include <carto/surface/budget.hpp>

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

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

bool exceeds(std::uint64_t current, std::uint64_t additional, std::uint64_t limit) {
    return limit != 0U && (additional > limit || current > limit - additional);
}

bool exceeds(double current, double additional, double limit) {
    const double total = current + additional;
    return !std::isfinite(total) || (limit != 0.0 && total > limit);
}

bool checked_add(std::uint64_t current, std::uint64_t additional, std::uint64_t& result) {
    if (additional > std::numeric_limits<std::uint64_t>::max() - current) return false;
    result = current + additional;
    return true;
}

double normalized_cost(const FidelityOption& option) {
    constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
    return 1.0 + option.estimated_runtime_ms / 16.666666666666668 +
        option.estimated_cook_seconds + static_cast<double>(option.bytes) / bytes_per_megabyte;
}

bool canonical_option_less(const FidelityOption& left, const FidelityOption& right) {
    if (left.domain != right.domain) return left.domain < right.domain;
    if (left.representation != right.representation) {
        return left.representation < right.representation;
    }
    return left.option_id < right.option_id;
}

} // namespace

core::Result<void> FidelityBudget::validate() const {
    if (!finite_nonnegative(max_estimated_runtime_ms) ||
        !finite_nonnegative(max_cook_seconds)) {
        return core::Result<void>::failure(invalid("surface fidelity budget is invalid"));
    }
    return core::Result<void>::success();
}

std::string FidelityBudget::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-fidelity-budget-v1;package=" << max_package_bytes
           << ";geometry=" << max_geometry_bytes
           << ";closure=" << max_closure_bytes
           << ";runtime=" << max_estimated_runtime_ms
           << ";cook=" << max_cook_seconds;
    return output.str();
}

assets::Sha256Digest FidelityBudget::digest() const {
    return digest_text(canonical());
}

core::Result<void> FidelityOption::validate() const {
    if (option_id == 0U || !domain || !representation || source_revision.exhausted()) {
        return core::Result<void>::failure(invalid("surface fidelity option identity is invalid"));
    }
    if (!finite_nonnegative(expected_quality) || expected_quality > 1.0 ||
        !finite_nonnegative(estimated_runtime_ms) ||
        !finite_nonnegative(estimated_cook_seconds) ||
        bytes < geometry_bytes || bytes - geometry_bytes < closure_bytes) {
        return core::Result<void>::failure(invalid("surface fidelity option metrics are invalid"));
    }
    return core::Result<void>::success();
}

std::string FidelityOption::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "option=" << option_id
           << ";domain=" << domain.value
           << ";representation=" << representation.value
           << ";revision=" << source_revision.value()
           << ";quality=" << expected_quality
           << ";runtime=" << estimated_runtime_ms
           << ";cook=" << estimated_cook_seconds
           << ";bytes=" << bytes
           << ";geometry=" << geometry_bytes
           << ";closure=" << closure_bytes;
    return output.str();
}

core::Result<void> FidelityBudgetSelection::validate() const {
    if (auto result = budget.validate(); !result) return result;
    if (!finite_nonnegative(total_quality) || !finite_nonnegative(total_estimated_runtime_ms) ||
        !finite_nonnegative(total_cook_seconds)) {
        return core::Result<void>::failure(validation("surface fidelity selection totals are invalid"));
    }
    std::set<SurfaceDomainId> domains;
    std::set<std::uint64_t> option_ids;
    std::uint64_t bytes = 0U;
    std::uint64_t geometry_bytes = 0U;
    std::uint64_t closure_bytes = 0U;
    double quality = 0.0;
    double runtime = 0.0;
    double cook = 0.0;
    if (!std::is_sorted(selected.begin(), selected.end(), canonical_option_less)) {
        return core::Result<void>::failure(
            validation("surface fidelity selection is not in canonical order"));
    }
    for (const FidelityOption& option : selected) {
        if (auto result = option.validate(); !result) return result;
        if (option.source_revision != source_revision) {
            return core::Result<void>::failure(
                validation("surface fidelity selection mixes source revisions"));
        }
        if (!domains.insert(option.domain).second || !option_ids.insert(option.option_id).second) {
            return core::Result<void>::failure(
                validation("surface fidelity selection contains duplicate options"));
        }
        if (!checked_add(bytes, option.bytes, bytes) ||
            !checked_add(geometry_bytes, option.geometry_bytes, geometry_bytes) ||
            !checked_add(closure_bytes, option.closure_bytes, closure_bytes)) {
            return core::Result<void>::failure(
                validation("surface fidelity selection byte totals overflow"));
        }
        quality += option.expected_quality;
        runtime += option.estimated_runtime_ms;
        cook += option.estimated_cook_seconds;
    }
    if (bytes != total_bytes || geometry_bytes != total_geometry_bytes ||
        closure_bytes != total_closure_bytes || quality != total_quality ||
        runtime != total_estimated_runtime_ms || cook != total_cook_seconds ||
        exceeds(0U, bytes, budget.max_package_bytes) ||
        exceeds(0U, geometry_bytes, budget.max_geometry_bytes) ||
        exceeds(0U, closure_bytes, budget.max_closure_bytes) ||
        exceeds(0.0, runtime, budget.max_estimated_runtime_ms) ||
        exceeds(0.0, cook, budget.max_cook_seconds)) {
        return core::Result<void>::failure(validation("surface fidelity selection exceeds its budget"));
    }
    if (!assets::Sha256Digest::from_hex(digest) ||
        digest != digest_text(canonical_without_digest()).hex()) {
        return core::Result<void>::failure(validation("surface fidelity selection digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string FidelityBudgetSelection::canonical_without_digest() const {
    std::ostringstream output;
    output << budget.canonical() << ";revision=" << source_revision.value() << ";selected=";
    for (const FidelityOption& option : selected) output << option.canonical() << ';';
    output << std::setprecision(17)
           << ";quality=" << total_quality
           << ";runtime=" << total_estimated_runtime_ms
           << ";cook=" << total_cook_seconds
           << ";bytes=" << total_bytes
           << ";geometry_bytes=" << total_geometry_bytes
           << ";closure_bytes=" << total_closure_bytes;
    return output.str();
}

core::Result<void> FidelityBudgetSelection::refresh_digest() {
    if (auto result = budget.validate(); !result) return result;
    digest = digest_text(canonical_without_digest()).hex();
    return core::Result<void>::success();
}

core::Result<FidelityBudgetSelection> select_fidelity_options(
    const FidelityBudget& budget,
    const std::vector<FidelityOption>& options) {
    if (auto result = budget.validate(); !result) {
        return core::Result<FidelityBudgetSelection>::failure(result.error());
    }
    if (options.size() > FidelityBudgetSelection::max_options) {
        return core::Result<FidelityBudgetSelection>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                             "surface fidelity selector input reached its option limit"));
    }
    for (const FidelityOption& option : options) {
        if (auto result = option.validate(); !result) {
            return core::Result<FidelityBudgetSelection>::failure(result.error());
        }
    }

    std::vector<const FidelityOption*> ranked;
    ranked.reserve(options.size());
    for (const FidelityOption& option : options) ranked.push_back(&option);
    std::sort(ranked.begin(), ranked.end(), [](const FidelityOption* left,
                                               const FidelityOption* right) {
        const double left_priority = left->expected_quality / normalized_cost(*left);
        const double right_priority = right->expected_quality / normalized_cost(*right);
        if (left_priority != right_priority) return left_priority > right_priority;
        if (left->expected_quality != right->expected_quality) {
            return left->expected_quality > right->expected_quality;
        }
        if (left->domain != right->domain) return left->domain < right->domain;
        if (left->representation != right->representation) {
            return left->representation < right->representation;
        }
        return left->option_id < right->option_id;
    });

    FidelityBudgetSelection selection;
    selection.budget = budget;
    if (!options.empty()) selection.source_revision = options.front().source_revision;
    for (const FidelityOption& option : options) {
        if (option.source_revision != selection.source_revision) {
            return core::Result<FidelityBudgetSelection>::failure(
                validation("surface fidelity options mix source revisions"));
        }
    }
    std::set<SurfaceDomainId> selected_domains;
    for (const FidelityOption* option : ranked) {
        if (selected_domains.contains(option->domain)) continue;
        if (exceeds(selection.total_bytes, option->bytes, budget.max_package_bytes) ||
            exceeds(selection.total_geometry_bytes, option->geometry_bytes,
                    budget.max_geometry_bytes) ||
            exceeds(selection.total_closure_bytes, option->closure_bytes,
                    budget.max_closure_bytes) ||
            exceeds(selection.total_estimated_runtime_ms, option->estimated_runtime_ms,
                    budget.max_estimated_runtime_ms) ||
            exceeds(selection.total_cook_seconds, option->estimated_cook_seconds,
                    budget.max_cook_seconds)) {
            continue;
        }
        std::uint64_t next_bytes = 0U;
        std::uint64_t next_geometry_bytes = 0U;
        std::uint64_t next_closure_bytes = 0U;
        if (!checked_add(selection.total_bytes, option->bytes, next_bytes) ||
            !checked_add(selection.total_geometry_bytes, option->geometry_bytes,
                         next_geometry_bytes) ||
            !checked_add(selection.total_closure_bytes, option->closure_bytes,
                         next_closure_bytes)) {
            continue;
        }
        const double next_quality = selection.total_quality + option->expected_quality;
        const double next_runtime =
            selection.total_estimated_runtime_ms + option->estimated_runtime_ms;
        const double next_cook = selection.total_cook_seconds + option->estimated_cook_seconds;
        if (!finite_nonnegative(next_quality) || !finite_nonnegative(next_runtime) ||
            !finite_nonnegative(next_cook)) {
            continue;
        }
        selected_domains.insert(option->domain);
        selection.selected.push_back(*option);
        selection.total_quality = next_quality;
        selection.total_estimated_runtime_ms = next_runtime;
        selection.total_cook_seconds = next_cook;
        selection.total_bytes = next_bytes;
        selection.total_geometry_bytes = next_geometry_bytes;
        selection.total_closure_bytes = next_closure_bytes;
    }
    std::sort(selection.selected.begin(), selection.selected.end(), canonical_option_less);
    if (auto result = selection.refresh_digest(); !result) {
        return core::Result<FidelityBudgetSelection>::failure(result.error());
    }
    return core::Result<FidelityBudgetSelection>::success(std::move(selection));
}

} // namespace carto::surface
