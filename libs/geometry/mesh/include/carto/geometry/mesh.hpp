#pragma once

#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/geometry/topology_edit.hpp>

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::geometry {

// A two-vertex weld policy. `last_selected` is a selection-bound request that
// editor commands resolve to explicit target/source identities before calling
// the mesh kernel; the kernel itself accepts only target or center.
enum class VertexMergeMode : std::uint8_t {
    target,
    center,
    last_selected,
};

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

// Replayable request to delete one face from an exact source revision.
struct FaceDeletionPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    FaceId face;
    bool remove_orphaned_vertices = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FaceDeletionPatch> deserialize(std::string_view bytes);
};

// Replayable request to split one edge from an exact source revision.
struct EdgeSplitPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    EdgeId edge;
    double factor = 0.5;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<EdgeSplitPatch> deserialize(std::string_view bytes);
};

// Replayable bounded edge-dissolve request against an exact source revision.
// Selected edge IDs are canonicalized by serialize().
struct EdgeDissolvePatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxEdges = 1'000'000U;
    static constexpr std::size_t kMaxSerializedBytes = 24U * 1024U * 1024U;

    core::Revision expected_revision;
    std::vector<EdgeId> edges;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<EdgeDissolvePatch> deserialize(
        std::string_view bytes);
};

// Replayable deterministic merge-by-distance request against an exact source
// revision. Candidate IDs are canonicalized by serialize().
struct MergeByDistancePatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxCandidates = 1'000'000U;
    static constexpr std::size_t kMaxSerializedBytes = 24U * 1024U * 1024U;

    core::Revision expected_revision;
    std::vector<VertexId> candidates;
    double distance = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MergeByDistancePatch> deserialize(
        std::string_view bytes);
};

// Replayable two-vertex weld request against an exact source revision.
// The target/source roles are intentional and are not reordered.
struct VertexMergePatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    VertexId target;
    VertexId source;
    VertexMergeMode mode = VertexMergeMode::target;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<VertexMergePatch> deserialize(
        std::string_view bytes);
};

// Replayable request to cut a validated quad edge ring from an exact source
// revision. The seed edge deterministically identifies the ring and midpoint.
struct LoopCutPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    EdgeId edge;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<LoopCutPatch> deserialize(std::string_view bytes);
};

// Replayable bounded cut path against one exact source face and revision.
struct KnifeCutPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxPathPoints = 4096U;
    static constexpr std::size_t kMaxSerializedBytes = 512U * 1024U;

    core::Revision expected_revision;
    FaceId face;
    std::vector<core::Vec3d> path;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<KnifeCutPatch> deserialize(
        std::string_view bytes);
};

// Replayable bounded edge bevel request against an exact source revision.
struct BevelEdgePatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    EdgeId edge;
    double width = 0.0;
    std::uint32_t segments = 1U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<BevelEdgePatch> deserialize(std::string_view bytes);
};

// Replayable bounded vertex bevel request against an exact source revision.
struct BevelVertexPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    VertexId vertex;
    double width = 0.0;
    std::uint32_t segments = 1U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<BevelVertexPatch> deserialize(std::string_view bytes);
};

// Replayable bounded face extrusion request against an exact source revision.
struct FaceExtrusionPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    FaceId face;
    double distance = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FaceExtrusionPatch> deserialize(
        std::string_view bytes);
};

// Replayable bounded face inset request against an exact source revision.
struct FaceInsetPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    FaceId face;
    double distance = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FaceInsetPatch> deserialize(std::string_view bytes);
};

// Replayable face poke request against an exact source revision.
struct FacePokePatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    FaceId face;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<FacePokePatch> deserialize(std::string_view bytes);
};

// Replayable triangle-pair conversion against an exact source revision.
// Face identifiers are canonicalized by serialize() so the symmetric
// operation has one durable representation.
struct TriToQuadPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 256U;

    core::Revision expected_revision;
    FaceId first_face;
    FaceId second_face;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<TriToQuadPatch> deserialize(
        std::string_view bytes);
};

struct MeshPatch {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxVertexChanges = 1'000'000U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    core::Revision expected_revision;
    std::vector<VertexChange> vertex_changes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] MeshPatch inverse(core::Revision expected_revision) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<MeshPatch> deserialize(std::string_view bytes);
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
    // Moves one vertex along an incident support edge without changing
    // topology. The factor is strictly inside (0, 1); invalid or
    // geometry-collapsing slides are rejected atomically.
    [[nodiscard]] core::Result<void> slide_vertex(
        VertexId vertex,
        EdgeId support_edge,
        double factor);
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
    // Splits a validated quad edge loop at its midpoint and connects the new
    // vertices across each traversed quad. Non-quad or malformed loops refuse
    // before publishing any topology change.
    [[nodiscard]] core::Result<TopologyEditReceipt> loop_cut(EdgeId edge);
    // Splits one planar face along a bounded polyline. The path endpoints must
    // be existing face vertices; intermediate points must be strictly inside
    // the face. Self-intersecting and boundary-crossing paths refuse atomically.
    [[nodiscard]] core::Result<TopologyEditReceipt> knife_cut(
        FaceId face, const std::vector<core::Vec3d>& path);
    // Replaces one internal manifold edge with a bounded chamfer profile.
    // The incident faces must be strictly convex and planar. Segment counts
    // are limited to 1..64; multi-segment profiles use circular interpolation
    // and create one quad per profile segment. Boundary edges, non-manifold
    // topology, ambiguous opposing offsets, and collapsing widths are refused.
    [[nodiscard]] core::Result<TopologyEditReceipt> bevel_edge(
        EdgeId id,
        double width,
        std::uint32_t segments = 1U);
    // Replaces one closed triangular vertex fan (valence 3..64) with chamfered
    // incident faces and a bounded cap. Segment counts are limited to 1..64;
    // multi-segment caps subdivide their boundary and triangulate the incident
    // faces with source-face lineage. The cap must remain simple, planar, and
    // convex. Boundary vertices, non-triangular fans, non-manifold edges, and
    // collapsing widths are refused.
    [[nodiscard]] core::Result<TopologyEditReceipt> bevel_vertex(
        VertexId id,
        double width,
        std::uint32_t segments = 1U);
    // Dissolves one internal manifold edge by joining its coplanar incident
    // faces into a simple polygon. Concave results are ear-clipped for
    // compilation; non-planar, self-intersecting, and degenerate joins refuse.
    [[nodiscard]] core::Result<TopologyEditReceipt> dissolve_edge(EdgeId id);
    static constexpr std::size_t kMaxDissolveEdges = 1'000'000U;
    // Dissolves a deterministic set of internal edges as one atomic edit and
    // one revision. Every intermediate and final face must remain a simple,
    // coplanar polygon.
    [[nodiscard]] core::Result<TopologyEditReceipt> dissolve_edges(
        const std::vector<EdgeId>& ids);
    // Converts exactly two compatible triangles sharing one internal edge
    // into one validated simple planar quad.
    [[nodiscard]] core::Result<TopologyEditReceipt> tri_to_quad(
        FaceId first,
        FaceId second);
    // Welds source onto target. The target identity is preserved; with
    // VertexMergeMode::target its position is kept, and with
    // VertexMergeMode::center it moves to the midpoint of both inputs.
    // Degenerate faces created by the collapse are removed and recorded in
    // the topology receipt.
    [[nodiscard]] core::Result<TopologyEditReceipt> merge_vertices(
        VertexId target,
        VertexId source,
        VertexMergeMode mode = VertexMergeMode::target);
    // Welds every candidate onto the lowest-ID unmerged candidate whose
    // authored position lies within distance, visiting candidates in
    // ascending stable-ID order. Clustering is not transitive and survivors
    // keep their positions. All merges publish atomically as one revision and
    // one receipt; no-op requests, non-simple collapses, and candidate sets
    // above kMaxMergeByDistanceCandidates are rejected without mutation.
    static constexpr std::size_t kMaxMergeByDistanceCandidates = 1'000'000U;
    [[nodiscard]] core::Result<TopologyEditReceipt> merge_by_distance(
        const std::vector<VertexId>& candidates,
        double distance);
    [[nodiscard]] core::Result<TopologyEditReceipt> inset_face(
        FaceId id,
        double distance);
    // Replaces a strictly convex planar face with a fan of triangles around
    // a new centroid vertex. Unsupported geometry is rejected atomically.
    [[nodiscard]] core::Result<TopologyEditReceipt> poke_face(FaceId id);
    [[nodiscard]] core::Result<void> restore_from(const EditableMesh& source);
    [[nodiscard]] core::Result<void> restore_revision(core::Revision revision);

    // UVs and other attributes are authored against the current topology. A
    // payload can be attached only after its domain cardinalities and UV-set
    // bindings validate against this mesh. Topology edits are then rejected
    // until an explicit transfer result is attached.
    [[nodiscard]] core::Result<void> set_attribute_payload(
        attributes::AttributeSet attributes,
        attributes::UvSetTable uv_sets);
    [[nodiscard]] core::Result<void> clear_attribute_payload();
    [[nodiscard]] const attributes::AttributeSet& attributes() const noexcept {
        return attributes_;
    }
    [[nodiscard]] const attributes::UvSetTable& uv_sets() const noexcept {
        return uv_sets_;
    }
    [[nodiscard]] bool has_attribute_payload() const noexcept {
        return !attributes_.domain_counts().empty() || attributes_.layer_count() != 0U ||
            !uv_sets_.descriptors().empty();
    }

    [[nodiscard]] const Vertex* find_vertex(VertexId id) const noexcept;
    [[nodiscard]] const Face* find_face(FaceId id) const noexcept;
    [[nodiscard]] const EdgeRecord* find_edge(EdgeId id) const noexcept;
    [[nodiscard]] std::vector<Vertex> vertices_sorted() const;
    [[nodiscard]] std::vector<Face> faces_sorted() const;
    [[nodiscard]] std::vector<Face> faces_sorted(std::size_t limit) const;
    [[nodiscard]] std::vector<EdgeRecord> edges_sorted(std::size_t limit) const;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] core::Result<TopologySnapshot> topology() const;
    [[nodiscard]] core::Result<CompiledMesh> compile() const;

    [[nodiscard]] core::Revision revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t vertex_count() const noexcept { return vertices_.size(); }
    [[nodiscard]] std::size_t face_count() const noexcept { return faces_.size(); }
    [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }

private:
    [[nodiscard]] core::Result<void> rebuild_topology();
    [[nodiscard]] core::Result<TopologyEditReceipt> apply_vertex_merges(
        const std::map<VertexId, VertexId>& replacements,
        const std::map<VertexId, core::Vec3d>& moved_targets);
    [[nodiscard]] core::Result<void> reject_topology_edit_with_attributes() const;
    [[nodiscard]] core::Result<void> validate_attribute_payload() const;
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
    attributes::AttributeSet attributes_;
    attributes::UvSetTable uv_sets_;
    core::Revision revision_{};
    mutable std::optional<CompiledMesh> compiled_cache_;
};

} // namespace carto::geometry
