#pragma once

#include <carto/weights/transfer.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace carto::weights {

// Immutable transfer result as a portable source record. It contains the
// complete per-vertex binding, the layer metadata and transfer receipt; it is
// not itself a project transaction. A digest verifies internal consistency,
// not that the claimed transfer actually ran or that the source still exists.
struct TransferredWeightRecord {
    static constexpr std::size_t kMaxSerializedBytes =
        kinematic::SkinBinding::kMaxSerializedBytes +
        WeightTransferReceipt::kMaxSerializedBytes + 4096U;

    WeightTransferOutput output;

    [[nodiscard]] core::Result<void> validate() const;
    // In addition to record validation, require the exact graph and resolve
    // every influenced joint. The caller must separately verify the mesh.
    [[nodiscard]] core::Result<void> validate_against_graph(
        const kinematic::KinematicGraph& graph) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<TransferredWeightRecord> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

} // namespace carto::weights
