#pragma once

#include <carto/geometry/compiled_mesh.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/geometry/topology_edit.hpp>

#include <carto/core/result.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace carto::geometry {

struct Vertex {
    VertexId id;
    core::Vec3d position;
};

struct Face {
    FaceId id;
    std::vector<VertexId> vertices;
};

struct EdgeRecord {
    EdgeId id;
    VertexId first;
    VertexId second;
    HalfEdgeId first_half_edge;
    std::optional<HalfEdgeId> second_half_edge;
};

struct HalfEdgeRecord {
    HalfEdgeId id;
    VertexId origin;
    VertexId destination;
    FaceId face;
    HalfEdgeId next;
    HalfEdgeId previous;
    std::optional<HalfEdgeId> twin;
    EdgeId edge;
    CornerId corner;
};

struct CornerRecord {
    CornerId id;
    FaceId face;
    VertexId vertex;
    HalfEdgeId half_edge;
};

struct VertexChange {
    VertexId id;
    core::Vec3d before;
    core::Vec3d after;
};

struct MeshPatch {
    core::Revision expected_revision;
    std::vector<VertexChange> vertex_changes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] MeshPatch inverse(core::Revision expected_revision) const;
};

struct TopologySnapshot {
    std::vector<EdgeRecord> edges;
    std::vector<HalfEdgeRecord> half_edges;
    std::vector<CornerRecord> corners;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<std::vector<HalfEdgeId>> face_boundary(FaceId face) const;
    [[nodiscard]] std::vector<HalfEdgeId> boundary_half_edges() const;
    [[nodiscard]] core::Result<std::vector<EdgeId>> edge_loop(EdgeId edge) const;
    [[nodiscard]] core::Result<std::vector<EdgeId>> edge_ring(EdgeId edge) const;
    [[nodiscard]] core::Result<std::vector<EdgeId>> boundary_loop(EdgeId edge) const;
    [[nodiscard]] core::Result<std::vector<FaceId>> vertex_fan(VertexId vertex) const;
    [[nodiscard]] core::Result<std::vector<FaceId>> face_region(FaceId face) const;
    [[nodiscard]] core::Result<std::vector<VertexId>> linked_component(VertexId vertex) const;
    [[nodiscard]] core::Result<std::vector<VertexId>> shortest_path(
        VertexId start, VertexId goal) const;
};

class EditableMesh {
public:
    EditableMesh() = default;

    [[nodiscard]] core::Result<VertexId> add_vertex(core::Vec3d position);
    [[nodiscard]] core::Result<FaceId> add_face(std::vector<VertexId> vertices);
    [[nodiscard]] core::Result<void> insert_vertex(Vertex vertex);
    [[nodiscard]] core::Result<void> insert_face(Face face);
    [[nodiscard]] core::Result<void> set_vertex_position(VertexId id, core::Vec3d position);
    [[nodiscard]] core::Result<void> apply_patch(const MeshPatch& patch);
    // Inserts a complete interchange topology and rebuilds adjacency once.
    // Bounded importers use this instead of repeatedly rebuilding after every
    // face admission.
    [[nodiscard]] core::Result<void> insert_bulk(
        std::vector<Vertex> vertices,
        std::vector<Face> faces);
    [[nodiscard]] core::Result<void> extrude_face(FaceId id, double distance);
    [[nodiscard]] core::Result<TopologyEditReceipt> extrude_face_with_receipt(
        FaceId id, double distance);
    [[nodiscard]] core::Result<TopologyEditReceipt> delete_face(
        FaceId id,
        bool remove_orphaned_vertices = true);
    [[nodiscard]] core::Result<TopologyEditReceipt> split_edge(
        EdgeId id,
        double factor = 0.5);
    [[nodiscard]] core::Result<TopologyEditReceipt> inset_face(
        FaceId id,
        double distance);
    [[nodiscard]] core::Result<void> restore_from(const EditableMesh& source);
    [[nodiscard]] core::Result<void> restore_revision(core::Revision revision);

    [[nodiscard]] const Vertex* find_vertex(VertexId id) const noexcept;
    [[nodiscard]] const Face* find_face(FaceId id) const noexcept;
    [[nodiscard]] const EdgeRecord* find_edge(EdgeId id) const noexcept;
    [[nodiscard]] std::vector<Vertex> vertices_sorted() const;
    [[nodiscard]] std::vector<Face> faces_sorted() const;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<TopologySnapshot> topology() const;
    [[nodiscard]] core::Result<CompiledMesh> compile() const;

    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t vertex_count() const noexcept { return vertices_.size(); }
    [[nodiscard]] std::size_t face_count() const noexcept { return faces_.size(); }

private:
    [[nodiscard]] core::Result<void> rebuild_topology();
    [[nodiscard]] core::Result<void> validate_patch_geometry(const MeshPatch& patch) const;
    [[nodiscard]] core::Result<void> validate_vertex_face_index() const;
    [[nodiscard]] core::Result<void> validate_topology_state() const;
    [[nodiscard]] TopologySnapshot topology_snapshot() const;
    void bump_revision() noexcept {
        compiled_cache_.reset();
        revision_ = revision_.next();
    }

    std::map<VertexId, Vertex> vertices_;
    std::map<FaceId, Face> faces_;
    std::map<EdgeId, EdgeRecord> edges_;
    std::map<HalfEdgeId, HalfEdgeRecord> half_edges_;
    std::map<CornerId, CornerRecord> corners_;
    std::map<FaceId, HalfEdgeId> face_boundaries_;
    std::map<VertexId, std::vector<FaceId>> vertex_faces_;
    std::uint64_t next_vertex_id_ = 1;
    std::uint64_t next_face_id_ = 1;
    std::uint64_t next_edge_id_ = 1;
    std::uint64_t next_half_edge_id_ = 1;
    std::uint64_t next_corner_id_ = 1;
    core::Revision revision_{};
    mutable std::optional<CompiledMesh> compiled_cache_;
};

} // namespace carto::geometry
