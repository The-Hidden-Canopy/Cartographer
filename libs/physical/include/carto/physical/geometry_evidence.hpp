#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/physical/types.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace carto::physical {

// Per-triangle thickness estimates measured from the mesh bounds center.
// Thickness is geometric evidence, not a manufactured fact: it tells the
// consumer how confidently the mesh supports thin-shell or wall constructions.
struct ThicknessEvidence {
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    std::uint32_t samples = 0U;
    bool ambiguous = true;

    [[nodiscard]] core::Result<void> validate() const;
};

// Neutral geometric evidence derived once from a compiled mesh. The record is
// digest-bound to the mesh identity and source revision it was derived from;
// consumers verify the digest before trusting the numbers. It contains no
// runtime handles and no provider state.
struct GeometryEvidence {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 1U * 1024U * 1024U;

    std::uint64_t mesh_identity = 0U;
    core::Revision source_revision;
    // Optional content digest of the exact compiled mesh bytes. Present when
    // the deriving stage already content-addresses its mesh products.
    std::optional<assets::Sha256Digest> mesh_content_digest;
    std::uint64_t triangle_count = 0U;
    double signed_volume = 0.0;
    double volume = 0.0;
    double surface_area = 0.0;
    bool winding_consistent = false;
    core::Vec3d volume_centroid{};
    // Inertia of the enclosed volume at unit density, about the volume
    // centroid. Mass stages scale this tensor; they never re-integrate mesh.
    detail::SymmetricTensor3 unit_density_inertia{};
    ThicknessEvidence thickness{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<GeometryEvidence> deserialize(
        std::string_view text);
};

// Derives evidence from a validated compiled mesh. Fails closed on open,
// inconsistently wound, or non-finite meshes: mass stages must never invent
// geometry facts. The mesh identity is the stable authoring identity chosen by
// the deriving stage; the evidence digest binds identity + revision + numbers.
[[nodiscard]] core::Result<GeometryEvidence> derive_geometry_evidence(
    std::uint64_t mesh_identity,
    const geometry::CompiledMesh& mesh);

namespace detail {
void encode_evidence(std::ostream& output, const GeometryEvidence& evidence);
core::Result<void> decode_evidence(std::istream& input, GeometryEvidence& evidence);
} // namespace detail

} // namespace carto::physical
