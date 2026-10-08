#include <carto/mesh_attributes/provenance.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

core::Result<attributes::TopologyProvenance> provenance_from_receipt(
    const geometry::EditableMesh& source,
    const geometry::EditableMesh& destination,
    const geometry::TopologyEditReceipt& receipt) {
    if (auto result = receipt.validate(); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(
            result.error().with_context("mesh topology receipt"));
    }
    if (source.revision() != receipt.revision_before ||
        destination.revision() != receipt.revision_after) {
        return core::Result<attributes::TopologyProvenance>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "mesh topology receipt revisions do not match source and destination"));
    }

    const auto source_snapshot = stable_topology_snapshot(source);
    if (!source_snapshot) {
        return core::Result<attributes::TopologyProvenance>::failure(source_snapshot.error());
    }
    const auto destination_snapshot = stable_topology_snapshot(destination);
    if (!destination_snapshot) {
        return core::Result<attributes::TopologyProvenance>::failure(
            destination_snapshot.error());
    }
    auto provenance = attributes::derive_provenance(
        source_snapshot.value(), destination_snapshot.value());
    if (!provenance) {
        return provenance;
    }

    const auto verify_delta = [&](attributes::AttributeDomain domain,
                                  const auto& created_ids,
                                  const auto& removed_ids) -> core::Result<void> {
        const auto source_ids = source_snapshot.value().element_ids.find(domain);
        const auto destination_ids = destination_snapshot.value().element_ids.find(domain);
        if (source_ids == source_snapshot.value().element_ids.end() ||
            destination_ids == destination_snapshot.value().element_ids.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "mesh topology receipt references an unavailable delta domain"));
        }
        const auto as_values = [](const auto& ids) {
            std::vector<std::uint64_t> values;
            values.reserve(ids.size());
            for (const auto id : ids) values.push_back(id.value);
            return values;
        };
        const auto expected_created = [&]() {
            std::vector<std::uint64_t> values;
            std::set_difference(
                destination_ids->second.begin(), destination_ids->second.end(),
                source_ids->second.begin(), source_ids->second.end(),
                std::back_inserter(values));
            return values;
        }();
        const auto expected_removed = [&]() {
            std::vector<std::uint64_t> values;
            std::set_difference(
                source_ids->second.begin(), source_ids->second.end(),
                destination_ids->second.begin(), destination_ids->second.end(),
                std::back_inserter(values));
            return values;
        }();
        if (as_values(created_ids) != expected_created ||
            as_values(removed_ids) != expected_removed) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "mesh topology receipt delta does not match source and destination identities"));
        }
        return core::Result<void>::success();
    };
    if (auto result = verify_delta(
            attributes::AttributeDomain::vertex,
            receipt.created_vertices, receipt.removed_vertices); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    if (auto result = verify_delta(
            attributes::AttributeDomain::edge,
            receipt.created_edges, receipt.removed_edges); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    if (auto result = verify_delta(
            attributes::AttributeDomain::face,
            receipt.created_faces, receipt.removed_faces); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    if (auto result = verify_delta(
            attributes::AttributeDomain::corner,
            receipt.created_corners, receipt.removed_corners); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }

    const auto apply_origins = [&](
        attributes::AttributeDomain domain,
        const auto& origins) -> core::Result<void> {
        if (origins.empty()) return core::Result<void>::success();
        const auto source_ids = source_snapshot.value().element_ids.find(domain);
        const auto destination_ids = destination_snapshot.value().element_ids.find(domain);
        if (source_ids == source_snapshot.value().element_ids.end() ||
            destination_ids == destination_snapshot.value().element_ids.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "mesh topology receipt references an unavailable attribute domain"));
        }
        const auto mapping = provenance.value().source_indices.find(domain);
        if (mapping == provenance.value().source_indices.end() ||
            mapping->second.size() != destination_ids->second.size()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "mesh topology receipt provenance has an inconsistent destination cardinality"));
        }

        std::map<std::uint64_t, std::size_t> source_indices;
        for (std::size_t index = 0U; index < source_ids->second.size(); ++index) {
            source_indices.emplace(source_ids->second[index], index);
        }
        std::map<std::uint64_t, std::size_t> destination_indices;
        for (std::size_t index = 0U; index < destination_ids->second.size(); ++index) {
            destination_indices.emplace(destination_ids->second[index], index);
        }
        for (const auto& [destination_id, origin] : origins) {
            const auto destination_index = destination_indices.find(destination_id.value);
            if (destination_index == destination_indices.end()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "mesh topology receipt references a missing destination identity"));
            }
            std::vector<std::size_t> source_mapping;
            source_mapping.reserve(origin.source_ids.size());
            for (const std::uint64_t source_id : origin.source_ids) {
                const auto source_index = source_indices.find(source_id);
                if (source_index == source_indices.end()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "mesh topology receipt references a missing source identity"));
                }
                source_mapping.push_back(source_index->second);
            }
            mapping->second[destination_index->second] = std::move(source_mapping);
        }
        return core::Result<void>::success();
    };

    if (auto result = apply_origins(
            attributes::AttributeDomain::vertex, receipt.vertex_origins); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    if (auto result = apply_origins(
            attributes::AttributeDomain::edge, receipt.edge_origins); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    if (auto result = apply_origins(
            attributes::AttributeDomain::face, receipt.face_origins); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    if (auto result = apply_origins(
            attributes::AttributeDomain::corner, receipt.corner_origins); !result) {
        return core::Result<attributes::TopologyProvenance>::failure(result.error());
    }
    return provenance;
}

} // namespace carto::mesh_attributes
