#include <carto/surface/evidence.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
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

bool valid_digest(std::string_view value) {
    return static_cast<bool>(assets::Sha256Digest::from_hex(value));
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

bool sorted_unique_dependencies(const std::vector<SurfaceDependencyId>& dependencies) {
    return std::all_of(dependencies.begin(), dependencies.end(), [](SurfaceDependencyId id) {
               return static_cast<bool>(id);
           }) &&
        std::is_sorted(dependencies.begin(), dependencies.end()) &&
        std::adjacent_find(dependencies.begin(), dependencies.end()) == dependencies.end();
}

bool same_identity(const SurfaceEvidenceCase& evidence, const SurfaceEvidenceQuery& query) {
    return evidence.domain == query.domain &&
        evidence.representation == query.representation &&
        evidence.view_region.canonical() == query.view_region.canonical() &&
        evidence.light_region.canonical() == query.light_region.canonical();
}

bool current_for_query(const SurfaceEvidenceCase& evidence, const SurfaceEvidenceQuery& query) {
    return evidence.source_revision == query.source_revision &&
        evidence.source_content_digest == query.source_content_digest &&
        evidence.dependencies == query.dependencies;
}

} // namespace

core::Result<void> SurfaceRegion::validate() const {
    if (id == 0U || !direction.finite() || direction.length() <= 1e-12 ||
        !std::isfinite(spread) || spread < 0.0 || spread > 3.14159265358979323846 ||
        sample_count == 0U || sample_count > 1'000'000U) {
        return core::Result<void>::failure(invalid("surface evidence region is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceRegion::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << id << ':' << direction.x << ',' << direction.y << ',' << direction.z << ':'
           << spread << ':' << sample_count;
    return output.str();
}

core::Result<void> SurfaceEvidenceCase::validate() const {
    if (!id || !domain || !representation || source_revision.exhausted() ||
        !valid_digest(source_content_digest)) {
        return core::Result<void>::failure(invalid("surface evidence identity is invalid"));
    }
    if (auto result = view_region.validate(); !result) return result;
    if (auto result = light_region.validate(); !result) return result;
    if (auto result = measured_error.validate(); !result) return result;
    if (!finite_nonnegative(diagnostic_weight) || !sorted_unique_dependencies(dependencies)) {
        return core::Result<void>::failure(invalid("surface evidence metadata is invalid"));
    }
    if (!valid_digest(digest) || digest != digest_text(canonical_without_digest()).hex()) {
        return core::Result<void>::failure(validation("surface evidence digest is invalid"));
    }
    return core::Result<void>::success();
}

std::string SurfaceEvidenceCase::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "surface-evidence-v1;id=" << id.value
           << ";domain=" << domain.value
           << ";representation=" << representation.value
           << ";view=" << view_region.canonical()
           << ";light=" << light_region.canonical()
           << ";error=" << measured_error.max_position_error << ','
           << measured_error.max_normal_error << ','
           << measured_error.max_silhouette_error << ','
           << measured_error.rms_radiance_error << ','
           << measured_error.max_radiance_error << ','
           << measured_error.diffuse_error << ','
           << measured_error.specular_error << ','
           << measured_error.visibility_error << ','
           << measured_error.energy_error
           << ";dependencies=";
    for (const SurfaceDependencyId dependency : dependencies) output << dependency.value << ',';
    output << ";weight=" << diagnostic_weight
           << ";revision=" << source_revision.value()
           << ";source=" << source_content_digest;
    return output.str();
}

core::Result<void> SurfaceEvidenceCase::refresh_digest() {
    if (!id || !domain || !representation || source_revision.exhausted() ||
        !valid_digest(source_content_digest)) {
        return core::Result<void>::failure(invalid(
            "surface evidence cannot refresh from invalid identity"));
    }
    if (auto result = view_region.validate(); !result) return result;
    if (auto result = light_region.validate(); !result) return result;
    if (auto result = measured_error.validate(); !result) return result;
    if (!finite_nonnegative(diagnostic_weight) || !sorted_unique_dependencies(dependencies)) {
        return core::Result<void>::failure(invalid("surface evidence metadata is invalid"));
    }
    digest = digest_text(canonical_without_digest()).hex();
    return core::Result<void>::success();
}

core::Result<void> SurfaceEvidenceQuery::validate() const {
    if (!domain || !representation || source_revision.exhausted() ||
        !valid_digest(source_content_digest)) {
        return core::Result<void>::failure(invalid("surface evidence query identity is invalid"));
    }
    if (auto result = view_region.validate(); !result) return result;
    if (auto result = light_region.validate(); !result) return result;
    if (!sorted_unique_dependencies(dependencies)) {
        return core::Result<void>::failure(invalid("surface evidence query dependencies are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> SurfaceEvidenceReservoir::append(SurfaceEvidenceCase evidence) {
    if (records_.size() >= max_cases) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::unsupported,
                             "surface evidence reservoir reached its bounded case limit"));
    }
    if (auto result = evidence.validate(); !result) return result;
    records_.push_back(std::move(evidence));
    return core::Result<void>::success();
}

core::Result<std::optional<SurfaceEvidenceCase>> SurfaceEvidenceReservoir::query(
    const SurfaceEvidenceQuery& query_value) const {
    if (auto result = query_value.validate(); !result) {
        return core::Result<std::optional<SurfaceEvidenceCase>>::failure(result.error());
    }
    for (auto iterator = records_.rbegin(); iterator != records_.rend(); ++iterator) {
        if (!same_identity(*iterator, query_value)) continue;
        if (!current_for_query(*iterator, query_value)) {
            return core::Result<std::optional<SurfaceEvidenceCase>>::success(std::nullopt);
        }
        return core::Result<std::optional<SurfaceEvidenceCase>>::success(*iterator);
    }
    return core::Result<std::optional<SurfaceEvidenceCase>>::success(std::nullopt);
}

core::Result<void> SurfaceEvidenceReservoir::validate() const {
    if (records_.size() > max_cases) {
        return core::Result<void>::failure(validation("surface evidence reservoir exceeds its case limit"));
    }
    for (const SurfaceEvidenceCase& evidence : records_) {
        if (auto result = evidence.validate(); !result) return result;
    }
    return core::Result<void>::success();
}

} // namespace carto::surface
