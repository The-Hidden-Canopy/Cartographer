#include <carto/mesh_attributes/provenance.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace carto::mesh_attributes {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

template <typename Record, typename IdSelector>
std::vector<std::uint64_t> sorted_ids(
    const std::vector<Record>& records,
    IdSelector selector) {
    std::vector<std::uint64_t> ids;
    ids.reserve(records.size());
    for (const auto& record : records) ids.push_back(selector(record));
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

core::Result<attributes::StableTopologySnapshot> stable_topology_snapshot(
    const geometry::EditableMesh& mesh) {
    if (auto result = mesh.validate(); !result) {
        return core::Result<attributes::StableTopologySnapshot>::failure(
            result.error().with_context("mesh topology snapshot"));
    }
    const auto topology = mesh.topology();
    if (!topology) {
        return core::Result<attributes::StableTopologySnapshot>::failure(
            topology.error().with_context("mesh topology snapshot"));
    }

    attributes::StableTopologySnapshot snapshot;
    const auto vertices = mesh.vertices_sorted();
    std::vector<std::uint64_t> vertex_ids;
    vertex_ids.reserve(vertices.size());
    for (const auto& vertex : vertices) vertex_ids.push_back(vertex.id.value);
    snapshot.element_ids.emplace(attributes::AttributeDomain::vertex, std::move(vertex_ids));

    const auto faces = mesh.faces_sorted();
    std::vector<std::uint64_t> face_ids;
    face_ids.reserve(faces.size());
    for (const auto& face : faces) face_ids.push_back(face.id.value);
    snapshot.element_ids.emplace(attributes::AttributeDomain::face, std::move(face_ids));

    snapshot.element_ids.emplace(
        attributes::AttributeDomain::edge,
        sorted_ids(topology.value().edges, [](const geometry::EdgeRecord& edge) {
            return edge.id.value;
        }));
    snapshot.element_ids.emplace(
        attributes::AttributeDomain::corner,
        sorted_ids(topology.value().corners, [](const geometry::CornerRecord& corner) {
            return corner.id.value;
        }));

    if (auto result = snapshot.validate(); !result) {
        return core::Result<attributes::StableTopologySnapshot>::failure(
            invalid("mesh topology snapshot contains invalid stable identities"));
    }
    return core::Result<attributes::StableTopologySnapshot>::success(std::move(snapshot));
}

core::Result<attributes::TopologyProvenance> derive_provenance(
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& destination) {
    const auto source_snapshot = stable_topology_snapshot(source);
    if (!source_snapshot) {
        return core::Result<attributes::TopologyProvenance>::failure(source_snapshot.error());
    }
    const auto destination_snapshot = stable_topology_snapshot(destination);
    if (!destination_snapshot) {
        return core::Result<attributes::TopologyProvenance>::failure(
            destination_snapshot.error());
    }
    return attributes::derive_provenance(source_snapshot.value(), destination_snapshot.value());
}

} // namespace carto::mesh_attributes
