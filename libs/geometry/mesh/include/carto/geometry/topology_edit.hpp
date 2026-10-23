#pragma once

#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/ids.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::geometry {

enum class OriginKind {
    preserved,
    duplicated_from,
    interpolated_from,
    split_from,
    generated_from_vertex,
    generated_from_edge,
    generated_from_face,
    boolean_intersection,
    subdivision_child,
};

struct ElementOrigin {
    OriginKind kind = OriginKind::preserved;
    std::vector<std::uint64_t> source_ids;
};

// The source IDs in an origin map use the domain of that map. For example,
// a generated vertex may carry source vertex IDs even when its kind is
// generated_from_edge or generated_from_face.
// Structured identity changes for a topology edit. Compiled indices are not
// included: authoring IDs remain the source of truth for selection, history,
// diagnostics, and future viewport picking.
struct TopologyEditReceipt {
    core::Revision revision_before;
    core::Revision revision_after;
    std::vector<VertexId> created_vertices;
    std::vector<EdgeId> created_edges;
    std::vector<FaceId> created_faces;
    std::vector<VertexId> removed_vertices;
    std::vector<EdgeId> removed_edges;
    std::vector<FaceId> removed_faces;
    std::vector<CornerId> created_corners;
    std::vector<CornerId> removed_corners;
    // A merge maps each removed source vertex to the surviving target vertex.
    // This is deliberately separate from origin maps: the target is a
    // preserved identity, not a newly-created element.
    std::map<VertexId, VertexId> merged_vertices;
    std::map<VertexId, ElementOrigin> vertex_origins;
    std::map<EdgeId, ElementOrigin> edge_origins;
    std::map<FaceId, ElementOrigin> face_origins;
    std::map<CornerId, ElementOrigin> corner_origins;

    [[nodiscard]] core::Result<void> validate() const;
    // Deterministic bounded text form for durable project lineage. The
    // receipt never contains geometry, compiled indices, or runtime handles.
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<TopologyEditReceipt> deserialize(
        std::string_view text);
};

} // namespace carto::geometry
