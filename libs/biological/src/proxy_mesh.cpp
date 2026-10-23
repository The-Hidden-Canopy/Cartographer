#include <carto/biological/proxy_mesh.hpp>

#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace carto::biological {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::string canonical_product(const BiologicalAuthoringDocument& document,
    StructureId structure, const geometry::EditableMesh& mesh) {
    const auto authoring_digest = document.canonical_digest();
    if (!authoring_digest) return {};
    std::ostringstream output;
    output << "CARTOGRAPHER_ELLIPSOID_PROXY 1\n"
           << std::quoted(document.source.source_namespace) << ' '
           << document.source.asset_id << ' ' << document.source.revision.value() << ' '
           << document.source.digest.hex() << ' ' << authoring_digest.value().hex() << ' '
           << structure.value << '\n' << std::setprecision(17);
    for (const auto& vertex : mesh.vertices_sorted()) {
        output << "V " << vertex.id.value << ' ' << vertex.position.x << ' '
               << vertex.position.y << ' ' << vertex.position.z << '\n';
    }
    for (const auto& face : mesh.faces_sorted()) {
        output << "F " << face.id.value << ' ' << face.vertices.size();
        for (const auto vertex : face.vertices) output << ' ' << vertex.value;
        output << '\n';
    }
    return output.str();
}

core::Result<geometry::EditableMesh> make_ellipsoid(const EllipsoidProxyProfile& profile) {
    if (auto result = profile.validate(); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    const std::uint32_t longitude = profile.longitude_segments;
    const std::uint32_t latitude = profile.latitude_segments;
    const std::size_t vertex_count = 2U +
        static_cast<std::size_t>(longitude) * (latitude - 1U);
    const std::size_t face_count = static_cast<std::size_t>(longitude) * 2U *
        (latitude - 1U);
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.reserve(vertex_count);
    faces.reserve(face_count);

    const auto add_vertex = [&vertices](core::Vec3d position) {
        const auto id = geometry::VertexId{static_cast<std::uint64_t>(vertices.size() + 1U)};
        vertices.push_back(geometry::Vertex{id, position});
        return id;
    };
    const auto north = add_vertex({profile.center.x, profile.center.y + profile.radii.y,
        profile.center.z});
    std::vector<std::vector<geometry::VertexId>> rings;
    rings.reserve(latitude - 1U);
    constexpr double kPi = 3.141592653589793238462643383279502884;
    for (std::uint32_t ring_index = 1U; ring_index < latitude; ++ring_index) {
        const double phi = kPi * static_cast<double>(ring_index) /
            static_cast<double>(latitude);
        const double sin_phi = std::sin(phi);
        std::vector<geometry::VertexId> ring;
        ring.reserve(longitude);
        for (std::uint32_t segment = 0U; segment < longitude; ++segment) {
            const double theta = 2.0 * kPi * static_cast<double>(segment) /
                static_cast<double>(longitude);
            ring.push_back(add_vertex({
                profile.center.x + profile.radii.x * sin_phi * std::cos(theta),
                profile.center.y + profile.radii.y * std::cos(phi),
                profile.center.z + profile.radii.z * sin_phi * std::sin(theta),
            }));
        }
        rings.push_back(std::move(ring));
    }
    const auto south = add_vertex({profile.center.x, profile.center.y - profile.radii.y,
        profile.center.z});
    const auto add_face = [&faces](std::vector<geometry::VertexId> indices) {
        faces.push_back(geometry::Face{
            geometry::FaceId{static_cast<std::uint64_t>(faces.size() + 1U)},
            std::move(indices)});
    };
    for (std::uint32_t segment = 0U; segment < longitude; ++segment) {
        const auto next = (segment + 1U) % longitude;
        add_face({north, rings.front()[next], rings.front()[segment]});
    }
    for (std::size_t ring = 0U; ring + 1U < rings.size(); ++ring) {
        for (std::uint32_t segment = 0U; segment < longitude; ++segment) {
            const auto next = (segment + 1U) % longitude;
            add_face({rings[ring][segment], rings[ring][next],
                rings[ring + 1U][next], rings[ring + 1U][segment]});
        }
    }
    for (std::uint32_t segment = 0U; segment < longitude; ++segment) {
        const auto next = (segment + 1U) % longitude;
        add_face({south, rings.back()[segment], rings.back()[next]});
    }

    geometry::EditableMesh mesh;
    if (auto result = mesh.insert_bulk(std::move(vertices), std::move(faces)); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    if (auto result = mesh.validate(); !result) {
        return core::Result<geometry::EditableMesh>::failure(result.error());
    }
    return core::Result<geometry::EditableMesh>::success(std::move(mesh));
}

bool same_mesh(const geometry::EditableMesh& left, const geometry::EditableMesh& right) {
    if (left.vertex_count() != right.vertex_count() ||
        left.face_count() != right.face_count()) return false;
    const auto left_vertices = left.vertices_sorted();
    const auto right_vertices = right.vertices_sorted();
    for (std::size_t index = 0U; index < left_vertices.size(); ++index) {
        const auto& a = left_vertices[index];
        const auto& b = right_vertices[index];
        if (a.id != b.id || a.position.x != b.position.x ||
            a.position.y != b.position.y || a.position.z != b.position.z) return false;
    }
    const auto left_faces = left.faces_sorted();
    const auto right_faces = right.faces_sorted();
    for (std::size_t index = 0U; index < left_faces.size(); ++index) {
        if (left_faces[index].id != right_faces[index].id ||
            left_faces[index].vertices != right_faces[index].vertices) return false;
    }
    return true;
}

} // namespace

core::Result<ProxyMeshProduct> compile_ellipsoid_proxy(
    const BiologicalAuthoringDocument& document, StructureId structure) {
    if (auto result = document.validate(); !result) {
        return core::Result<ProxyMeshProduct>::failure(result.error());
    }
    const auto found = document.structures().find(structure);
    if (found == document.structures().end() || !found->second.proxy_surface.has_value()) {
        return core::Result<ProxyMeshProduct>::failure(invalid(
            "biological structure has no explicit ellipsoid proxy profile"));
    }
    auto mesh = make_ellipsoid(*found->second.proxy_surface);
    if (!mesh) return core::Result<ProxyMeshProduct>::failure(mesh.error());
    const std::string canonical = canonical_product(document, structure, mesh.value());
    if (canonical.empty()) {
        return core::Result<ProxyMeshProduct>::failure(invalid(
            "biological source cannot be canonically validated"));
    }
    ProxyMeshProduct product{
        .source = document.source,
        .structure = structure,
        .mesh = std::move(mesh).value(),
        .product_digest = digest_text(canonical),
    };
    return core::Result<ProxyMeshProduct>::success(std::move(product));
}

core::Result<void> ProxyMeshProduct::validate_against(
    const BiologicalAuthoringDocument& document) const {
    if (source.source_namespace != document.source.source_namespace ||
        source.asset_id != document.source.asset_id ||
        source.revision != document.source.revision || source.digest != document.source.digest) {
        return core::Result<void>::failure(stale(
            "biological proxy product source identity or revision is stale"));
    }
    auto expected = compile_ellipsoid_proxy(document, structure);
    if (!expected) return core::Result<void>::failure(expected.error());
    if (!same_mesh(mesh, expected.value().mesh) ||
        product_digest != expected.value().product_digest) {
        return core::Result<void>::failure(stale(
            "biological proxy product geometry or digest differs from its source"));
    }
    return core::Result<void>::success();
}

} // namespace carto::biological
