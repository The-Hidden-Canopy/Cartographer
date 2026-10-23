#include <carto/navgraph/access.hpp>

#include "detail.hpp"

namespace carto::navgraph {

core::Result<void> StructureAccessProduct::validate() const {
    if (!id || !source_graph || source_node == 0U ||
        !detail::valid_access_primitive(primitive) ||
        !detail::safe_text(semantic_type, kMaxSemanticBytes) ||
        !position.finite()) {
        return core::Result<void>::failure(detail::invalid(
            "structure access identity, source, primitive, type, or position is invalid"));
    }
    if (area.has_value() && !area.value()) {
        return core::Result<void>::failure(detail::validation(
            "structure access area reference is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<StructureAccessProduct>> compile_access_points(
    const world::StructureGraph& graph) {
    std::vector<StructureAccessProduct> access_points;
    access_points.reserve(graph.nodes.size());
    for (const auto& [identity, node] : graph.nodes) {
        const auto primitive = classify_access_primitive(node.semantic_type);
        if (!primitive.has_value()) {
            continue; // Passive structure node: no traversal interface.
        }
        StructureAccessProduct access;
        access.id = derive_access_id(identity);
        access.source_graph = graph.id;
        access.source_node = identity;
        access.primitive = primitive.value();
        access.semantic_type = node.semantic_type;
        access.area = node.area;
        access.position = {};
        if (auto result = access.validate(); !result) {
            return core::Result<std::vector<StructureAccessProduct>>::failure(result.error());
        }
        access_points.push_back(std::move(access));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<std::vector<StructureAccessProduct>>::failure(result.error());
    }
    return core::Result<std::vector<StructureAccessProduct>>::success(std::move(access_points));
}

} // namespace carto::navgraph
