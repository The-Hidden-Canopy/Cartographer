#pragma once

#include <carto/core/revision.hpp>
#include <carto/geometry/ids.hpp>

#include <vector>

namespace carto::geometry {

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
};

} // namespace carto::geometry
