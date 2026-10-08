#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

enum class ArtifactKind {
    render_mesh,
    material_package,
    residency_page,
    publication_package,
    evidence_bundle,
};

enum class ExecutionForm {
    interactive,
    cached,
    offline,
    learned_proposal,
    temporal,
};

enum class ReceiptStatus {
    admitted,
    failed,
    stale,
    cancelled,
};

[[nodiscard]] const char* artifact_kind_name(ArtifactKind kind) noexcept;
[[nodiscard]] const char* execution_form_name(ExecutionForm form) noexcept;
[[nodiscard]] const char* receipt_status_name(ReceiptStatus status) noexcept;

struct AuthoritativeSource {
    std::string identity;
    core::Revision revision;
    assets::Sha256Digest content_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const AuthoritativeSource&) const noexcept = default;
};

// Opaque, stable profile identifiers let public cook receipts bind a
// decision context without carrying the private policy that interprets it.
struct ProductionDecisionContext {
    std::string truth_class;
    std::string observer_class;
    std::string precision_requirement;
    std::string materialization_requirement;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const ProductionDecisionContext&) const noexcept = default;
};

// A cook key binds a derived product to the exact source, algorithm, settings,
// platform, and execution form that produced it, plus optional decision scope.
// It is intentionally public so Cartographer workers and runtime adapters can
// share the same admission key without exposing policy implementation.
struct ProductionCookKey {
    AuthoritativeSource source;
    ArtifactKind kind = ArtifactKind::render_mesh;
    ExecutionForm form = ExecutionForm::interactive;
    std::string algorithm;
    assets::Sha256Digest settings_digest;
    std::string platform_profile;
    // Optional stable identifiers distinguish consumer representations and
    // spatial scopes without exposing the policy that selected them.
    std::string representation_kind;
    std::string spatial_scope;
    ProductionDecisionContext decision_context;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] bool operator==(const ProductionCookKey&) const noexcept = default;
};

struct QualityTelemetry {
    double measured_error = 0.0;
    double importance = 0.0;
    double confidence = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct CostTelemetry {
    double cook_milliseconds = 0.0;
    double cpu_milliseconds = 0.0;
    double gpu_milliseconds = 0.0;
    std::uint64_t resident_bytes = 0U;
    std::uint64_t disk_bytes = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

struct ProductionArtifactReceipt {
    ProductionCookKey key;
    ReceiptStatus status = ReceiptStatus::failed;
    assets::Sha256Digest output_digest;
    std::uint64_t output_bytes = 0U;
    QualityTelemetry quality;
    CostTelemetry cost;
    assets::Sha256Digest receipt_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

struct AdmissionPolicy {
    std::optional<double> max_measured_error;
    double min_confidence = 0.0;
    std::optional<std::uint64_t> max_resident_bytes;
    std::optional<double> max_cook_milliseconds;
    std::optional<double> max_cpu_milliseconds;
    std::optional<double> max_gpu_milliseconds;
    std::optional<std::uint64_t> max_disk_bytes;
    std::optional<std::uint64_t> max_output_bytes;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Validates a receipt immediately before publication. A receipt may be valid
// evidence but still be rejected when its source is stale or its measured
// quality/cost falls outside the current policy.
[[nodiscard]] core::Result<void> validate_publication(
    const ProductionArtifactReceipt& receipt,
    const AuthoritativeSource& current_source,
    const AdmissionPolicy& policy);

class ProductionReceiptLedger final {
public:
    static constexpr std::size_t max_receipts = 4096U;
    static constexpr std::size_t max_serialized_bytes = 4U * 1024U * 1024U;

    // Append-only evidence. Exact retries are idempotent, while two admitted
    // outputs for one deterministic cook key fail closed. A failed, stale, or
    // cancelled record supersedes an earlier admission; query never falls back.
    [[nodiscard]] core::Result<void> append(ProductionArtifactReceipt receipt);
    [[nodiscard]] core::Result<std::optional<ProductionArtifactReceipt>> query(
        const ProductionCookKey& key) const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ProductionReceiptLedger> deserialize(
        std::string_view text);

    [[nodiscard]] const std::vector<ProductionArtifactReceipt>& records() const noexcept {
        return records_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::vector<ProductionArtifactReceipt> records_;
};

} // namespace carto::production
