#include <carto/mesh_attributes/provenance.hpp>
#include <carto/attributes/transfer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
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
    const auto source_topology = source.topology();
    if (!source_topology) {
        return core::Result<attributes::TopologyProvenance>::failure(
            source_topology.error());
    }
    const auto destination_topology = destination.topology();
    if (!destination_topology) {
        return core::Result<attributes::TopologyProvenance>::failure(
            destination_topology.error());
    }

    const auto two_source_weights = [](core::Vec3d first, core::Vec3d second,
                                       core::Vec3d destination_position)
        -> std::optional<std::vector<double>> {
        const core::Vec3d direction = second - first;
        const double length_squared = core::dot(direction, direction);
        if (!std::isfinite(length_squared) || length_squared <= 0.0) return std::nullopt;
        double factor = core::dot(destination_position - first, direction) / length_squared;
        if (!std::isfinite(factor)) return std::nullopt;
        const double length = std::sqrt(length_squared);
        const double coordinate_scale = std::max({
            1.0, length, first.length(), second.length(), destination_position.length()});
        const double coordinate_tolerance = 1e-9 * coordinate_scale;
        const double factor_tolerance = coordinate_tolerance / length;
        if (factor < -factor_tolerance || factor > 1.0 + factor_tolerance) {
            return std::nullopt;
        }
        factor = std::clamp(factor, 0.0, 1.0);
        const core::Vec3d projected = first + direction * factor;
        if ((projected - destination_position).length() > coordinate_tolerance) {
            return std::nullopt;
        }
        return std::vector<double>{1.0 - factor, factor};
    };

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

            if ((domain == attributes::AttributeDomain::vertex ||
                 domain == attributes::AttributeDomain::edge ||
                 domain == attributes::AttributeDomain::corner) &&
                origin.kind == geometry::OriginKind::generated_from_face) {
                provenance.value().invalidated_destinations[domain].insert(
                    destination_index->second);
            }

            if (origin.kind != geometry::OriginKind::interpolated_from ||
                origin.source_ids.size() != 2U ||
                (domain != attributes::AttributeDomain::vertex &&
                 domain != attributes::AttributeDomain::corner)) {
                continue;
            }

            std::array<core::Vec3d, 2U> source_positions{};
            core::Vec3d destination_position{};
            if (domain == attributes::AttributeDomain::vertex) {
                for (std::size_t index = 0U; index < source_positions.size(); ++index) {
                    const auto* vertex = source.find_vertex(
                        geometry::VertexId{origin.source_ids[index]});
                    if (!vertex) {
                        return core::Result<void>::failure(core::Diagnostic(
                            core::ErrorCode::stale_data,
                            "interpolated vertex receipt references a missing source position"));
                    }
                    source_positions[index] = vertex->position;
                }
                const auto* vertex = destination.find_vertex(
                    geometry::VertexId{destination_id.value});
                if (!vertex) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "interpolated vertex receipt references a missing destination position"));
                }
                destination_position = vertex->position;
            } else {
                const auto find_corner = [](const auto& corners, std::uint64_t id)
                    -> const geometry::CornerRecord* {
                    const auto found = std::find_if(corners.begin(), corners.end(),
                        [id](const geometry::CornerRecord& corner) {
                            return corner.id.value == id;
                        });
                    return found == corners.end() ? nullptr : &*found;
                };
                for (std::size_t index = 0U; index < source_positions.size(); ++index) {
                    const auto* corner = find_corner(
                        source_topology.value().corners, origin.source_ids[index]);
                    const auto* vertex = corner ? source.find_vertex(corner->vertex) : nullptr;
                    if (!vertex) {
                        return core::Result<void>::failure(core::Diagnostic(
                            core::ErrorCode::stale_data,
                            "interpolated corner receipt references a missing source position"));
                    }
                    source_positions[index] = vertex->position;
                }
                const auto* corner = find_corner(
                    destination_topology.value().corners, destination_id.value);
                const auto* vertex = corner ? destination.find_vertex(corner->vertex) : nullptr;
                if (!vertex) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "interpolated corner receipt references a missing destination position"));
                }
                destination_position = vertex->position;
            }
            auto weights = two_source_weights(
                source_positions[0U], source_positions[1U], destination_position);
            if (!weights.has_value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "two-source interpolation receipt does not match its geometric position"));
            }
            auto& domain_weights = provenance.value().source_weights[domain];
            if (domain_weights.empty()) {
                domain_weights.resize(destination_ids->second.size());
            }
            domain_weights[destination_index->second] = std::move(*weights);
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

    const auto source_vertex_ids = source_snapshot.value().element_ids.find(
        attributes::AttributeDomain::vertex);
    const auto destination_vertex_ids = destination_snapshot.value().element_ids.find(
        attributes::AttributeDomain::vertex);
    const auto vertex_mapping = provenance.value().source_indices.find(
        attributes::AttributeDomain::vertex);
    if (source_vertex_ids == source_snapshot.value().element_ids.end() ||
        destination_vertex_ids == destination_snapshot.value().element_ids.end() ||
        vertex_mapping == provenance.value().source_indices.end() ||
        vertex_mapping->second.size() != destination_vertex_ids->second.size()) {
        return core::Result<attributes::TopologyProvenance>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "mesh topology receipt has incomplete vertex merge provenance"));
    }
    for (const auto& [source_vertex, target_vertex] : receipt.merged_vertices) {
        const auto source_id = std::lower_bound(
            source_vertex_ids->second.begin(), source_vertex_ids->second.end(),
            source_vertex.value);
        const auto target_id = std::lower_bound(
            destination_vertex_ids->second.begin(), destination_vertex_ids->second.end(),
            target_vertex.value);
        if (source_id == source_vertex_ids->second.end() ||
            *source_id != source_vertex.value ||
            target_id == destination_vertex_ids->second.end() ||
            *target_id != target_vertex.value) {
            return core::Result<attributes::TopologyProvenance>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "mesh topology receipt merge mapping references a missing vertex"));
        }
        const std::size_t source_index = static_cast<std::size_t>(
            std::distance(source_vertex_ids->second.begin(), source_id));
        const std::size_t target_index = static_cast<std::size_t>(
            std::distance(destination_vertex_ids->second.begin(), target_id));
        auto& target_sources = vertex_mapping->second[target_index];
        target_sources.push_back(source_index);
        std::sort(target_sources.begin(), target_sources.end());
        if (std::adjacent_find(target_sources.begin(), target_sources.end()) !=
            target_sources.end()) {
            return core::Result<attributes::TopologyProvenance>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "mesh topology receipt duplicates a merged vertex source"));
        }
    }
    return provenance;
}

core::Result<geometry::TopologyEditReceipt> merge_vertices_with_attribute_transfer(
    geometry::EditableMesh& mesh,
    geometry::VertexId target,
    geometry::VertexId source,
    geometry::VertexMergeMode mode) {
    const auto supports_interpolation = [](attributes::AttributeType type) noexcept {
        switch (type) {
        case attributes::AttributeType::float32:
        case attributes::AttributeType::float64:
        case attributes::AttributeType::vec2:
        case attributes::AttributeType::vec3:
        case attributes::AttributeType::vec4:
        case attributes::AttributeType::color4:
            return true;
        default:
            return false;
        }
    };

    geometry::EditableMesh after = mesh;
    if (!mesh.has_attribute_payload()) {
        auto merged = after.merge_vertices(target, source, mode);
        if (!merged) {
            return core::Result<geometry::TopologyEditReceipt>::failure(merged.error());
        }
        mesh = std::move(after);
        return core::Result<geometry::TopologyEditReceipt>::success(
            std::move(merged.value()));
    }

    const attributes::AttributeSet source_attributes = mesh.attributes();
    const attributes::UvSetTable source_uv_sets = mesh.uv_sets();
    if (auto result = after.clear_attribute_payload(); !result) {
        return core::Result<geometry::TopologyEditReceipt>::failure(result.error());
    }
    const geometry::EditableMesh geometry_source = after;
    auto merged = after.merge_vertices(target, source, mode);
    if (!merged) {
        return core::Result<geometry::TopologyEditReceipt>::failure(merged.error());
    }
    geometry::TopologyEditReceipt receipt = std::move(merged.value());
    auto provenance = provenance_from_receipt(geometry_source, after, receipt);
    if (!provenance) {
        return core::Result<geometry::TopologyEditReceipt>::failure(provenance.error());
    }

    attributes::AttributeTransferPolicy policy;
    policy.default_mode = attributes::TransferMode::preserve;
    for (const auto& [id, layer] : source_attributes.layers()) {
        if (layer.descriptor.domain != attributes::AttributeDomain::vertex) continue;
        if (!supports_interpolation(layer.descriptor.type)) {
            return core::Result<geometry::TopologyEditReceipt>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "vertex merge cannot transfer a discrete vertex attribute layer"));
        }
        policy.per_layer.emplace(id, attributes::TransferMode::interpolate);
    }
    auto transferred = attributes::transfer(
        source_attributes, std::move(provenance.value()), policy);
    if (!transferred) {
        return core::Result<geometry::TopologyEditReceipt>::failure(transferred.error());
    }
    if (auto result = after.set_attribute_payload(
            std::move(transferred.value()), source_uv_sets); !result) {
        return core::Result<geometry::TopologyEditReceipt>::failure(result.error());
    }
    receipt.revision_before = mesh.revision();
    receipt.revision_after = after.revision();
    if (auto result = receipt.validate(); !result) {
        return core::Result<geometry::TopologyEditReceipt>::failure(result.error());
    }
    mesh = std::move(after);
    return core::Result<geometry::TopologyEditReceipt>::success(std::move(receipt));
}

core::Result<geometry::TopologyEditReceipt> knife_cut_with_attribute_transfer(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    const std::vector<core::Vec3d>& path) {
    geometry::EditableMesh after = mesh;
    if (!mesh.has_attribute_payload()) {
        auto cut = after.knife_cut(face, path);
        if (!cut) {
            return core::Result<geometry::TopologyEditReceipt>::failure(cut.error());
        }
        mesh = std::move(after);
        return core::Result<geometry::TopologyEditReceipt>::success(std::move(cut.value()));
    }

    const attributes::AttributeSet source_attributes = mesh.attributes();
    const attributes::UvSetTable source_uv_sets = mesh.uv_sets();
    if (auto result = after.clear_attribute_payload(); !result) {
        return core::Result<geometry::TopologyEditReceipt>::failure(result.error());
    }
    const geometry::EditableMesh geometry_source = after;
    auto cut = after.knife_cut(face, path);
    if (!cut) {
        return core::Result<geometry::TopologyEditReceipt>::failure(cut.error());
    }
    geometry::TopologyEditReceipt receipt = std::move(cut.value());
    auto provenance = provenance_from_receipt(geometry_source, after, receipt);
    if (!provenance) {
        return core::Result<geometry::TopologyEditReceipt>::failure(provenance.error());
    }

    const auto supports_interpolation = [](attributes::AttributeType type) noexcept {
        switch (type) {
        case attributes::AttributeType::float32:
        case attributes::AttributeType::float64:
        case attributes::AttributeType::vec2:
        case attributes::AttributeType::vec3:
        case attributes::AttributeType::vec4:
        case attributes::AttributeType::color4:
            return true;
        default:
            return false;
        }
    };
    attributes::AttributeTransferPolicy policy;
    policy.default_mode = attributes::TransferMode::invalidate;
    for (const auto& [id, layer] : source_attributes.layers()) {
        switch (layer.descriptor.domain) {
        case attributes::AttributeDomain::vertex:
        case attributes::AttributeDomain::corner:
            if (!supports_interpolation(layer.descriptor.type)) {
                return core::Result<geometry::TopologyEditReceipt>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "knife cut cannot interpolate a discrete vertex or corner attribute"));
            }
            policy.per_layer.emplace(id, attributes::TransferMode::interpolate);
            break;
        case attributes::AttributeDomain::edge:
        case attributes::AttributeDomain::face:
        case attributes::AttributeDomain::object:
            policy.per_layer.emplace(id, attributes::TransferMode::duplicate);
            break;
        default:
            return core::Result<geometry::TopologyEditReceipt>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "knife cut cannot transfer this non-mesh attribute domain"));
        }
    }

    const auto object_count = source_attributes.domain_count(attributes::AttributeDomain::object);
    if (object_count.has_value()) {
        if (*object_count > 1U) {
            return core::Result<geometry::TopologyEditReceipt>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "knife cut requires a single-element object attribute domain"));
        }
        provenance.value().source_indices.insert_or_assign(
            attributes::AttributeDomain::object,
            *object_count == 1U
                ? std::vector<std::vector<std::size_t>>{{0U}}
                : std::vector<std::vector<std::size_t>>{});
    }
    for (const auto& [domain, count] : source_attributes.domain_counts()) {
        if (domain == attributes::AttributeDomain::object ||
            domain == attributes::AttributeDomain::vertex ||
            domain == attributes::AttributeDomain::edge ||
            domain == attributes::AttributeDomain::face ||
            domain == attributes::AttributeDomain::corner) {
            continue;
        }
        if (count != 0U) {
            return core::Result<geometry::TopologyEditReceipt>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "knife cut cannot transfer a non-empty non-mesh attribute domain"));
        }
        provenance.value().source_indices.emplace(domain,
            std::vector<std::vector<std::size_t>>{});
    }

    // The geometry receipt naturally describes all topology domains, while a
    // sparse AttributeSet may only carry vertex/corner payloads. Keep the
    // transfer contract limited to domains the source payload actually owns.
    for (auto iterator = provenance.value().source_indices.begin();
         iterator != provenance.value().source_indices.end();) {
        if (source_attributes.domain_count(iterator->first).has_value()) {
            ++iterator;
        } else {
            iterator = provenance.value().source_indices.erase(iterator);
        }
    }
    for (auto iterator = provenance.value().source_weights.begin();
         iterator != provenance.value().source_weights.end();) {
        if (source_attributes.domain_count(iterator->first).has_value()) {
            ++iterator;
        } else {
            iterator = provenance.value().source_weights.erase(iterator);
        }
    }
    for (auto iterator = provenance.value().invalidated_destinations.begin();
         iterator != provenance.value().invalidated_destinations.end();) {
        if (source_attributes.domain_count(iterator->first).has_value()) {
            ++iterator;
        } else {
            iterator = provenance.value().invalidated_destinations.erase(iterator);
        }
    }

    auto transferred = attributes::transfer(
        source_attributes, std::move(provenance.value()), policy);
    if (!transferred) {
        return core::Result<geometry::TopologyEditReceipt>::failure(transferred.error());
    }
    if (auto result = after.set_attribute_payload(
            std::move(transferred.value()), source_uv_sets); !result) {
        return core::Result<geometry::TopologyEditReceipt>::failure(result.error());
    }
    receipt.revision_before = mesh.revision();
    receipt.revision_after = after.revision();
    if (auto result = receipt.validate(); !result) {
        return core::Result<geometry::TopologyEditReceipt>::failure(result.error());
    }
    mesh = std::move(after);
    return core::Result<geometry::TopologyEditReceipt>::success(std::move(receipt));
}

} // namespace carto::mesh_attributes
