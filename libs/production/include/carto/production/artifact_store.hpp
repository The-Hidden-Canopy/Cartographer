#pragma once

#include <carto/production/closure.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace carto::production {

// Binds immutable content-addressed bytes to an admitted production receipt.
// The blob store owns bytes; this layer owns admission and source freshness.
// One instance is intentionally single-writer; callers must serialize access
// until a durable multi-worker ledger owns cross-thread/process publication.
class ProductionArtifactStore final {
public:
    explicit ProductionArtifactStore(assets::BlobStore& blobs) noexcept
        : blobs_(blobs) {}

    [[nodiscard]] core::Result<void> publish(
        ProductionArtifactReceipt receipt,
        std::span<const std::uint8_t> payload,
        const AuthoritativeSource& current_source,
        const AdmissionPolicy& policy);

    [[nodiscard]] core::Result<std::vector<std::uint8_t>> read(
        const ProductionCookKey& key,
        const AuthoritativeSource& current_source,
        const AdmissionPolicy& policy) const;

    [[nodiscard]] core::Result<std::optional<ProductionArtifactReceipt>> find(
        const ProductionCookKey& key) const;

    [[nodiscard]] const ProductionReceiptLedger& ledger() const noexcept {
        return ledger_;
    }

private:
    assets::BlobStore& blobs_;
    ProductionReceiptLedger ledger_;
};

} // namespace carto::production
