#pragma once

#include <carto/geometry/compiled_mesh.hpp>

#include <carto/core/result.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace carto::geometry {

struct VertexId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const VertexId&) const noexcept = default;
};

struct FaceId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const FaceId&) const noexcept = default;
};

struct Vertex {
    VertexId id;
    core::Vec3d position;
};

struct Face {
    FaceId id;
    std::vector<VertexId> vertices;
};

struct HalfEdgeId {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }
    [[nodiscard]] constexpr auto operator<=>(const HalfEdgeId&) const noexcept = default;
};

struct HalfEdgeRecord {
    HalfEdgeId id;
    VertexId origin;
    VertexId destination;
    FaceId face;
    HalfEdgeId next;
    HalfEdgeId previous;
    std::optional<HalfEdgeId> twin;
};

struct TopologySnapshot {
    std::vector<HalfEdgeRecord> half_edges;

    [[nodiscard]] core::Result<void> validate() const;
};

class EditableMesh {
public:
    EditableMesh() = default;

    [[nodiscard]] core::Result<VertexId> add_vertex(core::Vec3d position);
    [[nodiscard]] core::Result<FaceId> add_face(std::vector<VertexId> vertices);
    [[nodiscard]] core::Result<void> insert_vertex(Vertex vertex);
    [[nodiscard]] core::Result<void> insert_face(Face face);
    [[nodiscard]] core::Result<void> set_vertex_position(VertexId id, core::Vec3d position);
    [[nodiscard]] core::Result<void> extrude_face(FaceId id, double distance);
    [[nodiscard]] core::Result<void> restore_from(const EditableMesh& source);
    void restore_revision(core::Revision revision) noexcept { revision_ = revision; }

    [[nodiscard]] const Vertex* find_vertex(VertexId id) const noexcept;
    [[nodiscard]] const Face* find_face(FaceId id) const noexcept;
    [[nodiscard]] std::vector<Vertex> vertices_sorted() const;
    [[nodiscard]] std::vector<Face> faces_sorted() const;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<TopologySnapshot> topology() const;
    [[nodiscard]] core::Result<CompiledMesh> compile() const;

    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t vertex_count() const noexcept { return vertices_.size(); }
    [[nodiscard]] std::size_t face_count() const noexcept { return faces_.size(); }

private:
    void bump_revision() noexcept { revision_ = revision_.next(); }

    std::map<VertexId, Vertex> vertices_;
    std::map<FaceId, Face> faces_;
    std::uint64_t next_vertex_id_ = 1;
    std::uint64_t next_face_id_ = 1;
    core::Revision revision_{};
};

} // namespace carto::geometry
