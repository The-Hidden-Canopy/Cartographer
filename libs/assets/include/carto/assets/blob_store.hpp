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
};

struct BlobOrphanReport {
    // Stored digests absent from the caller-supplied live set, sorted by
    // digest. Advisory only: deletion authority stays with the caller and
    // computing this report never mutates the store.
    std::vector<Sha256Digest> orphan_candidates;
    // Stored digests excluded from orphan_candidates because their bytes
    // failed digest re-verification, sorted by digest.
    std::vector<Sha256Digest> corrupt;
};

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
    [[nodiscard]] core::Result<BlobOrphanReport> orphan_candidates(
        std::span<const Sha256Digest> live_digests) const;

    [[nodiscard]] std::filesystem::path path_for(const Sha256Digest& digest) const;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
    BlobStoreLimits limits_;
};

} // namespace carto::assets
