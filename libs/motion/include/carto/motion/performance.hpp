#pragma once

#include <carto/motion/ids.hpp>
#include <carto/motion/mocap.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/revision.hpp>
#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::motion {

// One reusable performance take: an immutable, digest-bound revision of an
// accepted capture ingest plus its adaptation profile. A take dropped onto a
// timeline produces a source-bound instance referencing this identity and
// revision, never a destructive copy.
struct PerformanceTake {
    static constexpr std::size_t kMaxNameBytes = 256U;
    static constexpr std::size_t kMaxTagBytes = 128U;
    static constexpr std::size_t kMaxTags = 64U;

    PerformanceTakeId id;
    std::string take_name;
    core::Revision revision = core::Revision{1U};
    // Digest of the accepted MocapIngestRecord this take derives from.
    assets::Sha256Digest ingest_digest;
    // Digest of the retarget receipt when the take is adapted to a rig;
    // absent for unretargeted source takes.
    std::optional<assets::Sha256Digest> retarget_receipt_digest;
    // Sorted, unique, searchable tags.
    std::vector<std::string> tags;

    [[nodiscard]] core::Result<void> validate() const;
};

// The bounded performance take catalog. Takes are sorted by identity,
// identities are unique, and the catalog fails closed past its capacity.
class PerformanceTakeCatalog final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::size_t kMaxTakes = 4'096U;

    std::vector<PerformanceTake> takes;

    [[nodiscard]] core::Result<void> insert(PerformanceTake take);
    [[nodiscard]] const PerformanceTake* find(PerformanceTakeId id) const noexcept;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<PerformanceTakeCatalog> deserialize(
        std::string_view text);
};

} // namespace carto::motion
