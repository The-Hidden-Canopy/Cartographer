#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/production/prepared_publication.hpp>
#include <carto/production/prepared_scene.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::verification {

// One expected neutral capability result. Expected capabilities form an
// exact set: a gate fails when the prepared scene drifts by even one result.
struct CapabilityExpectation {
    std::string feature;
    production::PreparedCapabilityEvidence evidence =
        production::PreparedCapabilityEvidence::preserved_source;
    bool required = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool matches(
        const production::PreparedCapabilityResult& result) const noexcept;
};

// Expected number of source-to-prepared correspondences for one entity pair.
// Keys are unique within a gate spec and stored sorted for canonical form.
struct CorrespondenceCountExpectation {
    production::PreparedSourceEntityKind source_kind =
        production::PreparedSourceEntityKind::mesh_asset;
    production::PreparedEntityKind prepared_kind = production::PreparedEntityKind::asset;
    std::uint64_t expected_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

// Deterministic, engine-neutral regression gate over one prepared scene. The
// gate never imports game content and never touches runtime state; it binds
// expected digests, capability results, and correspondence counts to one
// published product set.
struct RegressionGateSpec {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxExpectations = 4096U;
    static constexpr std::size_t kMaxFeatureBytes = 256U;
    static constexpr std::size_t kMaxSerializedBytes = 4U * 1024U * 1024U;

    assets::Sha256Digest expected_prepared_digest;
    std::vector<CapabilityExpectation> expected_capabilities;
    std::vector<CorrespondenceCountExpectation> expected_correspondence_counts;

    [[nodiscard]] core::Result<void> validate() const;
    // Canonical form: expectations sorted by feature and correspondence keys
    // sorted by (source_kind, prepared_kind).
    [[nodiscard]] core::Result<void> normalize();
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<RegressionGateSpec> deserialize(
        std::string_view text);
};

// Outcome of one gate evaluation. A gate "failure" is data, not an error:
// Result failure is reserved for invalid specs or unverifiable input.
struct RegressionGateOutcome {
    static constexpr std::size_t kMaxFailures = 64U;
    static constexpr std::size_t kMaxFailureBytes = 256U;

    bool passed = false;
    assets::Sha256Digest observed_prepared_digest;
    std::vector<std::string> failures;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] static core::Result<RegressionGateOutcome> deserialize(
        std::string_view text);
};

// Headless gate runner: evaluates one spec against one composed prepared
// scene and, when supplied, its publication receipt. All comparisons are
// exact; any drift appends one bounded failure reason.
[[nodiscard]] core::Result<RegressionGateOutcome> run_regression_gate(
    const RegressionGateSpec& spec,
    const production::PreparedSceneEnvelope& prepared,
    const production::PreparedPublicationReceipt* publication = nullptr);

} // namespace carto::verification
