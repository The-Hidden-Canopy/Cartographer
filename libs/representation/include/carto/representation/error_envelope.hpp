#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/surface/core.hpp>
#include <carto/web_geometry/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace carto::representation {

// Neutral per-variant error envelope: conservative bounds on the four public
// error axes. It wraps the surface evidence envelope conceptually but is its
// own record so representation federation does not depend on surface policy.
struct RepresentationErrorEnvelope {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U;

    double max_position_error = 0.0;
    double max_silhouette_error = 0.0;
    double max_normal_error = 0.0;
    double max_radiance_error = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const RepresentationErrorEnvelope&) const noexcept =
        default;

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RepresentationErrorEnvelope> deserialize(
        std::string_view text);
};

// Lifts the surface evidence envelope onto the neutral four-axis record. The
// radiance bound keeps the worst-case surface estimate.
[[nodiscard]] RepresentationErrorEnvelope from_surface_envelope(
    const surface::SurfaceErrorEnvelope& envelope);
[[nodiscard]] surface::SurfaceErrorEnvelope to_surface_envelope(
    const RepresentationErrorEnvelope& envelope);

// Promotes the web_geometry scalar geometric_error to the neutral envelope.
// Radiance is unknown at cluster level and bounded by zero only when the
// scalar bound is zero.
[[nodiscard]] RepresentationErrorEnvelope envelope_for_cluster(
    const web_geometry::ClusterRecord& cluster);

// Upgrades a cluster record from the scalar geometric_error to an envelope-
// derived bound: the scalar becomes the maximum of the geometric axes
// (position, silhouette, normal). Attaching is fail-closed: it never tightens
// the existing scalar bound below what the cluster already claimed, because a
// tighter bound is new evidence, not an attachment.
[[nodiscard]] core::Result<void> attach_cluster_error(
    web_geometry::ClusterRecord& cluster,
    const RepresentationErrorEnvelope& envelope);

} // namespace carto::representation
