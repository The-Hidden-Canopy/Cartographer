#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/connection/graph.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/sweep.hpp>
#include <carto/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace carto::condition {

enum class SweepEndpointSide : std::uint8_t {
    start = 0U,
    end = 1U,
};

// Identifies which end of an exact sweep-source revision is represented by a
// generic authored connection port. This contract carries no attachment state
// and does not claim that the port frame is spatially snapped to the curve.
struct SweepEndpointPortBinding final {
    static constexpr std::size_t kMaxIdentityBytes = 128U;

    std::string sweep_source_identity;
    core::Revision sweep_source_revision;
    SweepEndpointSide side = SweepEndpointSide::start;
    connection::PortId port_id;

    [[nodiscard]] core::Result<void> validate(
        const geometry::SweepSource& source,
        const connection::ConnectionPort& port) const;
};

// Deterministic evidence that two revision-bound sweep endpoints' generic
// connection ports passed the existing tag/capability compatibility gate.
// This is a pure authoring check; it does not connect or mutate a graph.
struct SweepEndpointCompatibilityEvidence final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 512U * 1024U;

    SweepEndpointPortBinding first;
    SweepEndpointPortBinding second;
    assets::Sha256Digest first_sweep_digest;
    assets::Sha256Digest second_sweep_digest;
    assets::Sha256Digest first_port_digest;
    assets::Sha256Digest second_port_digest;
    connection::PortCompatibilityEvidence port_compatibility;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Explicit caller policy for a read-only spatial review. The local port axis
// has no implicit meaning: callers choose which axis of the authored frame
// represents the endpoint's outward direction and set their own tolerances.
struct SweepEndpointSpatialPolicy final {
    core::Vec3d port_outward_axis_local{};
    double maximum_position_error = -1.0;
    double minimum_axis_alignment = 2.0;

    [[nodiscard]] core::Result<void> validate() const;
};

// Measured relation between one exact sweep endpoint and its graph port.
// This is neither an attachment edge nor a snap/fitting command. Its digest
// binds the current source, path, port frame chain, project/scene revisions,
// and explicit caller tolerances.
struct SweepEndpointSpatialReview final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxFrameDepth = 64U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    SweepEndpointPortBinding binding;
    std::uint64_t project_generation = 0U;
    core::Revision project_revision;
    core::Revision scene_revision;
    assets::Sha256Digest sweep_source_digest;
    assets::Sha256Digest centerline_digest;
    assets::Sha256Digest port_digest;
    std::vector<std::uint64_t> frame_chain_port_ids;
    std::vector<assets::Sha256Digest> frame_chain_port_digests;
    SweepEndpointSpatialPolicy policy;
    core::Vec3d sweep_endpoint_world;
    core::Vec3d sweep_outward_axis_world;
    core::Vec3d port_origin_world;
    core::Vec3d port_outward_axis_world;
    double position_error = 0.0;
    double axis_alignment = 0.0;
    bool position_within_tolerance = false;
    bool axis_within_tolerance = false;
    bool aligned = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

[[nodiscard]] core::Result<SweepEndpointCompatibilityEvidence>
check_sweep_endpoint_compatibility(
    const geometry::SweepSource& first_source,
    const SweepEndpointPortBinding& first_binding,
    const connection::ConnectionPort& first_port,
    const geometry::SweepSource& second_source,
    const SweepEndpointPortBinding& second_binding,
    const connection::ConnectionPort& second_port);

[[nodiscard]] core::Result<SweepEndpointSpatialReview>
review_sweep_endpoint_spatial_fit(
    const geometry::SweepSource& source,
    const geometry::CurvePath& centerline,
    const SweepEndpointPortBinding& binding,
    const connection::ConnectionPortGraph& ports,
    const scene::Scene& scene,
    const SweepEndpointSpatialPolicy& policy,
    std::uint64_t project_generation,
    core::Revision project_revision);

} // namespace carto::condition
