#include <carto/collision/primitive.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <span>

namespace carto::collision {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_kind(PrimitiveCollisionKind kind) {
    switch (kind) {
    case PrimitiveCollisionKind::sphere:
    case PrimitiveCollisionKind::box:
    case PrimitiveCollisionKind::capsule:
    case PrimitiveCollisionKind::cylinder:
        return true;
    }
    return false;
}

bool zero_vec(core::Vec3d value) {
    return value.x == 0.0 && value.y == 0.0 && value.z == 0.0;
}

bool non_negative_vec(core::Vec3d value) {
    return value.x >= 0.0 && value.y >= 0.0 && value.z >= 0.0;
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

core::Result<void> require_mesh(const geometry::CompiledMesh& mesh) {
    if (!mesh.valid() || mesh.positions.empty()) {
        return core::Result<void>::failure(invalid(
            "primitive fit requires a valid compiled mesh with vertices"));
    }
    return core::Result<void>::success();
}

core::Vec3d bounds_center(const core::Bounds3d& bounds) {
    return {
        (bounds.minimum.x + bounds.maximum.x) * 0.5,
        (bounds.minimum.y + bounds.maximum.y) * 0.5,
        (bounds.minimum.z + bounds.maximum.z) * 0.5,
    };
}

core::Vec3d bounds_extents(const core::Bounds3d& bounds) {
    return {
        bounds.maximum.x - bounds.minimum.x,
        bounds.maximum.y - bounds.minimum.y,
        bounds.maximum.z - bounds.minimum.z,
    };
}

// Longest-axis selection with a strict greater-than scan keeps ties resolved
// toward the earliest axis, which keeps every fit byte-stable.
std::size_t longest_axis(core::Vec3d extents) {
    std::size_t axis = 0U;
    double best = extents.x;
    if (extents.y > best) {
        best = extents.y;
        axis = 1U;
    }
    if (extents.z > best) {
        axis = 2U;
    }
    return axis;
}

double axis_component(core::Vec3d value, std::size_t axis) {
    return axis == 0U ? value.x : (axis == 1U ? value.y : value.z);
}

core::Vec3d axis_unit(std::size_t axis) {
    return axis == 0U ? core::Vec3d{1.0, 0.0, 0.0}
                      : (axis == 1U ? core::Vec3d{0.0, 1.0, 0.0}
                                    : core::Vec3d{0.0, 0.0, 1.0});
}

double point_outside_sphere(core::Vec3d point, core::Vec3d center, double radius) {
    return std::max(0.0, (point - center).length() - radius);
}

double point_outside_box(
    core::Vec3d point, core::Vec3d center, core::Vec3d half_extents) {
    const double dx = std::max(0.0, std::abs(point.x - center.x) - half_extents.x);
    const double dy = std::max(0.0, std::abs(point.y - center.y) - half_extents.y);
    const double dz = std::max(0.0, std::abs(point.z - center.z) - half_extents.z);
    return std::hypot(std::hypot(dx, dy), dz);
}

double point_outside_capsule(
    core::Vec3d point,
    core::Vec3d center,
    double radius,
    double half_height,
    std::size_t axis) {
    const core::Vec3d direction = axis_unit(axis);
    const core::Vec3d segment = direction * (2.0 * half_height);
    if (half_height == 0.0) {
        return std::max(0.0, (point - center).length() - radius);
    }
    const core::Vec3d start = center - direction * half_height;
    const double segment_length_squared = segment.length_squared();
    double t = dot(point - start, segment) / segment_length_squared;
    t = std::clamp(t, 0.0, 1.0);
    const core::Vec3d closest = start + segment * t;
    return std::max(0.0, (point - closest).length() - radius);
}

// Conservative unsigned distance to a capped cylinder: the Euclidean norm of
// the radial and axial excesses. This deliberately over-reports near the rim
// so fit error is never silently understated.
double point_outside_cylinder(
    core::Vec3d point,
    core::Vec3d center,
    double radius,
    double half_height,
    std::size_t axis) {
    const core::Vec3d delta = point - center;
    const double axial = std::max(
        0.0, std::abs(axis_component(delta, axis)) - half_height);
    core::Vec3d radial_delta = delta;
    if (axis == 0U) {
        radial_delta.x = 0.0;
    } else if (axis == 1U) {
        radial_delta.y = 0.0;
    } else {
        radial_delta.z = 0.0;
    }
    const double radial = std::max(0.0, radial_delta.length() - radius);
    return std::hypot(radial, axial);
}

double primitive_volume(const PrimitiveCollisionCandidate& candidate) {
    constexpr double kPi = 3.14159265358979323846;
    switch (candidate.kind) {
    case PrimitiveCollisionKind::sphere:
        return 4.0 * kPi * candidate.radius * candidate.radius * candidate.radius / 3.0;
    case PrimitiveCollisionKind::box:
        return 8.0 * candidate.half_extents.x * candidate.half_extents.y *
            candidate.half_extents.z;
    case PrimitiveCollisionKind::capsule:
        // Cylinder core plus two spherical caps.
        return kPi * candidate.radius * candidate.radius *
            (2.0 * candidate.half_height +
                4.0 * candidate.radius / 3.0);
    case PrimitiveCollisionKind::cylinder:
        return kPi * candidate.radius * candidate.radius * 2.0 * candidate.half_height;
    }
    return 0.0;
}

// Signed tetrahedron volume sum; exact for consistently wound closed meshes.
double signed_mesh_volume(const geometry::CompiledMesh& mesh) {
    double volume6 = 0.0;
    for (std::size_t corner = 0U; corner < mesh.indices.size(); corner += 3U) {
        const core::Vec3d& a = mesh.positions[mesh.indices[corner]];
        const core::Vec3d& b = mesh.positions[mesh.indices[corner + 1U]];
        const core::Vec3d& c = mesh.positions[mesh.indices[corner + 2U]];
        volume6 += dot(a, cross(b, c));
    }
    return volume6 / 6.0;
}

template <typename Distance>
PrimitiveCollisionCandidate finish_fit(
    PrimitiveCollisionKind kind,
    core::Vec3d center,
    core::Vec3d half_extents,
    double radius,
    double half_height,
    const geometry::CompiledMesh& mesh,
    Distance distance) {
    PrimitiveCollisionCandidate candidate;
    candidate.kind = kind;
    candidate.center = center;
    candidate.half_extents = half_extents;
    candidate.radius = radius;
    candidate.half_height = half_height;
    candidate.fitted_vertex_count = mesh.positions.size();
    double max_outside = 0.0;
    for (const core::Vec3d& point : mesh.positions) {
        max_outside = std::max(max_outside, distance(point));
    }
    candidate.fit.max_outside_distance = max_outside;
    candidate.fit.volume_residual =
        std::abs(signed_mesh_volume(mesh) - primitive_volume(candidate));
    static_cast<void>(candidate.refresh_digest());
    return candidate;
}

} // namespace

std::string_view primitive_collision_kind_name(PrimitiveCollisionKind kind) noexcept {
    switch (kind) {
    case PrimitiveCollisionKind::sphere: return "sphere";
    case PrimitiveCollisionKind::box: return "box";
    case PrimitiveCollisionKind::capsule: return "capsule";
    case PrimitiveCollisionKind::cylinder: return "cylinder";
    }
    return "unknown";
}

core::Result<void> CollisionFitError::validate() const {
    if (!std::isfinite(max_outside_distance) || max_outside_distance < 0.0 ||
        !std::isfinite(volume_residual) || volume_residual < 0.0) {
        return core::Result<void>::failure(validation(
            "primitive fit error terms must be finite and non-negative"));
    }
    return core::Result<void>::success();
}

std::string CollisionFitError::canonical() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << max_outside_distance << ' ' << volume_residual;
    return output.str();
}

core::Result<void> PrimitiveCollisionCandidate::validate() const {
    if (!valid_kind(kind)) {
        return core::Result<void>::failure(invalid(
            "primitive collision kind is invalid"));
    }
    if (!center.finite() || !half_extents.finite() || !std::isfinite(radius) ||
        !std::isfinite(half_height)) {
        return core::Result<void>::failure(validation(
            "primitive collision parameters must be finite"));
    }
    switch (kind) {
    case PrimitiveCollisionKind::sphere:
        if (!zero_vec(half_extents) || half_height != 0.0 || radius <= 0.0) {
            return core::Result<void>::failure(validation(
                "sphere candidate must carry only a positive radius"));
        }
        break;
    case PrimitiveCollisionKind::box:
        if (zero_vec(half_extents) || !non_negative_vec(half_extents) ||
            radius != 0.0 || half_height != 0.0) {
            return core::Result<void>::failure(validation(
                "box candidate must carry only positive non-negative half extents"));
        }
        break;
    case PrimitiveCollisionKind::capsule:
    case PrimitiveCollisionKind::cylinder:
        if (!zero_vec(half_extents) || radius <= 0.0 || half_height < 0.0) {
            return core::Result<void>::failure(validation(
                "capsule and cylinder candidates must carry a positive radius "
                "and non-negative half height"));
        }
        break;
    }
    if (auto result = fit.validate(); !result) return result;
    if (fitted_vertex_count == 0U) {
        return core::Result<void>::failure(validation(
            "primitive candidate must name its fitted vertex count"));
    }
    if (candidate_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "primitive candidate must be digest-bound via refresh_digest"));
    }
    return core::Result<void>::success();
}

std::string PrimitiveCollisionCandidate::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "PRIMITIVE KIND " << primitive_collision_kind_name(kind)
           << " CENTER " << center.x << ' ' << center.y << ' ' << center.z
           << " HALF_EXTENTS " << half_extents.x << ' ' << half_extents.y << ' '
           << half_extents.z
           << " RADIUS " << radius
           << " HALF_HEIGHT " << half_height
           << " FIT " << fit.canonical()
           << " VERTICES " << fitted_vertex_count;
    return output.str();
}

std::string PrimitiveCollisionCandidate::canonical() const {
    return canonical_without_digest() + " DIGEST " + candidate_digest.hex();
}

core::Result<void> PrimitiveCollisionCandidate::refresh_digest() {
    candidate_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<void> PrimitiveFitOptions::validate() const {
    if (permitted_kinds.empty() || permitted_kinds.size() > 4U) {
        return core::Result<void>::failure(validation(
            "primitive fit must permit between one and four kinds"));
    }
    for (PrimitiveCollisionKind kind : permitted_kinds) {
        if (!valid_kind(kind)) {
            return core::Result<void>::failure(invalid(
                "primitive fit permits an unknown kind"));
        }
    }
    for (std::size_t first = 0U; first < permitted_kinds.size(); ++first) {
        for (std::size_t second = first + 1U; second < permitted_kinds.size();
             ++second) {
            if (permitted_kinds[first] == permitted_kinds[second]) {
                return core::Result<void>::failure(validation(
                    "primitive fit permits a duplicate kind"));
            }
        }
    }
    return core::Result<void>::success();
}

core::Result<PrimitiveCollisionCandidate> fit_sphere_candidate(
    const geometry::CompiledMesh& mesh) {
    if (auto result = require_mesh(mesh); !result) {
        return core::Result<PrimitiveCollisionCandidate>::failure(result.error());
    }
    const core::Vec3d center = bounds_center(mesh.bounds);
    double radius = 0.0;
    for (const core::Vec3d& point : mesh.positions) {
        radius = std::max(radius, (point - center).length());
    }
    return core::Result<PrimitiveCollisionCandidate>::success(finish_fit(
        PrimitiveCollisionKind::sphere, center, {}, radius, 0.0, mesh,
        [center, radius](core::Vec3d point) {
            return point_outside_sphere(point, center, radius);
        }));
}

core::Result<PrimitiveCollisionCandidate> fit_box_candidate(
    const geometry::CompiledMesh& mesh) {
    if (auto result = require_mesh(mesh); !result) {
        return core::Result<PrimitiveCollisionCandidate>::failure(result.error());
    }
    const core::Vec3d center = bounds_center(mesh.bounds);
    const core::Vec3d half_extents = bounds_extents(mesh.bounds) * 0.5;
    return core::Result<PrimitiveCollisionCandidate>::success(finish_fit(
        PrimitiveCollisionKind::box, center, half_extents, 0.0, 0.0, mesh,
        [center, half_extents](core::Vec3d point) {
            return point_outside_box(point, center, half_extents);
        }));
}

core::Result<PrimitiveCollisionCandidate> fit_capsule_candidate(
    const geometry::CompiledMesh& mesh) {
    if (auto result = require_mesh(mesh); !result) {
        return core::Result<PrimitiveCollisionCandidate>::failure(result.error());
    }
    const core::Vec3d center = bounds_center(mesh.bounds);
    const core::Vec3d extents = bounds_extents(mesh.bounds);
    const std::size_t axis = longest_axis(extents);
    double radius = 0.0;
    for (const core::Vec3d& point : mesh.positions) {
        core::Vec3d radial = point - center;
        if (axis == 0U) {
            radial.x = 0.0;
        } else if (axis == 1U) {
            radial.y = 0.0;
        } else {
            radial.z = 0.0;
        }
        radius = std::max(radius, radial.length());
    }
    const double half_height =
        std::max(0.0, axis_component(extents, axis) * 0.5 - radius);
    return core::Result<PrimitiveCollisionCandidate>::success(finish_fit(
        PrimitiveCollisionKind::capsule, center, {}, radius, half_height, mesh,
        [center, radius, half_height, axis](core::Vec3d point) {
            return point_outside_capsule(point, center, radius, half_height, axis);
        }));
}

core::Result<PrimitiveCollisionCandidate> fit_cylinder_candidate(
    const geometry::CompiledMesh& mesh) {
    if (auto result = require_mesh(mesh); !result) {
        return core::Result<PrimitiveCollisionCandidate>::failure(result.error());
    }
    const core::Vec3d center = bounds_center(mesh.bounds);
    const core::Vec3d extents = bounds_extents(mesh.bounds);
    const std::size_t axis = longest_axis(extents);
    double radius = 0.0;
    for (const core::Vec3d& point : mesh.positions) {
        core::Vec3d radial = point - center;
        if (axis == 0U) {
            radial.x = 0.0;
        } else if (axis == 1U) {
            radial.y = 0.0;
        } else {
            radial.z = 0.0;
        }
        radius = std::max(radius, radial.length());
    }
    const double half_height = axis_component(extents, axis) * 0.5;
    return core::Result<PrimitiveCollisionCandidate>::success(finish_fit(
        PrimitiveCollisionKind::cylinder, center, {}, radius, half_height, mesh,
        [center, radius, half_height, axis](core::Vec3d point) {
            return point_outside_cylinder(point, center, radius, half_height, axis);
        }));
}

core::Result<std::vector<PrimitiveCollisionCandidate>> generate_primitive_candidates(
    const geometry::CompiledMesh& mesh,
    const PrimitiveFitOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<std::vector<PrimitiveCollisionCandidate>>::failure(
            result.error());
    }
    if (auto result = require_mesh(mesh); !result) {
        return core::Result<std::vector<PrimitiveCollisionCandidate>>::failure(
            result.error());
    }
    std::vector<PrimitiveCollisionCandidate> candidates;
    const auto permitted = [&options](PrimitiveCollisionKind kind) {
        return std::find(options.permitted_kinds.begin(),
                    options.permitted_kinds.end(), kind) !=
            options.permitted_kinds.end();
    };
    // Fixed generation order keeps digests reproducible.
    if (permitted(PrimitiveCollisionKind::sphere)) {
        auto fit = fit_sphere_candidate(mesh);
        if (!fit) return core::Result<std::vector<PrimitiveCollisionCandidate>>::failure(
            fit.error());
        candidates.push_back(fit.value());
    }
    if (permitted(PrimitiveCollisionKind::box)) {
        auto fit = fit_box_candidate(mesh);
        if (!fit) return core::Result<std::vector<PrimitiveCollisionCandidate>>::failure(
            fit.error());
        candidates.push_back(fit.value());
    }
    if (permitted(PrimitiveCollisionKind::capsule)) {
        auto fit = fit_capsule_candidate(mesh);
        if (!fit) return core::Result<std::vector<PrimitiveCollisionCandidate>>::failure(
            fit.error());
        candidates.push_back(fit.value());
    }
    if (permitted(PrimitiveCollisionKind::cylinder)) {
        auto fit = fit_cylinder_candidate(mesh);
        if (!fit) return core::Result<std::vector<PrimitiveCollisionCandidate>>::failure(
            fit.error());
        candidates.push_back(fit.value());
    }
    return core::Result<std::vector<PrimitiveCollisionCandidate>>::success(
        std::move(candidates));
}

} // namespace carto::collision
