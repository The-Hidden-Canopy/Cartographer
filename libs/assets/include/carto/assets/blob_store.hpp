#pragma once

#include <carto/core/result.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::assets {

struct Sha256Digest {
    std::array<std::uint8_t, 32> bytes{};

    [[nodiscard]] bool is_zero() const noexcept;
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] static core::Result<Sha256Digest> from_hex(std::string_view value);
    [[nodiscard]] constexpr auto operator<=>(const Sha256Digest&) const noexcept = default;
};

[[nodiscard]] Sha256Digest sha256(std::span<const std::uint8_t> bytes);

struct BlobRef {
    Sha256Digest digest;
    std::uint64_t bytes = 0U;
    std::string media_type;
};

struct BlobStoreLimits {
    std::uint64_t max_blob_bytes = 256ULL * 1024ULL * 1024ULL;
    std::uint32_t max_media_type_bytes = 256U;
    std::uint32_t max_listed_blobs = 1'048'576U;
    std::uint64_t max_inventory_verification_bytes = 512ULL * 1024ULL * 1024ULL;
};

struct BlobOrphanReport {
    // Stored digests absent from both the caller-supplied live and pending-work
    // sets, sorted by digest. Advisory only: deletion authority stays with the
    // caller and computing this report never mutates the store.
    std::vector<Sha256Digest> orphan_candidates;
    // Stored digests excluded from orphan_candidates because their bytes
    // could not be read and digest-verified within configured limits.
    // Sorted by digest.
    std::vector<Sha256Digest> corrupt;
    // Caller-supplied live digests absent from the store, sorted and unique.
    std::vector<Sha256Digest> missing_live_references;
    // Verified stored digests protected by caller-supplied pending work, sorted
    // and unique. They are excluded from orphan_candidates but are not treated
    // as required durable references when computing missing_live_references.
    std::vector<Sha256Digest> protected_pending_work_digests;
};

inline constexpr std::size_t kMaxPendingBlobWorkDigests = 65'536U;

class BlobStore {
public:
    explicit BlobStore(std::filesystem::path root, BlobStoreLimits limits = {});

    [[nodiscard]] core::Result<BlobRef> put(
        std::span<const std::uint8_t> bytes,
        std::string media_type);
    [[nodiscard]] core::Result<BlobRef> put(
        std::string_view bytes,
        std::string media_type);
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read(
        const Sha256Digest& digest) const;
    [[nodiscard]] core::Result<void> verify(const Sha256Digest& digest) const;

    // Read-only enumeration of the store, sorted by digest. Media types are
    // caller metadata and are not persisted, so listed entries carry an empty
    // media type. Fails closed if the store exceeds limits_.max_listed_blobs.
    [[nodiscard]] core::Result<std::vector<BlobRef>> list() const;
    // Pending work suppresses orphan classification only; it does not count
    // as a durable live reference or contribute to missing_live_references.
    [[nodiscard]] core::Result<BlobOrphanReport> orphan_candidates(
        std::span<const Sha256Digest> live_digests,
        std::span<const Sha256Digest> pending_work_digests = {}) const;

    [[nodiscard]] std::filesystem::path path_for(const Sha256Digest& digest) const;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    BlobStoreLimits limits_;
};

} // namespace carto::assets
