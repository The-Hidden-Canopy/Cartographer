#include <carto/collision/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <span>
#include <sstream>

namespace carto::collision {
namespace {

constexpr double kPi = 3.14159265358979323846;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_term(double value) {
    return std::isfinite(value) && value >= 0.0;
}

double clamped(double value) {
    return value > 0.0 ? value : 0.0;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

// Conservative axis-aligned bounding volume used whenever an exact pairwise
// formula is unavailable (capsule/cylinder overlap contributions).
core::Bounds3d conservative_bounds(const PrimitiveCollisionCandidate& candidate) {
    core::Bounds3d bounds;
    switch (candidate.kind) {
    case PrimitiveCollisionKind::sphere:
        bounds.include(candidate.center - core::Vec3d{
            candidate.radius, candidate.radius, candidate.radius});
        bounds.include(candidate.center + core::Vec3d{
            candidate.radius, candidate.radius, candidate.radius});
        return bounds;
    case PrimitiveCollisionKind::box:
        bounds.include(candidate.center - candidate.half_extents);
        bounds.include(candidate.center + candidate.half_extents);
        return bounds;
    case PrimitiveCollisionKind::capsule:
    case PrimitiveCollisionKind::cylinder:
        // The fit axis is not stored on the candidate, so the conservative
        // fallback is the bounding sphere, which always contains the true
        // volume and never under-reports overlap.
        const double reach = candidate.half_height + candidate.radius;
        bounds.include(candidate.center - core::Vec3d{reach, reach, reach});
        bounds.include(candidate.center + core::Vec3d{reach, reach, reach});
        return bounds;
    }
    return bounds;
}

double bounds_overlap_volume(const core::Bounds3d& first, const core::Bounds3d& second) {
    const double x = clamped(std::min(first.maximum.x, second.maximum.x) -
        std::max(first.minimum.x, second.minimum.x));
    const double y = clamped(std::min(first.maximum.y, second.maximum.y) -
        std::max(first.minimum.y, second.minimum.y));
    const double z = clamped(std::min(first.maximum.z, second.maximum.z) -
        std::max(first.minimum.z, second.minimum.z));
    return x * y * z;
}

double center_distance(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second) {
    return (first.center - second.center).length();
}

double bounding_radius(const PrimitiveCollisionCandidate& candidate) {
    switch (candidate.kind) {
    case PrimitiveCollisionKind::sphere:
        return candidate.radius;
    case PrimitiveCollisionKind::box:
        return candidate.half_extents.length();
    case PrimitiveCollisionKind::capsule:
        return candidate.half_height + candidate.radius;
    case PrimitiveCollisionKind::cylinder:
        return std::hypot(candidate.half_height, candidate.radius);
    }
    return 0.0;
}

} // namespace

core::Result<void> CollisionErrorMetrics::validate() const {
    if (!valid_term(volume_overlap_residual) || !valid_term(max_penetration_depth) ||
        !valid_term(one_sided_hausdorff) || !valid_term(clearance_violation)) {
        return core::Result<void>::failure(validation(
            "collision error metric terms must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

std::string CollisionErrorMetrics::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "METRICS VOLUME_OVERLAP " << volume_overlap_residual
           << " PENETRATION " << max_penetration_depth
           << " HAUSDORFF " << one_sided_hausdorff
           << " CLEARANCE " << clearance_violation;
    return output.str();
}

core::Result<void> CollisionErrorThresholdPolicy::validate() const {
    const auto check = [](const std::optional<double>& bound, const char* label) {
        return !bound.has_value() || (std::isfinite(bound.value()) && bound.value() >= 0.0)
            ? core::Result<void>::success()
            : core::Result<void>::failure(validation(
                  std::string("collision error threshold for ") + label +
                  " must be finite and non-negative"));
    };
    if (auto result = check(max_volume_overlap_residual, "volume overlap");
        !result) return result;
    if (auto result = check(max_penetration_depth, "penetration"); !result) return result;
    if (auto result = check(max_one_sided_hausdorff, "hausdorff"); !result) return result;
    if (auto result = check(max_clearance_violation, "clearance"); !result) return result;
    return core::Result<void>::success();
}

std::string CollisionErrorThresholdPolicy::canonical() const {
    const auto encode = [](const std::optional<double>& bound) {
        if (!bound.has_value()) return std::string("-");
        std::ostringstream output;
        output << std::setprecision(17) << bound.value();
        return output.str();
    };
    std::ostringstream output;
    output << "THRESHOLDS VOLUME_OVERLAP " << encode(max_volume_overlap_residual)
           << " PENETRATION " << encode(max_penetration_depth)
           << " HAUSDORFF " << encode(max_one_sided_hausdorff)
           << " CLEARANCE " << encode(max_clearance_violation);
    return output.str();
}

core::Result<assets::Sha256Digest> CollisionErrorThresholdPolicy::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = this->canonical();
    return core::Result<assets::Sha256Digest>::success(
        digest_text(canonical));
}

core::Result<void> evaluate_collision_error(
    const CollisionErrorMetrics& metrics,
    const CollisionErrorThresholdPolicy& policy) {
    if (auto result = metrics.validate(); !result) return result;
    if (auto result = policy.validate(); !result) return result;
    const auto exceed = [](double measured, const std::optional<double>& bound) {
        return bound.has_value() && measured > bound.value();
    };
    if (exceed(metrics.volume_overlap_residual, policy.max_volume_overlap_residual)) {
        return core::Result<void>::failure(validation(
            "volume overlap residual exceeds the admission threshold"));
    }
    if (exceed(metrics.max_penetration_depth, policy.max_penetration_depth)) {
        return core::Result<void>::failure(validation(
            "max penetration depth exceeds the admission threshold"));
    }
    if (exceed(metrics.one_sided_hausdorff, policy.max_one_sided_hausdorff)) {
        return core::Result<void>::failure(validation(
            "one-sided hausdorff distance exceeds the admission threshold"));
    }
    if (exceed(metrics.clearance_violation, policy.max_clearance_violation)) {
        return core::Result<void>::failure(validation(
            "clearance violation exceeds the admission threshold"));
    }
    return core::Result<void>::success();
}

core::Result<double> mesh_volume(const geometry::CompiledMesh& mesh) {
    if (!mesh.valid()) {
        return core::Result<double>::failure(invalid(
            "mesh volume requires a valid compiled mesh"));
    }
    double volume6 = 0.0;
    for (std::size_t corner = 0U; corner < mesh.indices.size(); corner += 3U) {
        const core::Vec3d& a = mesh.positions[mesh.indices[corner]];
        const core::Vec3d& b = mesh.positions[mesh.indices[corner + 1U]];
        const core::Vec3d& c = mesh.positions[mesh.indices[corner + 2U]];
        volume6 += dot(a, cross(b, c));
    }
    return core::Result<double>::success(volume6 / 6.0);
}

double one_sided_hausdorff(
    std::span<const core::Vec3d> from,
    std::span<const core::Vec3d> to) {
    if (from.empty() || to.empty()) return 0.0;
    double maximum = 0.0;
    for (const core::Vec3d& point : from) {
        double nearest = (point - to.front()).length();
        for (std::size_t index = 1U; index < to.size(); ++index) {
            nearest = std::min(nearest, (point - to[index]).length());
        }
        maximum = std::max(maximum, nearest);
    }
    return maximum;
}

double sphere_sphere_overlap_volume(
    core::Vec3d first_center,
    double first_radius,
    core::Vec3d second_center,
    double second_radius) {
    const double distance = (first_center - second_center).length();
    if (distance >= first_radius + second_radius) return 0.0;
    const double large = std::max(first_radius, second_radius);
    const double small = std::min(first_radius, second_radius);
    if (distance <= large - small) {
        return 4.0 * kPi * small * small * small / 3.0;
    }
    // The lens is the sum of the two spherical caps facing each other.
    const double first_cap_height =
        first_radius - (distance * distance + first_radius * first_radius -
            second_radius * second_radius) / (2.0 * distance);
    const double second_cap_height =
        second_radius - (distance * distance + second_radius * second_radius -
            first_radius * first_radius) / (2.0 * distance);
    const auto cap_volume = [](double radius, double height) {
        return kPi * height * height * (radius - height / 3.0);
    };
    return cap_volume(first_radius, first_cap_height) +
        cap_volume(second_radius, second_cap_height);
}

double box_box_overlap_volume(
    core::Vec3d first_center,
    core::Vec3d first_half_extents,
    core::Vec3d second_center,
    core::Vec3d second_half_extents) {
    core::Bounds3d first;
    first.include(first_center - first_half_extents);
    first.include(first_center + first_half_extents);
    core::Bounds3d second;
    second.include(second_center - second_half_extents);
    second.include(second_center + second_half_extents);
    return bounds_overlap_volume(first, second);
}

double sphere_box_overlap_volume(
    core::Vec3d sphere_center,
    double sphere_radius,
    core::Vec3d box_center,
    core::Vec3d box_half_extents) {
    // Closest point in the box to the sphere center decides the overlap: the
    // sphere contributes a spherical cap of height radius - d when the center
    // lies outside, or its full volume when the center lies inside.
    const double dx = std::max(
        0.0, std::abs(sphere_center.x - box_center.x) - box_half_extents.x);
    const double dy = std::max(
        0.0, std::abs(sphere_center.y - box_center.y) - box_half_extents.y);
    const double dz = std::max(
        0.0, std::abs(sphere_center.z - box_center.z) - box_half_extents.z);
    const double outside = std::hypot(std::hypot(dx, dy), dz);
    if (outside >= sphere_radius) return 0.0;
    if (outside == 0.0) {
        return 4.0 * kPi * sphere_radius * sphere_radius * sphere_radius / 3.0;
    }
    const double cap_height = sphere_radius - outside;
    return kPi * cap_height * cap_height * (3.0 * sphere_radius - cap_height) / 3.0;
}

core::Result<double> primitive_pair_volume_overlap(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second) {
    if (auto result = first.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (auto result = second.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    const bool first_round = first.kind == PrimitiveCollisionKind::sphere;
    const bool second_round = second.kind == PrimitiveCollisionKind::sphere;
    const bool first_box = first.kind == PrimitiveCollisionKind::box;
    const bool second_box = second.kind == PrimitiveCollisionKind::box;
    if (first_round && second_round) {
        return core::Result<double>::success(sphere_sphere_overlap_volume(
            first.center, first.radius, second.center, second.radius));
    }
    if (first_box && second_box) {
        return core::Result<double>::success(box_box_overlap_volume(
            first.center, first.half_extents, second.center, second.half_extents));
    }
    if (first_round && second_box) {
        return core::Result<double>::success(sphere_box_overlap_volume(
            first.center, first.radius, second.center, second.half_extents));
    }
    if (first_box && second_round) {
        return core::Result<double>::success(sphere_box_overlap_volume(
            second.center, second.radius, first.center, first.half_extents));
    }
    return core::Result<double>::success(bounds_overlap_volume(
        conservative_bounds(first), conservative_bounds(second)));
}

core::Result<double> primitive_pair_max_penetration(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second) {
    if (auto result = first.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (auto result = second.validate(); !result) {
        return core::Result<double>::failure(result.error());
    }
    if (first.kind == PrimitiveCollisionKind::sphere &&
        second.kind == PrimitiveCollisionKind::sphere) {
        return core::Result<double>::success(clamped(
            first.radius + second.radius - center_distance(first, second)));
    }
    if (first.kind == PrimitiveCollisionKind::box &&
        second.kind == PrimitiveCollisionKind::box) {
        // Minimal translation depth across the separating axes of two AABBs.
        const double mx = clamped(
            (first.half_extents.x + second.half_extents.x) -
            std::abs(first.center.x - second.center.x));
        const double my = clamped(
            (first.half_extents.y + second.half_extents.y) -
            std::abs(first.center.y - second.center.y));
        const double mz = clamped(
            (first.half_extents.z + second.half_extents.z) -
            std::abs(first.center.z - second.center.z));
        if (mx <= 0.0 || my <= 0.0 || mz <= 0.0) {
            return core::Result<double>::success(0.0);
        }
        return core::Result<double>::success(std::min({mx, my, mz}));
    }
    if (first.kind == PrimitiveCollisionKind::sphere &&
        second.kind == PrimitiveCollisionKind::box) {
        const double dx = std::max(
            0.0, std::abs(first.center.x - second.center.x) - second.half_extents.x);
        const double dy = std::max(
            0.0, std::abs(first.center.y - second.center.y) - second.half_extents.y);
        const double dz = std::max(
            0.0, std::abs(first.center.z - second.center.z) - second.half_extents.z);
        return core::Result<double>::success(
            clamped(first.radius - std::hypot(std::hypot(dx, dy), dz)));
    }
    if (first.kind == PrimitiveCollisionKind::box &&
        second.kind == PrimitiveCollisionKind::sphere) {
        return primitive_pair_max_penetration(second, first);
    }
    // Conservative fallback: bounding-sphere interpenetration.
    return core::Result<double>::success(clamped(
        bounding_radius(first) + bounding_radius(second) -
        center_distance(first, second)));
}

double clearance_violation(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second,
    double required_clearance) {
    const double separation =
        center_distance(first, second) - bounding_radius(first) - bounding_radius(second);
    return clamped(required_clearance - separation);
}

core::Result<CollisionErrorMetrics> compute_pair_metrics(
    const PrimitiveCollisionCandidate& first,
    const PrimitiveCollisionCandidate& second,
    double required_clearance) {
    if (!std::isfinite(required_clearance) || required_clearance < 0.0) {
        return core::Result<CollisionErrorMetrics>::failure(validation(
            "required clearance must be finite and non-negative"));
    }
    CollisionErrorMetrics metrics;
    auto overlap = primitive_pair_volume_overlap(first, second);
    if (!overlap) return core::Result<CollisionErrorMetrics>::failure(overlap.error());
    auto penetration = primitive_pair_max_penetration(first, second);
    if (!penetration) {
        return core::Result<CollisionErrorMetrics>::failure(penetration.error());
    }
    metrics.volume_overlap_residual = overlap.value();
    metrics.max_penetration_depth = penetration.value();
    metrics.one_sided_hausdorff = 0.0;
    metrics.clearance_violation = clearance_violation(
        first, second, required_clearance);
    if (auto result = metrics.validate(); !result) {
        return core::Result<CollisionErrorMetrics>::failure(result.error());
    }
    return core::Result<CollisionErrorMetrics>::success(metrics);
}

} // namespace carto::collision
