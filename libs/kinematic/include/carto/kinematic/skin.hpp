#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace carto::kinematic {

class KinematicGraph;

// One weighted joint influence on a mesh vertex.
struct JointInfluence {
    JointId joint;
    double weight = 0.0;
};

// Provider-neutral skin binding: which mesh is deformed, by which joints, with
// which normalized weights, relative to which bind pose. The binding is
// content-addressed against both the mesh and the kinematic graph so a stale
// binding fails closed instead of silently deforming a changed source.
// Current vertex positions and posed matrices are runtime-owned and absent.
struct SkinBinding {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 32U * 1024U * 1024U;
    static constexpr std::size_t kMaxInfluencedVertices = 262'144U;
    static constexpr std::size_t kMaxInfluencesPerVertex = 8U;
    static constexpr std::uint32_t kMaxVertexIndex = 16'777'215U;
    // Influences are considered normalized when their sum is within this
    // tolerance of exactly one.
    static constexpr double kWeightSumTolerance = 1e-6;

    // Stable mesh asset identity (scene mesh-asset id namespace).
    std::uint64_t mesh_asset = 0U;
    // Digest of the mesh content this binding was authored against.
    assets::Sha256Digest mesh_digest;
    // Digest of the KinematicGraph this binding was authored against.
    assets::Sha256Digest graph_digest;
    // Mesh-to-model transform captured at bind time. Per-joint bind transforms
    // are the graph rest transforms: a single source of truth, not duplicated.
    core::Transform mesh_bind_transform = core::Transform::identity();
    // Vertices with at least one joint influence, keyed by mesh vertex index
    // and sorted by construction. Vertices with zero influences are rigid and
    // are simply absent. Per-vertex influence lists are sorted by joint id,
    // unique, and weight-normalized.
    std::map<std::uint32_t, std::vector<JointInfluence>> influences;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<SkinBinding> deserialize(
        std::string_view text);
};

} // namespace carto::kinematic
