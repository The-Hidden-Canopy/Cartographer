#include <carto/production/artifact_store.hpp>

#include <utility>

namespace carto::production {

namespace {

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

core::Result<void> ProductionArtifactStore::publish(
    ProductionArtifactReceipt receipt,
    std::span<const std::uint8_t> payload,
    const AuthoritativeSource& current_source,
    const AdmissionPolicy& policy) {
    if (auto result = validate_publication(receipt, current_source, policy); !result) {
        return result;
    }
    if (receipt.output_bytes != payload.size() ||
        receipt.output_digest != assets::sha256(payload)) {
        return core::Result<void>::failure(validation(
            "production payload does not match its admitted output identity"));
    }

    // Reject deterministic identity conflicts before touching content-addressed
    // storage. The ledger performs the same check at append time so direct
    // ledger users and store users share one fail-closed contract.
    const auto existing = ledger_.query(receipt.key);
    if (!existing) return core::Result<void>::failure(existing.error());
    if (existing.value().has_value() &&
        (existing.value()->output_digest != receipt.output_digest ||
         existing.value()->output_bytes != receipt.output_bytes)) {
        return core::Result<void>::failure(validation(
            "deterministic production cook key has conflicting admitted output"));
    }

    const auto stored = blobs_.put(payload, "application/vnd.cartographer.derived");
    if (!stored) return core::Result<void>::failure(stored.error());
    if (stored.value().digest != receipt.output_digest ||
        stored.value().bytes != receipt.output_bytes) {
        return core::Result<void>::failure(validation(
            "content-addressed production blob disagrees with its receipt"));
    }

    // Blob publication is immutable. If ledger admission fails, the blob is
    // unreachable from this store's evidence and can be garbage-collected by
    // a later maintenance pass; it is never exposed as admitted output.
    return ledger_.append(std::move(receipt));
}

core::Result<std::vector<std::uint8_t>> ProductionArtifactStore::read(
    const ProductionCookKey& key,
    const AuthoritativeSource& current_source,
    const AdmissionPolicy& policy) const {
    const auto record = ledger_.query(key);
    if (!record) return core::Result<std::vector<std::uint8_t>>::failure(record.error());
    if (!record.value().has_value()) {
        return core::Result<std::vector<std::uint8_t>>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "no admitted production artifact exists for the requested cook key"));
    }
    const auto& receipt = record.value().value();
    if (auto result = validate_publication(receipt, current_source, policy); !result) {
        return core::Result<std::vector<std::uint8_t>>::failure(result.error());
    }
    const auto payload = blobs_.read(receipt.output_digest);
    if (!payload) return payload;
    if (payload.value().size() != receipt.output_bytes ||
        assets::sha256(payload.value()) != receipt.output_digest) {
        return core::Result<std::vector<std::uint8_t>>::failure(validation(
            "stored production payload failed receipt verification"));
    }
    return payload;
}

core::Result<std::optional<ProductionArtifactReceipt>> ProductionArtifactStore::find(
    const ProductionCookKey& key) const {
    return ledger_.query(key);
}

} // namespace carto::production
