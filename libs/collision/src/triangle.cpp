#include <carto/collision/triangle.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <span>
#include <utility>

namespace carto::collision {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' && byte != '\r' &&
            byte != '\n';
    });
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

} // namespace

core::Result<void> TriangleCollisionCandidate::validate() const {
    if (!safe_text(source_identity, 256U)) {
        return core::Result<void>::failure(invalid(
            "triangle collision source identity is invalid"));
    }
    if (hierarchy_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "triangle collision candidate must bind its donor hierarchy digest"));
    }
    if (bound_pages.empty() || bound_pages.size() > kMaxBoundPages) {
        return core::Result<void>::failure(validation(
            "triangle collision candidate must bind a bounded non-empty page set"));
    }
    if (!std::is_sorted(bound_pages.begin(), bound_pages.end()) ||
        std::adjacent_find(bound_pages.begin(), bound_pages.end()) != bound_pages.end()) {
        return core::Result<void>::failure(validation(
            "triangle collision bound pages must be sorted and unique"));
    }
    if (triangle_count == 0U) {
        return core::Result<void>::failure(validation(
            "triangle collision candidate must name its triangle count"));
    }
    if (geometric_error_evidence < 0.0 ||
        !std::isfinite(geometric_error_evidence)) {
        return core::Result<void>::failure(validation(
            "triangle collision geometric error evidence must be finite and "
            "non-negative"));
    }
    if (candidate_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "triangle collision candidate must be digest-bound via refresh_digest"));
    }
    return core::Result<void>::success();
}

std::string TriangleCollisionCandidate::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "TRIANGLE SOURCE " << std::quoted(source_identity)
           << " REVISION " << source_revision.value()
           << " HIERARCHY_DIGEST " << hierarchy_digest.hex()
           << " TRIANGLES " << triangle_count
           << " ERROR " << geometric_error_evidence
           << " PAGES " << bound_pages.size();
    for (const web_geometry::RuntimePageId page : bound_pages) {
        output << ' ' << page;
    }
    return output.str();
}

std::string TriangleCollisionCandidate::canonical() const {
    return canonical_without_digest() + " DIGEST " + candidate_digest.hex();
}

core::Result<void> TriangleCollisionCandidate::refresh_digest() {
    candidate_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<TriangleCollisionCandidate> bind_triangle_collision_candidate(
    std::string source_identity,
    core::Revision source_revision,
    const web_geometry::ClusterHierarchy& hierarchy) {
    if (!safe_text(source_identity, 256U)) {
        return core::Result<TriangleCollisionCandidate>::failure(invalid(
            "triangle collision source identity is invalid"));
    }
    if (auto result = hierarchy.validate(); !result) {
        return core::Result<TriangleCollisionCandidate>::failure(
            result.error().with_context("triangle donor hierarchy is invalid"));
    }
    if (hierarchy.digest.is_zero()) {
        return core::Result<TriangleCollisionCandidate>::failure(validation(
            "triangle donor hierarchy must be digest-bound"));
    }

    TriangleCollisionCandidate candidate;
    candidate.source_identity = std::move(source_identity);
    candidate.source_revision = source_revision;
    candidate.hierarchy_digest = hierarchy.digest;
    double worst_error = 0.0;
    bool any_assigned = false;
    for (const web_geometry::ClusterRecord& cluster : hierarchy.clusters) {
        if (cluster.page == web_geometry::kUnassignedRuntimePage) {
            continue;
        }
        any_assigned = true;
        candidate.bound_pages.push_back(cluster.page);
        candidate.triangle_count += cluster.triangle_count;
        worst_error = std::max(worst_error, cluster.geometric_error);
    }
    if (!any_assigned) {
        return core::Result<TriangleCollisionCandidate>::failure(validation(
            "triangle donor hierarchy assigns no runtime pages to bind"));
    }
    std::sort(candidate.bound_pages.begin(), candidate.bound_pages.end());
    candidate.bound_pages.erase(
        std::unique(candidate.bound_pages.begin(), candidate.bound_pages.end()),
        candidate.bound_pages.end());
    candidate.geometric_error_evidence = worst_error;
    static_cast<void>(candidate.refresh_digest());
    return core::Result<TriangleCollisionCandidate>::success(std::move(candidate));
}

} // namespace carto::collision
