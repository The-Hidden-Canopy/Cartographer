#include <carto/physical/geometry_evidence.hpp>

#include <iomanip>
#include <sstream>
#include <utility>

namespace carto::physical {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PHYSICAL_GEOMETRY_EVIDENCE";

} // namespace

core::Result<void> ThicknessEvidence::validate() const {
    if (!detail::finite_nonnegative(minimum) || !detail::finite_nonnegative(maximum) ||
        !detail::finite_nonnegative(mean) || maximum <= 0.0 || minimum > maximum ||
        mean < minimum || mean > maximum || samples == 0U) {
        return core::Result<void>::failure(detail::invalid(
            "thickness evidence bounds or samples are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> GeometryEvidence::validate() const {
    if (mesh_identity == 0U || source_revision.exhausted()) {
        return core::Result<void>::failure(detail::invalid(
            "geometry evidence identity or source revision is invalid"));
    }
    if (!detail::finite_positive(signed_volume) || !detail::finite_positive(volume) ||
        !detail::finite_positive(surface_area) || !winding_consistent ||
        triangle_count == 0U || signed_volume <= 0.0) {
        return core::Result<void>::failure(detail::validation(
            "geometry evidence does not describe a consistently wound closed mesh"));
    }
    if (!volume_centroid.finite()) {
        return core::Result<void>::failure(detail::invalid(
            "geometry evidence volume centroid is invalid"));
    }
    if (!unit_density_inertia.finite() || !unit_density_inertia.positive_diagonal()) {
        return core::Result<void>::failure(detail::validation(
            "geometry evidence inertia tensor is invalid"));
    }
    if (auto result = thickness.validate(); !result) return result;
    if (static_cast<std::uint64_t>(thickness.samples) != triangle_count) {
        return core::Result<void>::failure(detail::validation(
            "geometry evidence thickness samples do not match its triangle count"));
    }
    return core::Result<void>::success();
}

std::string GeometryEvidence::serialize() const {
    std::ostringstream output;
    detail::encode_evidence(output, *this);
    return output.str();
}

core::Result<assets::Sha256Digest> GeometryEvidence::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string canonical = serialize();
    return core::Result<assets::Sha256Digest>::success(assets::sha256(
        std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()}));
}

core::Result<GeometryEvidence> GeometryEvidence::deserialize(std::string_view text) {
    if (auto result = detail::check_text_size(text, kMaxSerializedBytes, "geometry evidence");
        !result) {
        return core::Result<GeometryEvidence>::failure(result.error());
    }
    std::istringstream input{std::string(text)};
    GeometryEvidence evidence;
    if (auto result = detail::decode_evidence(input, evidence); !result) {
        return core::Result<GeometryEvidence>::failure(result.error());
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<GeometryEvidence>::failure(detail::parse_error(
            "geometry evidence contains trailing unbound data"));
    }
    if (auto result = evidence.validate(); !result) {
        return core::Result<GeometryEvidence>::failure(result.error());
    }
    return core::Result<GeometryEvidence>::success(std::move(evidence));
}

namespace detail {

void encode_evidence(std::ostream& output, const GeometryEvidence& evidence) {
    output << kMagic << ' ' << GeometryEvidence::kSchemaVersion << '\n';
    output << std::setprecision(17);
    output << "MESH " << evidence.mesh_identity << ' ';
    write_revision(output, evidence.source_revision) << ' ';
    write_digest(output, evidence.mesh_content_digest) << '\n';
    output << "VOLUME " << evidence.signed_volume << ' ' << evidence.volume << ' '
           << evidence.surface_area << ' ' << (evidence.winding_consistent ? 1 : 0) << ' '
           << evidence.triangle_count << '\n';
    output << "CENTROID " << evidence.volume_centroid.x << ' '
           << evidence.volume_centroid.y << ' ' << evidence.volume_centroid.z << '\n';
    output << "INERTIA ";
    write_tensor(output, evidence.unit_density_inertia) << '\n';
    output << "THICKNESS " << evidence.thickness.minimum << ' '
           << evidence.thickness.maximum << ' ' << evidence.thickness.mean << ' '
           << evidence.thickness.samples << ' ' << (evidence.thickness.ambiguous ? 1 : 0)
           << '\n';
    output << "END\n";
}

core::Result<void> decode_evidence(std::istream& input, GeometryEvidence& evidence) {
    if (auto result = read_frame(input, kMagic, GeometryEvidence::kSchemaVersion);
        !result) {
        return result;
    }
    if (!require_token(input, "MESH") ||
        !read_integer(input, evidence.mesh_identity) ||
        !read_revision(input, evidence.source_revision) ||
        !read_digest(input, evidence.mesh_content_digest)) {
        return core::Result<void>::failure(parse_error(
            "geometry evidence mesh identity record is malformed"));
    }
    unsigned winding = 0U;
    if (!require_token(input, "VOLUME") || !read_double(input, evidence.signed_volume) ||
        !read_double(input, evidence.volume) || !read_double(input, evidence.surface_area) ||
        !read_integer(input, winding) || !read_integer(input, evidence.triangle_count)) {
        return core::Result<void>::failure(parse_error(
            "geometry evidence volume record is malformed"));
    }
    evidence.winding_consistent = winding != 0U;
    if (!require_token(input, "CENTROID") || !read_double(input, evidence.volume_centroid.x) ||
        !read_double(input, evidence.volume_centroid.y) ||
        !read_double(input, evidence.volume_centroid.z)) {
        return core::Result<void>::failure(parse_error(
            "geometry evidence centroid record is malformed"));
    }
    if (!require_token(input, "INERTIA") || !read_tensor(input, evidence.unit_density_inertia)) {
        return core::Result<void>::failure(parse_error(
            "geometry evidence inertia record is malformed"));
    }
    unsigned ambiguous = 0U;
    if (!require_token(input, "THICKNESS") ||
        !read_double(input, evidence.thickness.minimum) ||
        !read_double(input, evidence.thickness.maximum) ||
        !read_double(input, evidence.thickness.mean) ||
        !read_integer(input, evidence.thickness.samples) ||
        !read_integer(input, ambiguous)) {
        return core::Result<void>::failure(parse_error(
            "geometry evidence thickness record is malformed"));
    }
    evidence.thickness.ambiguous = ambiguous != 0U;
    return read_end(input, kMagic);
}

} // namespace detail

core::Result<GeometryEvidence> derive_geometry_evidence(
    std::uint64_t mesh_identity,
    const geometry::CompiledMesh& mesh) {
    if (mesh_identity == 0U) {
        return core::Result<GeometryEvidence>::failure(detail::invalid(
            "geometry evidence requires a non-zero mesh identity"));
    }
    if (!mesh.valid()) {
        return core::Result<GeometryEvidence>::failure(detail::invalid(
            "geometry evidence cannot derive from an invalid compiled mesh"));
    }
    if (mesh.source_revision.exhausted()) {
        return core::Result<GeometryEvidence>::failure(detail::invalid(
            "geometry evidence source revision is exhausted"));
    }
    if (mesh.indices.empty()) {
        return core::Result<GeometryEvidence>::failure(detail::validation(
            "geometry evidence requires at least one triangle"));
    }

    const core::Vec3d reference = {
        (mesh.bounds.minimum.x + mesh.bounds.maximum.x) * 0.5,
        (mesh.bounds.minimum.y + mesh.bounds.maximum.y) * 0.5,
        (mesh.bounds.minimum.z + mesh.bounds.maximum.z) * 0.5,
    };

    double signed_volume = 0.0;
    double surface_area = 0.0;
    core::Vec3d first_moment{};
    // Raw second moment of volume about the reference point: C = integral of
    // x x^T dV. Accumulated per tetrahedron (reference, a, b, c) in triangle
    // order so the result is byte-for-byte deterministic.
    double c_xx = 0.0, c_xy = 0.0, c_xz = 0.0;
    double c_yy = 0.0, c_yz = 0.0, c_zz = 0.0;
    double thickness_min = 0.0;
    double thickness_max = 0.0;
    double thickness_sum = 0.0;
    bool first_triangle = true;

    const std::size_t triangle_count = mesh.indices.size() / 3U;
    for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
        const core::Vec3d a = mesh.positions[mesh.indices[triangle * 3U + 0U]] - reference;
        const core::Vec3d b = mesh.positions[mesh.indices[triangle * 3U + 1U]] - reference;
        const core::Vec3d c = mesh.positions[mesh.indices[triangle * 3U + 2U]] - reference;

        const core::Vec3d cross_bc = core::cross(b, c);
        const double tet_volume = core::dot(a, cross_bc) / 6.0;
        const core::Vec3d edge_ab = b - a;
        const core::Vec3d edge_ac = c - a;
        const double area = 0.5 * core::cross(edge_ab, edge_ac).length();

        signed_volume += tet_volume;
        surface_area += area;
        first_moment = first_moment + (a + b + c) * (tet_volume / 4.0);

        // Integral of v v^T over the tetrahedron with vertices {0, a, b, c}:
        // V/20 * (2 * sum(v_i v_i^T) + sum(i!=j, v_i v_j^T)); the reference
        // vertex contributes zero. (Uniform-simplex second moment with
        // E[x_i x_j] = (1 + delta_ij) / ((n+1)(n+2)).)
        c_xx += tet_volume / 20.0 *
            (2.0 * (a.x * a.x + b.x * b.x + c.x * c.x) +
             2.0 * (a.x * b.x + a.x * c.x + b.x * c.x));
        c_xy += tet_volume / 20.0 *
            (2.0 * (a.x * a.y + b.x * b.y + c.x * c.y) +
             (a.x * b.y + b.x * a.y) + (a.x * c.y + c.x * a.y) +
             (b.x * c.y + c.x * b.y));
        c_xz += tet_volume / 20.0 *
            (2.0 * (a.x * a.z + b.x * b.z + c.x * c.z) +
             (a.x * b.z + b.x * a.z) + (a.x * c.z + c.x * a.z) +
             (b.x * c.z + c.x * b.z));
        c_yy += tet_volume / 20.0 *
            (2.0 * (a.y * a.y + b.y * b.y + c.y * c.y) +
             2.0 * (a.y * b.y + a.y * c.y + b.y * c.y));
        c_yz += tet_volume / 20.0 *
            (2.0 * (a.y * a.z + b.y * b.z + c.y * c.z) +
             (a.y * b.z + b.y * a.z) + (a.y * c.z + c.y * a.z) +
             (b.y * c.z + c.y * b.z));
        c_zz += tet_volume / 20.0 *
            (2.0 * (a.z * a.z + b.z * b.z + c.z * c.z) +
             2.0 * (a.z * b.z + a.z * c.z + b.z * c.z));

        // Face plane distance from the reference point: V_tet = area * h / 3.
        const double thickness = area > 0.0 ? std::abs(tet_volume) * 3.0 / area : 0.0;
        if (first_triangle) {
            thickness_min = thickness;
            thickness_max = thickness;
            first_triangle = false;
        } else {
            thickness_min = thickness < thickness_min ? thickness : thickness_min;
            thickness_max = thickness > thickness_max ? thickness : thickness_max;
        }
        thickness_sum += thickness;
    }

    if (!(signed_volume > 0.0) || !std::isfinite(signed_volume) ||
        !std::isfinite(surface_area) || surface_area <= 0.0) {
        return core::Result<GeometryEvidence>::failure(detail::validation(
            "compiled mesh winding is inconsistent or the mesh is not closed"));
    }
    const double volume = signed_volume;
    const core::Vec3d centroid = reference + first_moment * (1.0 / volume);

    // Parallel axis to the volume centroid: C_c = C_ref - V * c c^T.
    const double rc_x = centroid.x - reference.x;
    const double rc_y = centroid.y - reference.y;
    const double rc_z = centroid.z - reference.z;
    const double cc_xx = c_xx - volume * rc_x * rc_x;
    const double cc_xy = c_xy - volume * rc_x * rc_y;
    const double cc_xz = c_xz - volume * rc_x * rc_z;
    const double cc_yy = c_yy - volume * rc_y * rc_y;
    const double cc_yz = c_yz - volume * rc_y * rc_z;
    const double cc_zz = c_zz - volume * rc_z * rc_z;

    // Inertia about the centroid for unit density: I = tr(C_c) I - C_c.
    GeometryEvidence evidence;
    evidence.mesh_identity = mesh_identity;
    evidence.source_revision = mesh.source_revision;
    evidence.triangle_count = static_cast<std::uint64_t>(triangle_count);
    evidence.signed_volume = signed_volume;
    evidence.volume = volume;
    evidence.surface_area = surface_area;
    evidence.winding_consistent = true;
    evidence.volume_centroid = centroid;
    evidence.unit_density_inertia.xx = cc_yy + cc_zz;
    evidence.unit_density_inertia.xy = -cc_xy;
    evidence.unit_density_inertia.xz = -cc_xz;
    evidence.unit_density_inertia.yy = cc_xx + cc_zz;
    evidence.unit_density_inertia.yz = -cc_yz;
    evidence.unit_density_inertia.zz = cc_xx + cc_yy;
    evidence.thickness.minimum = thickness_min;
    evidence.thickness.maximum = thickness_max;
    evidence.thickness.mean = thickness_sum / static_cast<double>(triangle_count);
    evidence.thickness.samples = static_cast<std::uint32_t>(triangle_count);
    evidence.thickness.ambiguous = false;

    if (auto result = evidence.validate(); !result) {
        return core::Result<GeometryEvidence>::failure(result.error());
    }
    return core::Result<GeometryEvidence>::success(std::move(evidence));
}

} // namespace carto::physical
