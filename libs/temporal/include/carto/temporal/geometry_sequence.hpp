#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/temporal/channel.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::temporal {

struct GeometrySequenceFrame {
    std::uint64_t frame_index = 0U;
    double time_seconds = 0.0;
    std::string source_locator;
    assets::Sha256Digest source_digest;
    std::uint64_t mesh_asset = 0U;
    core::Revision mesh_revision;
    assets::Sha256Digest geometry_digest;
    assets::Sha256Digest topology_digest;
    assets::Sha256Digest correspondence_digest;
};

// A sampled geometry source manifest. Per-frame source, geometry, topology,
// and correspondence digests stay distinct so changing topology is explicit
// instead of being mistaken for a motion-only deformation.
class GeometrySequence final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxFrames = 65'536U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;

    SequenceId id;
    std::string source_identity;
    std::string source_revision;
    assets::Sha256Digest source_digest;
    core::Revision revision;
    bool topology_stable = true;
    std::vector<GeometrySequenceFrame> frames;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<GeometrySequence> deserialize(
        std::string_view bytes);
    [[nodiscard]] core::Result<void> validate_source(
        std::string_view current_identity,
        std::string_view current_revision,
        const assets::Sha256Digest& current_digest) const;
};

} // namespace carto::temporal
