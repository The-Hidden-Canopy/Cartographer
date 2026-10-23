#include <carto/production/decal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace carto::production {
namespace {

constexpr std::size_t kMaxProjectionFaces = 250'000U;
constexpr std::size_t kMaxProjectionPolygons = 1'000'000U;
constexpr double kClipEpsilon = 1e-12;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

struct ClipVertex {
    core::Vec3d position;
    std::array<double, 3U> barycentric{};
};

double plane_distance(
    const core::Vec3d& point,
    const core::Vec3d& center,
    const core::Vec3d& axis_u,
    const core::Vec3d& axis_v,
    const core::Vec3d& axis_n,
    unsigned plane,
    double extent) {
    const core::Vec3d relative = point - center;
    const double u = core::dot(relative, axis_u);
    const double v = core::dot(relative, axis_v);
    const double n = core::dot(relative, axis_n);
    switch (plane) {
    case 0U: return u + extent; // left
    case 1U: return extent - u; // right
    case 2U: return v + extent; // bottom
    case 3U: return extent - v; // top
    case 4U: return n + extent; // back
    default: return extent - n; // front
    }
}

core::Result<std::vector<ClipVertex>> clip_triangle(
    std::array<ClipVertex, 3U> input,
    const core::Vec3d& center,
    const core::Vec3d& axis_u,
    const core::Vec3d& axis_v,
    const core::Vec3d& axis_n,
    double half_width,
    double half_height,
    double half_depth) {
    std::vector<ClipVertex> polygon(input.begin(), input.end());
    for (unsigned plane = 0U; plane < 6U && !polygon.empty(); ++plane) {
        const double extent = plane < 2U ? half_width :
            (plane < 4U ? half_height : half_depth);
        std::vector<ClipVertex> clipped;
        clipped.reserve(polygon.size() + 1U);
        ClipVertex previous = polygon.back();
        double previous_distance = plane_distance(previous.position, center,
            axis_u, axis_v, axis_n, plane, extent);
        if (!std::isfinite(previous_distance)) {
            return core::Result<std::vector<ClipVertex>>::failure(validation(
                "decal projection clipping exceeded numeric bounds"));
        }
        for (const ClipVertex& current : polygon) {
            const double current_distance = plane_distance(current.position, center,
                axis_u, axis_v, axis_n, plane, extent);
            if (!std::isfinite(current_distance)) {
                return core::Result<std::vector<ClipVertex>>::failure(validation(
                    "decal projection clipping exceeded numeric bounds"));
            }
            const bool previous_inside = previous_distance >= -kClipEpsilon;
            const bool current_inside = current_distance >= -kClipEpsilon;
            if (previous_inside != current_inside) {
                const double denominator = previous_distance - current_distance;
                if (std::abs(denominator) > kClipEpsilon) {
                    const double t = std::clamp(previous_distance / denominator, 0.0, 1.0);
                    ClipVertex intersection;
                    intersection.position = previous.position +
                        (current.position - previous.position) * t;
                    if (!intersection.position.finite()) {
                        return core::Result<std::vector<ClipVertex>>::failure(validation(
                            "decal projection generated an unrepresentable intersection"));
                    }
                    for (std::size_t i = 0U; i < 3U; ++i) {
                        intersection.barycentric[i] = previous.barycentric[i] +
                            (current.barycentric[i] - previous.barycentric[i]) * t;
                    }
                    clipped.push_back(intersection);
                }
            }
            if (current_inside) clipped.push_back(current);
            previous = current;
            previous_distance = current_distance;
        }
        polygon = std::move(clipped);
    }
    return core::Result<std::vector<ClipVertex>>::success(std::move(polygon));
}

bool read_unsigned(std::istream& input, std::uint64_t& value) {
    input >> value;
    return static_cast<bool>(input);
}

bool read_revision(std::istream& input, core::Revision& revision) {
    std::uint64_t value = 0U;
    if (!read_unsigned(input, value)) return false;
    revision = core::Revision(value);
    return !revision.exhausted();
}

} // namespace

core::Result<void> DecalPlacement::validate() const {
    const double normal_length = normal.length();
    const double tangent_length = tangent.length();
    const double orthogonality = std::abs(core::dot(normal, tangent));
    if (id == 0U || revision.exhausted() || target_mesh_asset == 0U ||
        target_mesh_revision.exhausted() || target_mesh_digest.is_zero() || material_id == 0U ||
        !center.finite() || !normal.finite() || !tangent.finite() ||
        !std::isfinite(normal_length) || !std::isfinite(tangent_length) ||
        normal_length < 1e-9 || tangent_length < 1e-9 ||
        orthogonality > normal_length * tangent_length * 1e-6 ||
        !std::isfinite(width) || width <= 0.0 || width > kMaxExtent ||
        !std::isfinite(height) || height <= 0.0 || height > kMaxExtent ||
        !std::isfinite(depth) || depth <= 0.0 || depth > kMaxExtent ||
        !std::isfinite(minimum_normal_dot) || minimum_normal_dot < 0.0 ||
        minimum_normal_dot > 1.0) {
        return core::Result<void>::failure(validation(
            "decal placement identity, source binding, frame, or projection limits are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_decal_source(
    const DecalPlacement& placement,
    std::uint64_t current_mesh_asset,
    core::Revision current_mesh_revision,
    const assets::Sha256Digest& current_mesh_digest) {
    if (auto result = placement.validate(); !result) return result;
    if (placement.target_mesh_asset != current_mesh_asset ||
        placement.target_mesh_revision != current_mesh_revision ||
        placement.target_mesh_digest != current_mesh_digest) {
        return core::Result<void>::failure(stale(
            "decal placement source mesh identity, revision, or digest changed"));
    }
    return core::Result<void>::success();
}

core::Result<DecalProjection> project_decal(
    const DecalPlacement& placement,
    const geometry::EditableMesh& mesh,
    std::uint64_t current_mesh_asset,
    const assets::Sha256Digest& current_mesh_digest) {
    if (auto result = validate_decal_source(
            placement, current_mesh_asset, mesh.revision(), current_mesh_digest); !result) {
        return core::Result<DecalProjection>::failure(result.error());
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<DecalProjection>::failure(result.error().with_context(
            "decal projection target mesh"));
    }
    if (mesh.face_count() > kMaxProjectionFaces || mesh.vertex_count() > 1'000'000U) {
        return core::Result<DecalProjection>::failure(validation(
            "decal projection exceeds the bounded target mesh size"));
    }

    const core::Vec3d axis_n = placement.normal.normalized();
    const core::Vec3d tangent = placement.tangent - axis_n * core::dot(placement.tangent, axis_n);
    const core::Vec3d axis_u = tangent.normalized();
    const core::Vec3d axis_v = core::cross(axis_n, axis_u).normalized();
    if (!axis_n.finite() || !axis_u.finite() || !axis_v.finite()) {
        return core::Result<DecalProjection>::failure(validation(
            "decal projection frame could not be normalized"));
    }

    DecalProjection projection;
    projection.placement_id = placement.id;
    projection.placement_revision = placement.revision;
    projection.target_mesh_asset = placement.target_mesh_asset;
    projection.target_mesh_revision = placement.target_mesh_revision;
    projection.target_mesh_digest = placement.target_mesh_digest;
    projection.material_id = placement.material_id;
    const double cosine_limit = placement.minimum_normal_dot;
    const double half_width = placement.width * 0.5;
    const double half_height = placement.height * 0.5;
    const double half_depth = placement.depth * 0.5;

    auto compiled = mesh.compile();
    if (!compiled) {
        return core::Result<DecalProjection>::failure(compiled.error().with_context(
            "decal projection triangulation"));
    }
    if (!compiled.value().valid() || compiled.value().indices.size() / 3U > kMaxProjectionPolygons) {
        return core::Result<DecalProjection>::failure(validation(
            "decal projection triangulation exceeds its output bound"));
    }
    std::map<geometry::FaceId, std::uint32_t> source_triangle_ordinals;
    for (std::size_t triangle = 0U;
         triangle < compiled.value().triangle_faces.size(); ++triangle) {
            const geometry::FaceId source_face = compiled.value().triangle_faces[triangle];
            const std::uint32_t source_triangle_index = source_triangle_ordinals[source_face]++;
            std::array<ClipVertex, 3U> triangle_vertices{};
            bool resolved = true;
            for (std::size_t corner = 0U; corner < 3U; ++corner) {
                const std::size_t index = triangle * 3U + corner;
                const std::uint32_t compiled_index = compiled.value().indices[index];
                if (compiled_index >= compiled.value().positions.size() ||
                    compiled_index >= compiled.value().vertex_ids.size()) {
                    resolved = false;
                    break;
                }
                const auto* vertex = mesh.find_vertex(compiled.value().vertex_ids[compiled_index]);
                if (vertex == nullptr || !vertex->position.finite()) {
                    resolved = false;
                    break;
                }
                triangle_vertices[corner].position = vertex->position;
                triangle_vertices[corner].barycentric[corner] = 1.0;
            }
            if (!resolved) {
                return core::Result<DecalProjection>::failure(validation(
                    "decal projection encountered unresolved source vertices"));
            }
            const core::Vec3d source_normal = core::cross(
                triangle_vertices[1U].position - triangle_vertices[0U].position,
                triangle_vertices[2U].position - triangle_vertices[0U].position).normalized();
            if (!source_normal.finite()) {
                return core::Result<DecalProjection>::failure(validation(
                    "decal projection encountered a degenerate source triangle"));
            }
            if (core::dot(source_normal, axis_n) < cosine_limit) continue;
            auto clipped_result = clip_triangle(triangle_vertices, placement.center,
                axis_u, axis_v, axis_n, half_width, half_height, half_depth);
            if (!clipped_result) {
                return core::Result<DecalProjection>::failure(clipped_result.error());
            }
            const auto& clipped = clipped_result.value();
            if (clipped.size() < 3U) continue;
            if (projection.polygons.size() >= kMaxProjectionPolygons) {
                return core::Result<DecalProjection>::failure(validation(
                    "decal projection exceeds the bounded output polygon count"));
            }
            DecalProjectedPolygon polygon;
            polygon.source_face = source_face;
            polygon.source_triangle = source_triangle_index;
            polygon.vertices.reserve(clipped.size());
            for (const auto& vertex : clipped) {
                const core::Vec3d relative = vertex.position - placement.center;
                const double u = core::dot(relative, axis_u) / placement.width + 0.5;
                const double v = core::dot(relative, axis_v) / placement.height + 0.5;
                if (!std::isfinite(u) || !std::isfinite(v)) {
                    return core::Result<DecalProjection>::failure(validation(
                        "decal projection generated a non-finite UV coordinate"));
                }
                polygon.vertices.push_back({{std::clamp(u, 0.0, 1.0),
                    std::clamp(v, 0.0, 1.0)}, vertex.barycentric});
            }
            projection.polygons.push_back(std::move(polygon));
    }
    return core::Result<DecalProjection>::success(std::move(projection));
}

core::Result<DecalPlacement> revise_decal_placement(
    const DecalPlacement& current,
    core::Revision expected_revision,
    DecalPlacement desired) {
    if (auto result = current.validate(); !result) {
        return core::Result<DecalPlacement>::failure(result.error());
    }
    if (current.revision != expected_revision) {
        return core::Result<DecalPlacement>::failure(stale(
            "decal placement revision changed before edit"));
    }
    if (expected_revision.exhausted() || desired.id != current.id ||
        desired.revision != expected_revision.next()) {
        return core::Result<DecalPlacement>::failure(invalid(
            "decal edit must preserve identity and advance exactly one revision"));
    }
    if (auto result = desired.validate(); !result) {
        return core::Result<DecalPlacement>::failure(result.error());
    }
    return core::Result<DecalPlacement>::success(std::move(desired));
}

core::Result<std::string> serialize_decal_placement(const DecalPlacement& placement) {
    if (auto result = placement.validate(); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "CARTO_DECAL_PLACEMENT 1\n"
        << placement.id << ' ' << placement.revision.value() << ' '
        << placement.target_mesh_asset << ' ' << placement.target_mesh_revision.value() << ' '
        << placement.target_mesh_digest.hex() << ' ' << placement.material_id << '\n'
        << placement.center.x << ' ' << placement.center.y << ' ' << placement.center.z << ' '
        << placement.normal.x << ' ' << placement.normal.y << ' ' << placement.normal.z << ' '
        << placement.tangent.x << ' ' << placement.tangent.y << ' ' << placement.tangent.z << '\n'
        << placement.width << ' ' << placement.height << ' ' << placement.depth << ' '
        << placement.minimum_normal_dot << '\n';
    const std::string bytes = output.str();
    if (bytes.size() > DecalPlacement::kMaxSerializedBytes) {
        return core::Result<std::string>::failure(validation(
            "serialized decal placement exceeds its byte limit"));
    }
    return core::Result<std::string>::success(bytes);
}

core::Result<DecalPlacement> deserialize_decal_placement(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > DecalPlacement::kMaxSerializedBytes) {
        return core::Result<DecalPlacement>::failure(invalid(
            "decal placement record is empty or exceeds its byte limit"));
    }
    std::istringstream input{std::string(bytes)};
    input.imbue(std::locale::classic());
    std::string magic;
    std::uint32_t version = 0U;
    DecalPlacement placement;
    std::uint64_t target_asset = 0U;
    std::uint64_t material_id = 0U;
    std::string digest;
    if (!(input >> magic >> version) || magic != "CARTO_DECAL_PLACEMENT" || version != 1U ||
        !read_unsigned(input, placement.id) || !read_revision(input, placement.revision) ||
        !read_unsigned(input, target_asset) || !read_revision(input, placement.target_mesh_revision) ||
        !(input >> digest) || !read_unsigned(input, material_id) ||
        !(input >> placement.center.x >> placement.center.y >> placement.center.z
            >> placement.normal.x >> placement.normal.y >> placement.normal.z
            >> placement.tangent.x >> placement.tangent.y >> placement.tangent.z
            >> placement.width >> placement.height >> placement.depth
            >> placement.minimum_normal_dot)) {
        return core::Result<DecalPlacement>::failure(validation(
            "decal placement record is malformed"));
    }
    input >> std::ws;
    if (!input.eof()) {
        return core::Result<DecalPlacement>::failure(validation(
            "decal placement record contains trailing data"));
    }
    auto parsed_digest = assets::Sha256Digest::from_hex(digest);
    if (!parsed_digest) return core::Result<DecalPlacement>::failure(parsed_digest.error());
    placement.target_mesh_asset = target_asset;
    placement.target_mesh_digest = parsed_digest.value();
    placement.material_id = material_id;
    if (auto result = placement.validate(); !result) {
        return core::Result<DecalPlacement>::failure(result.error());
    }
    return core::Result<DecalPlacement>::success(std::move(placement));
}

core::Result<core::Revision> stage_decal_placement(
    project::ProjectTransaction& transaction,
    const DecalPlacement& placement,
    core::Revision expected_project_placement_revision) {
    if ((expected_project_placement_revision == core::Revision{} &&
         placement.revision != core::Revision{1U}) ||
        (expected_project_placement_revision != core::Revision{} &&
         (expected_project_placement_revision.exhausted() ||
          placement.revision != expected_project_placement_revision.next()))) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "decal placement revision must advance exactly once from the project record"));
    }
    const auto encoded = serialize_decal_placement(placement);
    if (!encoded) return core::Result<core::Revision>::failure(encoded.error());
    const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.value().data());
    const auto digest = assets::sha256(std::span<const std::uint8_t>(data, encoded.value().size()));
    return transaction.set_decal_placement(
        placement.id, expected_project_placement_revision, encoded.value(), digest);
}

core::Result<DecalPlacement> load_decal_placement(
    const project::ProjectDocument& document,
    std::uint64_t placement_id) {
    const auto stored = document.decal_placements().find(placement_id);
    if (stored == document.decal_placements().end()) {
        return core::Result<DecalPlacement>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "decal placement does not exist in the project"));
    }
    const auto decoded = deserialize_decal_placement(stored->second.bytes);
    if (!decoded || decoded.value().id != placement_id ||
        decoded.value().revision != stored->second.revision) {
        return core::Result<DecalPlacement>::failure(validation(
            "stored decal placement identity or revision does not match its project record"));
    }
    return decoded;
}

} // namespace carto::production
