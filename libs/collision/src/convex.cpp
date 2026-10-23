#include <carto/collision/convex.hpp>

#include <carto/collision/metrics.hpp>

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

std::string_view convex_proxy_capability_name(
    ConvexProxyCapability capability) noexcept {
    switch (capability) {
    case ConvexProxyCapability::unsupported: return "unsupported";
    case ConvexProxyCapability::cpu_reference: return "cpu_reference";
    }
    return "unknown";
}

std::string_view convex_proxy_status_name(ConvexProxyStatus status) noexcept {
    switch (status) {
    case ConvexProxyStatus::capability_blocked: return "capability_blocked";
    case ConvexProxyStatus::admitted_reference_evidence: return "admitted_reference_evidence";
    }
    return "unknown";
}

core::Result<void> ConvexProxyCandidate::validate() const {
    if (!safe_text(source_identity, 256U)) {
        return core::Result<void>::failure(invalid(
            "convex proxy source identity is invalid"));
    }
    if (source_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "convex proxy must bind its source digest"));
    }
    switch (status) {
    case ConvexProxyStatus::capability_blocked:
        if (!hull_vertices.empty() || one_sided_hausdorff_evidence != 0.0 ||
            volume_residual_evidence != 0.0) {
            return core::Result<void>::failure(validation(
                "capability-blocked convex proxy must not carry hull evidence"));
        }
        break;
    case ConvexProxyStatus::admitted_reference_evidence:
        if (hull_vertices.empty()) {
            return core::Result<void>::failure(validation(
                "admitted convex proxy must carry donor hull vertices"));
        }
        break;
    default:
        return core::Result<void>::failure(invalid(
            "convex proxy status is invalid"));
    }
    if (hull_vertices.size() > kMaxHullVertices) {
        return core::Result<void>::failure(validation(
            "convex proxy hull exceeds its bounded vertex count"));
    }
    for (const core::Vec3d& vertex : hull_vertices) {
        if (!vertex.finite()) {
            return core::Result<void>::failure(validation(
                "convex proxy hull vertices must be finite"));
        }
    }
    if (one_sided_hausdorff_evidence < 0.0 ||
        !std::isfinite(one_sided_hausdorff_evidence) ||
        volume_residual_evidence < 0.0 ||
        !std::isfinite(volume_residual_evidence)) {
        return core::Result<void>::failure(validation(
            "convex proxy evidence terms must be finite and non-negative"));
    }
    if (candidate_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "convex proxy must be digest-bound via refresh_digest"));
    }
    return core::Result<void>::success();
}

std::string ConvexProxyCandidate::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "CONVEX SOURCE " << std::quoted(source_identity)
           << " REVISION " << source_revision.value()
           << " SOURCE_DIGEST " << source_digest.hex()
           << " STATUS " << convex_proxy_status_name(status)
           << " OUTSIDE " << one_sided_hausdorff_evidence
           << " VOLUME_RESIDUAL " << volume_residual_evidence
           << " HULL_VERTICES " << hull_vertices.size();
    for (const core::Vec3d& vertex : hull_vertices) {
        output << ' ' << vertex.x << ' ' << vertex.y << ' ' << vertex.z;
    }
    return output.str();
}

std::string ConvexProxyCandidate::canonical() const {
    return canonical_without_digest() + " DIGEST " + candidate_digest.hex();
}

core::Result<void> ConvexProxyCandidate::refresh_digest() {
    candidate_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<ConvexProxyCandidate> admit_convex_proxy_candidate(
    std::string source_identity,
    core::Revision source_revision,
    assets::Sha256Digest source_digest,
    std::vector<core::Vec3d> hull_vertices,
    const geometry::CompiledMesh& mesh,
    ConvexProxyCapability capability) {
    if (capability == ConvexProxyCapability::unsupported) {
        return core::Result<ConvexProxyCandidate>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "convex proxy collision remains a roadmap item and the target "
            "capability is unsupported; no default representation was "
            "substituted"));
    }
    if (capability != ConvexProxyCapability::cpu_reference) {
        return core::Result<ConvexProxyCandidate>::failure(invalid(
            "convex proxy capability value is invalid"));
    }
    if (!mesh.valid() || mesh.positions.empty()) {
        return core::Result<ConvexProxyCandidate>::failure(invalid(
            "convex proxy admission requires a valid compiled mesh"));
    }
    if (hull_vertices.empty() || hull_vertices.size() > ConvexProxyCandidate::kMaxHullVertices) {
        return core::Result<ConvexProxyCandidate>::failure(validation(
            "convex proxy hull must be non-empty and bounded"));
    }
    for (const core::Vec3d& vertex : hull_vertices) {
        if (!vertex.finite()) {
            return core::Result<ConvexProxyCandidate>::failure(validation(
                "convex proxy hull vertices must be finite"));
        }
    }

    ConvexProxyCandidate candidate;
    candidate.source_identity = std::move(source_identity);
    candidate.source_revision = source_revision;
    candidate.source_digest = source_digest;
    candidate.status = ConvexProxyStatus::admitted_reference_evidence;
    candidate.hull_vertices = std::move(hull_vertices);
    // Conservative reference evidence only: one-sided Hausdorff from source
    // vertices to the donor hull vertex set, and volume residual against the
    // hull bounding volume. Neither claims native realization.
    candidate.one_sided_hausdorff_evidence = one_sided_hausdorff(
        mesh.positions, candidate.hull_vertices);
    core::Bounds3d hull_bounds;
    for (const core::Vec3d& vertex : candidate.hull_vertices) {
        hull_bounds.include(vertex);
    }
    const core::Vec3d extents{
        hull_bounds.maximum.x - hull_bounds.minimum.x,
        hull_bounds.maximum.y - hull_bounds.minimum.y,
        hull_bounds.maximum.z - hull_bounds.minimum.z,
    };
    const double hull_volume = extents.x * extents.y * extents.z;
    auto source_volume = mesh_volume(mesh);
    if (!source_volume) {
        return core::Result<ConvexProxyCandidate>::failure(source_volume.error());
    }
    candidate.volume_residual_evidence =
        std::abs(source_volume.value() - hull_volume);
    static_cast<void>(candidate.refresh_digest());
    return core::Result<ConvexProxyCandidate>::success(std::move(candidate));
}

} // namespace carto::collision
