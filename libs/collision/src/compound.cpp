#include <carto/collision/compound.hpp>

#include <carto/collision/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <span>
#include <tuple>

namespace carto::collision {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

struct PartVertices {
    std::vector<std::size_t> vertex_indices;
    core::Bounds3d bounds;
};

core::Vec3d bounds_extents(const core::Bounds3d& bounds) {
    return {
        bounds.maximum.x - bounds.minimum.x,
        bounds.maximum.y - bounds.minimum.y,
        bounds.maximum.z - bounds.minimum.z,
    };
}

// Longest-axis selection with a strict greater-than scan keeps ties on the
// earliest axis, so partitioning is byte-stable for identical inputs.
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

// Deterministic in-place selection of the lower median index along one axis.
std::size_t lower_median(
    std::vector<std::size_t>& vertices,
    const std::vector<core::Vec3d>& positions,
    std::size_t axis) {
    std::vector<double> keys(vertices.size());
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        const core::Vec3d& point = positions[vertices[index]];
        keys[index] = axis == 0U ? point.x : (axis == 1U ? point.y : point.z);
    }
    std::vector<std::size_t> order(vertices.size());
    std::iota(order.begin(), order.end(), std::size_t{0U});
    std::stable_sort(order.begin(), order.end(), [&keys](std::size_t left, std::size_t right) {
        return keys[left] < keys[right];
    });
    std::vector<std::size_t> sorted = vertices;
    for (std::size_t index = 0U; index < order.size(); ++index) {
        sorted[index] = vertices[order[index]];
    }
    vertices = std::move(sorted);
    return vertices.size() / 2U;
}

void partition_recursively(
    std::vector<std::size_t> vertices,
    const std::vector<core::Vec3d>& positions,
    std::uint32_t depth,
    const CompoundSearchOptions& options,
    std::vector<PartVertices>& parts) {
    if (vertices.size() <= options.target_vertices_per_part ||
        depth >= options.max_depth || parts.size() + 1U >= options.max_parts ||
        vertices.size() < 2U) {
        PartVertices part;
        part.vertex_indices = std::move(vertices);
        for (const std::size_t index : part.vertex_indices) {
            part.bounds.include(positions[index]);
        }
        parts.push_back(std::move(part));
        return;
    }
    core::Bounds3d bounds;
    for (const std::size_t index : vertices) {
        bounds.include(positions[index]);
    }
    const std::size_t axis = longest_axis(bounds_extents(bounds));
    std::vector<std::size_t> lower = vertices;
    const std::size_t median = lower_median(lower, positions, axis);
    std::vector<std::size_t> upper(lower.begin() + static_cast<std::ptrdiff_t>(median),
        lower.end());
    lower.erase(lower.begin() + static_cast<std::ptrdiff_t>(median), lower.end());
    if (lower.empty() || upper.empty()) {
        PartVertices part;
        part.vertex_indices = std::move(vertices);
        for (const std::size_t index : part.vertex_indices) {
            part.bounds.include(positions[index]);
        }
        parts.push_back(std::move(part));
        return;
    }
    partition_recursively(
        std::move(lower), positions, depth + 1U, options, parts);
    partition_recursively(
        std::move(upper), positions, depth + 1U, options, parts);
}

core::Result<PrimitiveCollisionCandidate> fit_part(
    const PartVertices& part,
    const geometry::CompiledMesh& mesh) {
    geometry::CompiledMesh part_mesh;
    part_mesh.bounds = part.bounds;
    part_mesh.source_revision = mesh.source_revision;
    part_mesh.positions.reserve(part.vertex_indices.size());
    part_mesh.normals.reserve(part.vertex_indices.size());
    part_mesh.vertex_ids.reserve(part.vertex_indices.size());
    std::vector<std::uint32_t> remap(mesh.positions.size(),
        std::numeric_limits<std::uint32_t>::max());
    for (std::size_t index = 0U; index < part.vertex_indices.size(); ++index) {
        const std::size_t source = part.vertex_indices[index];
        remap[source] = static_cast<std::uint32_t>(index);
        part_mesh.positions.push_back(mesh.positions[source]);
        part_mesh.normals.push_back(mesh.normals[source]);
        part_mesh.vertex_ids.push_back(mesh.vertex_ids[source]);
    }
    part_mesh.indices.reserve(mesh.indices.size());
    part_mesh.triangle_faces.reserve(mesh.triangle_faces.size());
    for (std::size_t corner = 0U; corner < mesh.indices.size(); corner += 3U) {
        const std::uint32_t a = mesh.indices[corner];
        const std::uint32_t b = mesh.indices[corner + 1U];
        const std::uint32_t c = mesh.indices[corner + 2U];
        if (remap[a] != std::numeric_limits<std::uint32_t>::max() &&
            remap[b] != std::numeric_limits<std::uint32_t>::max() &&
            remap[c] != std::numeric_limits<std::uint32_t>::max()) {
            part_mesh.indices.push_back(remap[a]);
            part_mesh.indices.push_back(remap[b]);
            part_mesh.indices.push_back(remap[c]);
            part_mesh.triangle_faces.push_back(
                mesh.triangle_faces[corner / 3U]);
            part_mesh.triangle_edges.emplace_back();
        }
    }
    if (part_mesh.positions.empty() || part_mesh.indices.empty()) {
        // A vertex-only part still fits against its own bounds; volume
        // evidence degrades to a zero-volume source, which is explicit.
        part_mesh.indices.clear();
        part_mesh.triangle_faces.clear();
        part_mesh.triangle_edges.clear();
    }
    auto box = fit_box_candidate(part_mesh);
    if (!box) return core::Result<PrimitiveCollisionCandidate>::failure(box.error());
    auto sphere = fit_sphere_candidate(part_mesh);
    if (!sphere) {
        return core::Result<PrimitiveCollisionCandidate>::failure(sphere.error());
    }
    // Deterministic tie-break toward the box keeps selection byte-stable.
    if (sphere.value().fit.aggregate() < box.value().fit.aggregate()) {
        return sphere;
    }
    return box;
}

} // namespace

std::string_view compound_partition_strategy_name(
    CompoundPartitionStrategy strategy) noexcept {
    switch (strategy) {
    case CompoundPartitionStrategy::longest_axis_median_split:
        return "longest_axis_median_split";
    }
    return "unknown";
}

core::Result<void> CompoundSearchOptions::validate() const {
    switch (strategy) {
    case CompoundPartitionStrategy::longest_axis_median_split:
        break;
    default:
        return core::Result<void>::failure(invalid(
            "compound partition strategy is invalid"));
    }
    if (max_parts == 0U || max_parts > kHardMaxParts) {
        return core::Result<void>::failure(validation(
            "compound search part count is outside its bounded range"));
    }
    if (max_depth == 0U || max_depth > kHardMaxDepth) {
        return core::Result<void>::failure(validation(
            "compound search depth is outside its bounded range"));
    }
    if (target_vertices_per_part < kMinVerticesPerPart) {
        return core::Result<void>::failure(validation(
            "compound search target vertices per part is below the floor"));
    }
    return core::Result<void>::success();
}

core::Result<void> CompoundCollisionCandidate::validate() const {
    switch (strategy) {
    case CompoundPartitionStrategy::longest_axis_median_split:
        break;
    default:
        return core::Result<void>::failure(invalid(
            "compound partition strategy is invalid"));
    }
    if (parts.empty() || parts.size() > CompoundSearchOptions::kHardMaxParts) {
        return core::Result<void>::failure(validation(
            "compound candidate must carry a bounded non-empty part set"));
    }
    for (const PrimitiveCollisionCandidate& part : parts) {
        if (auto result = part.validate(); !result) return result;
    }
    if (!std::isfinite(aggregate_error) || aggregate_error < 0.0 ||
        !std::isfinite(total_volume) || total_volume < 0.0) {
        return core::Result<void>::failure(validation(
            "compound aggregate error and volume must be finite and non-negative"));
    }
    if (fitted_vertex_count == 0U) {
        return core::Result<void>::failure(validation(
            "compound candidate must name its fitted vertex count"));
    }
    if (candidate_digest.is_zero()) {
        return core::Result<void>::failure(validation(
            "compound candidate must be digest-bound via refresh_digest"));
    }
    return core::Result<void>::success();
}

std::string CompoundCollisionCandidate::canonical_without_digest() const {
    std::ostringstream output;
    output << std::setprecision(17)
           << "COMPOUND STRATEGY " << compound_partition_strategy_name(strategy)
           << " AGGREGATE_ERROR " << aggregate_error
           << " TOTAL_VOLUME " << total_volume
           << " VERTICES " << fitted_vertex_count
           << " PART_COUNT " << parts.size();
    for (const PrimitiveCollisionCandidate& part : parts) {
        output << "\nPART " << part.canonical_without_digest();
    }
    return output.str();
}

std::string CompoundCollisionCandidate::canonical() const {
    return canonical_without_digest() + "\nDIGEST " + candidate_digest.hex();
}

core::Result<void> CompoundCollisionCandidate::refresh_digest() {
    candidate_digest = digest_text(canonical_without_digest());
    return core::Result<void>::success();
}

core::Result<CompoundCollisionCandidate> search_compound_candidate(
    const geometry::CompiledMesh& mesh,
    const CompoundSearchOptions& options) {
    if (auto result = options.validate(); !result) {
        return core::Result<CompoundCollisionCandidate>::failure(result.error());
    }
    if (!mesh.valid() || mesh.positions.empty()) {
        return core::Result<CompoundCollisionCandidate>::failure(invalid(
            "compound search requires a valid compiled mesh with vertices"));
    }

    std::vector<std::size_t> all(mesh.positions.size());
    std::iota(all.begin(), all.end(), std::size_t{0U});
    std::vector<PartVertices> parts;
    partition_recursively(std::move(all), mesh.positions, 0U, options, parts);
    if (parts.empty()) {
        return core::Result<CompoundCollisionCandidate>::failure(validation(
            "compound search produced no parts"));
    }
    // Canonical part order: lexicographic on bounds minimum then maximum, so
    // generation order never leaks into the digest.
    std::sort(parts.begin(), parts.end(), [](const PartVertices& left, const PartVertices& right) {
        return std::tie(left.bounds.minimum.x, left.bounds.minimum.y,
                   left.bounds.minimum.z, left.bounds.maximum.x,
                   left.bounds.maximum.y, left.bounds.maximum.z) <
            std::tie(right.bounds.minimum.x, right.bounds.minimum.y,
                right.bounds.minimum.z, right.bounds.maximum.x,
                right.bounds.maximum.y, right.bounds.maximum.z);
    });

    CompoundCollisionCandidate candidate;
    candidate.strategy = options.strategy;
    candidate.fitted_vertex_count = mesh.positions.size();
    candidate.parts.reserve(parts.size());
    double worst_fit = 0.0;
    for (const PartVertices& part : parts) {
        auto fit = fit_part(part, mesh);
        if (!fit) {
            return core::Result<CompoundCollisionCandidate>::failure(fit.error());
        }
        worst_fit = std::max(worst_fit, fit.value().fit.aggregate());
        candidate.parts.push_back(fit.value());
    }
    constexpr double kPi = 3.14159265358979323846;
    double total_volume = 0.0;
    for (const PrimitiveCollisionCandidate& part : candidate.parts) {
        if (part.kind == PrimitiveCollisionKind::box) {
            total_volume += 8.0 * part.half_extents.x * part.half_extents.y *
                part.half_extents.z;
        } else {
            total_volume += 4.0 * kPi * part.radius * part.radius * part.radius / 3.0;
        }
    }
    double overlap = 0.0;
    for (std::size_t first = 0U; first < candidate.parts.size(); ++first) {
        for (std::size_t second = first + 1U; second < candidate.parts.size();
             ++second) {
            auto pair_overlap = primitive_pair_volume_overlap(
                candidate.parts[first], candidate.parts[second]);
            if (!pair_overlap) {
                return core::Result<CompoundCollisionCandidate>::failure(
                    pair_overlap.error());
            }
            overlap += pair_overlap.value();
        }
    }
    candidate.total_volume = total_volume;
    candidate.aggregate_error = worst_fit + overlap;
    static_cast<void>(candidate.refresh_digest());
    return core::Result<CompoundCollisionCandidate>::success(std::move(candidate));
}

} // namespace carto::collision
