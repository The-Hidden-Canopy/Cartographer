#include <carto/geometry/mesh.hpp>
#include <carto/surface/qem.hpp>

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            throw TestFailure(std::string("requirement failed: ") + #condition);            \
        }                                                                                   \
    } while (false)

carto::geometry::EditableMesh grid_mesh(int side) {
    carto::geometry::EditableMesh mesh;
    std::vector<carto::geometry::VertexId> vertices;
    vertices.reserve(static_cast<std::size_t>(side * side));
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const auto vertex = mesh.add_vertex({static_cast<double>(x), static_cast<double>(y),
                                                 0.05 * static_cast<double>((x + 2 * y) % 3)});
            REQUIRE(vertex);
            vertices.push_back(vertex.value());
        }
    }
    for (int y = 0; y < side - 1; ++y) {
        for (int x = 0; x < side - 1; ++x) {
            const auto at = [=](int px, int py) {
                return vertices[static_cast<std::size_t>(py * side + px)];
            };
            REQUIRE(mesh.add_face({at(x, y), at(x + 1, y), at(x + 1, y + 1)}));
            REQUIRE(mesh.add_face({at(x, y), at(x + 1, y + 1), at(x, y + 1)}));
        }
    }
    return mesh;
}

void qem_is_deterministic_and_preserves_boundary() {
    const auto compiled = grid_mesh(3).compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{601U}, "grid", "concrete");
    REQUIRE(domain);
    carto::surface::SurfaceQemOptions options;
    options.target_triangles = 6U;
    options.max_collapses = 4U;
    const auto first = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options);
    const auto second = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options);
    REQUIRE(first && second);
    REQUIRE(first.value().digest == second.value().digest);
    REQUIRE(first.value().triangle_count() == 6U);
    REQUIRE(!first.value().collapses.empty());
    REQUIRE(first.value().validate(compiled.value(), domain.value()));
    for (const auto& collapse : first.value().collapses) REQUIRE(collapse.validate());

    auto forged = first.value();
    forged.digest[0] = forged.digest[0] == '0' ? '1' : '0';
    REQUIRE(!forged.validate(compiled.value(), domain.value()));
}

void qem_rejects_unsafe_or_unreachable_requests() {
    const auto compiled = grid_mesh(2).compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{602U});
    REQUIRE(domain);

    carto::surface::SurfaceQemOptions invalid;
    invalid.target_triangles = 0U;
    REQUIRE(!invalid.validate());
    invalid.target_triangles = 99U;
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), invalid));

    carto::surface::SurfaceQemOptions boundary_protected;
    boundary_protected.target_triangles = 1U;
    boundary_protected.max_collapses = 8U;
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), boundary_protected));

    auto stale = domain.value();
    stale.source_revision = carto::core::Revision{777U};
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), stale, boundary_protected));
}

void qem_preserves_source_bound_policy_edges() {
    auto editable = grid_mesh(4);
    const auto topology = editable.topology();
    REQUIRE(topology);
    std::vector<carto::surface::SurfaceQemProtectedEdge> protected_edges;
    for (const auto& edge : topology.value().edges) {
        if (!edge.second_half_edge.has_value()) continue;
        const auto policy = protected_edges.empty()
            ? carto::surface::SurfaceQemBoundaryPolicy::material
            : protected_edges.size() == 1U
                ? carto::surface::SurfaceQemBoundaryPolicy::constraint
                : carto::surface::SurfaceQemBoundaryPolicy::fracture;
        protected_edges.push_back({edge.id, policy});
        if (protected_edges.size() == 3U) break;
    }
    REQUIRE(protected_edges.size() == 3U);

    const auto compiled = editable.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{603U}, "grid", "stone");
    REQUIRE(domain);

    carto::surface::SurfaceQemOptions options;
    options.target_triangles = 16U;
    options.max_collapses = 2U;
    options.protected_edges = protected_edges;
    REQUIRE(options.validate());
    const auto result = carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options);
    REQUIRE(result);
    REQUIRE(result.value().protected_edges == options.protected_edges);
    REQUIRE(result.value().options_digest == options.digest().hex());
    REQUIRE(result.value().validate(compiled.value(), domain.value()));

    auto unknown_edge = options;
    unknown_edge.protected_edges = {{carto::geometry::EdgeId{999999U},
        carto::surface::SurfaceQemBoundaryPolicy::constraint}};
    REQUIRE(unknown_edge.validate());
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), unknown_edge));

    auto duplicate_edge = options;
    duplicate_edge.protected_edges.push_back(duplicate_edge.protected_edges.front());
    std::sort(duplicate_edge.protected_edges.begin(), duplicate_edge.protected_edges.end());
    REQUIRE(!duplicate_edge.validate());

    auto invalid_policy = options;
    invalid_policy.protected_edges.front().policy =
        static_cast<carto::surface::SurfaceQemBoundaryPolicy>(255U);
    REQUIRE(!invalid_policy.validate());

    auto conflicting_edges = options;
    conflicting_edges.protected_edges.push_back({
        conflicting_edges.protected_edges.front().edge,
        carto::surface::SurfaceQemBoundaryPolicy::fracture});
    std::sort(conflicting_edges.protected_edges.begin(), conflicting_edges.protected_edges.end());
    REQUIRE(!conflicting_edges.validate());
}

void qem_infers_and_binds_attribute_boundaries() {
    auto editable = grid_mesh(3);
    const auto initial = editable.compile();
    REQUIRE(initial);

    std::map<carto::geometry::EdgeId, std::set<carto::geometry::FaceId>> incident_faces;
    for (std::size_t triangle = 0U; triangle < initial.value().triangle_faces.size(); ++triangle) {
        for (const auto& edge : initial.value().triangle_edges[triangle]) {
            if (edge.has_value()) {
                incident_faces[*edge].insert(initial.value().triangle_faces[triangle]);
            }
        }
    }
    const auto material_edge = std::find_if(
        incident_faces.begin(), incident_faces.end(),
        [](const auto& entry) { return entry.second.size() == 2U; });
    REQUIRE(material_edge != incident_faces.end());

    const auto faces = editable.faces_sorted();
    const auto edges = editable.edges_sorted(editable.edge_count());
    std::vector<carto::attributes::AttributeValue> material_values(
        faces.size(), std::uint32_t{1U});
    const auto changed_face = *material_edge->second.begin();
    const auto changed_face_position = std::lower_bound(
        faces.begin(), faces.end(), changed_face,
        [](const auto& face, carto::geometry::FaceId id) { return face.id < id; });
    REQUIRE(changed_face_position != faces.end() && changed_face_position->id == changed_face);
    material_values[static_cast<std::size_t>(changed_face_position - faces.begin())] =
        std::uint32_t{2U};
    std::vector<carto::attributes::AttributeValue> constraint_region_values(
        faces.size(), std::uint32_t{1U});
    constraint_region_values[
        static_cast<std::size_t>(changed_face_position - faces.begin())] = std::uint32_t{2U};

    const auto constraint_edge = std::find_if(
        edges.begin(), edges.end(), [&material_edge](const auto& edge) {
            return edge.id != material_edge->first;
        });
    REQUIRE(constraint_edge != edges.end());
    std::vector<carto::attributes::AttributeValue> constraint_values(
        edges.size(), false);
    const auto constraint_position = std::lower_bound(
        edges.begin(), edges.end(), constraint_edge->id,
        [](const auto& edge, carto::geometry::EdgeId id) { return edge.id < id; });
    REQUIRE(constraint_position != edges.end() && constraint_position->id == constraint_edge->id);
    constraint_values[static_cast<std::size_t>(constraint_position - edges.begin())] = true;

    carto::attributes::AttributeSet payload({
        {carto::attributes::AttributeDomain::face, faces.size()},
        {carto::attributes::AttributeDomain::edge, edges.size()}});
    REQUIRE(payload.add_layer({
        {"surface.constraint_region", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32},
        std::move(constraint_region_values)}));
    REQUIRE(payload.add_layer({
        {"surface.material_group", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::uint32},
        std::move(material_values)}));
    REQUIRE(payload.add_layer({
        {"surface.keep_edge", carto::attributes::AttributeDomain::edge,
         carto::attributes::AttributeType::boolean},
        std::move(constraint_values)}));
    REQUIRE(editable.set_attribute_payload(std::move(payload), {}));

    const auto compiled = editable.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{604U}, "grid", "stone");
    REQUIRE(domain);

    carto::surface::SurfaceQemOptions options;
    options.target_triangles = 8U;
    options.max_collapses = 1U;
    options.boundary_attribute_rules = {
        {"surface.keep_edge", carto::surface::SurfaceQemBoundaryPolicy::constraint},
        {"surface.material_group", carto::surface::SurfaceQemBoundaryPolicy::material}};
    REQUIRE(options.validate());
    const auto result = carto::surface::build_constrained_qem(
        editable, domain.value(), options);
    REQUIRE(result);
    REQUIRE(result.value().validate(compiled.value(), domain.value()));
    REQUIRE(std::is_sorted(result.value().protected_edges.begin(),
        result.value().protected_edges.end()));
    REQUIRE(std::find(result.value().protected_edges.begin(), result.value().protected_edges.end(),
        carto::surface::SurfaceQemProtectedEdge{
            material_edge->first, carto::surface::SurfaceQemBoundaryPolicy::material}) !=
        result.value().protected_edges.end());
    REQUIRE(std::find(result.value().protected_edges.begin(), result.value().protected_edges.end(),
        carto::surface::SurfaceQemProtectedEdge{
            constraint_edge->id, carto::surface::SurfaceQemBoundaryPolicy::constraint}) !=
        result.value().protected_edges.end());
    auto effective_options = options;
    effective_options.protected_edges = result.value().protected_edges;
    REQUIRE(result.value().options_digest == effective_options.digest().hex());
    const auto repeat = carto::surface::build_constrained_qem(
        editable, domain.value(), options);
    REQUIRE(repeat && repeat.value().digest == result.value().digest);
    REQUIRE(!carto::surface::build_constrained_qem(
        compiled.value(), domain.value(), options));

    auto missing_layer = options;
    missing_layer.boundary_attribute_rules = {{
        "surface.missing", carto::surface::SurfaceQemBoundaryPolicy::material}};
    REQUIRE(!carto::surface::build_constrained_qem(
        editable, domain.value(), missing_layer));
    auto wrong_domain = options;
    wrong_domain.boundary_attribute_rules = {{
        "surface.keep_edge", carto::surface::SurfaceQemBoundaryPolicy::material}};
    REQUIRE(!carto::surface::build_constrained_qem(
        editable, domain.value(), wrong_domain));
    auto invalid_rules = options;
    invalid_rules.boundary_attribute_rules.push_back(invalid_rules.boundary_attribute_rules.front());
    std::sort(invalid_rules.boundary_attribute_rules.begin(),
        invalid_rules.boundary_attribute_rules.end());
    REQUIRE(!invalid_rules.validate());

    auto conflicting = options;
    conflicting.protected_edges = {{
        material_edge->first, carto::surface::SurfaceQemBoundaryPolicy::constraint}};
    REQUIRE(!carto::surface::build_constrained_qem(
        editable, domain.value(), conflicting));

    auto conflicting_rules = options;
    conflicting_rules.boundary_attribute_rules = {
        {"surface.constraint_region", carto::surface::SurfaceQemBoundaryPolicy::constraint},
        {"surface.material_group", carto::surface::SurfaceQemBoundaryPolicy::material}};
    REQUIRE(conflicting_rules.validate());
    REQUIRE(!carto::surface::build_constrained_qem(
        editable, domain.value(), conflicting_rules));

    const auto stale_domain = domain.value();
    REQUIRE(editable.clear_attribute_payload());
    REQUIRE(!carto::surface::build_constrained_qem(
        editable, stale_domain, options));
}

void qem_rejects_unsupported_boundary_attribute_types() {
    auto editable = grid_mesh(3);
    const auto faces = editable.faces_sorted();
    const auto edges = editable.edges_sorted(editable.edge_count());
    carto::attributes::AttributeSet payload({
        {carto::attributes::AttributeDomain::face, faces.size()},
        {carto::attributes::AttributeDomain::edge, edges.size()}});
    REQUIRE(payload.add_layer({
        {"surface.float_material", carto::attributes::AttributeDomain::face,
         carto::attributes::AttributeType::float32},
        std::vector<carto::attributes::AttributeValue>(faces.size(), 1.0F)}));
    REQUIRE(payload.add_layer({
        {"surface.float_keep", carto::attributes::AttributeDomain::edge,
         carto::attributes::AttributeType::float32},
        std::vector<carto::attributes::AttributeValue>(edges.size(), 0.0F)}));
    REQUIRE(editable.set_attribute_payload(std::move(payload), {}));
    const auto compiled = editable.compile();
    REQUIRE(compiled);
    const auto domain = carto::surface::build_surface_domain(
        compiled.value(), carto::surface::SurfaceDomainId{605U});
    REQUIRE(domain);

    carto::surface::SurfaceQemOptions options;
    options.target_triangles = 8U;
    options.max_collapses = 1U;
    options.boundary_attribute_rules = {{
        "surface.float_material", carto::surface::SurfaceQemBoundaryPolicy::material}};
    REQUIRE(!carto::surface::build_constrained_qem(editable, domain.value(), options));
    options.boundary_attribute_rules = {{
        "surface.float_keep", carto::surface::SurfaceQemBoundaryPolicy::constraint}};
    REQUIRE(!carto::surface::build_constrained_qem(editable, domain.value(), options));
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"QEM is deterministic and preserves boundary", qem_is_deterministic_and_preserves_boundary},
        {"QEM rejects unsafe or unreachable requests", qem_rejects_unsafe_or_unreachable_requests},
        {"QEM preserves source-bound policy edges", qem_preserves_source_bound_policy_edges},
        {"QEM infers and binds attribute boundaries", qem_infers_and_binds_attribute_boundaries},
        {"QEM rejects unsupported boundary attribute types", qem_rejects_unsupported_boundary_attribute_types},
    };
    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
