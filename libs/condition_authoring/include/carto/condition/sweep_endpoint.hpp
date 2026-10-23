#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/connection/port.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/sweep.hpp>

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

[[nodiscard]] core::Result<SweepEndpointCompatibilityEvidence>
check_sweep_endpoint_compatibility(
    const geometry::SweepSource& first_source,
    const SweepEndpointPortBinding& first_binding,
    const connection::ConnectionPort& first_port,
    const geometry::SweepSource& second_source,
    const SweepEndpointPortBinding& second_binding,
    const connection::ConnectionPort& second_port);

} // namespace carto::condition
