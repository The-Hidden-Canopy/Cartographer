#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/representation/error_envelope.hpp>
#include <carto/representation/family.hpp>
#include <carto/representation/runtime_evidence.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::representation {

// Public representation records describe what was measured and why a
// candidate is eligible. They do not contain a runtime device handle or the
// private algorithm that produced the payload.
enum class ResidualMode : std::uint8_t {
    none,
    normal,
    displacement,
    radiance,
    sparse_patch,
};

[[nodiscard]] std::string_view residual_mode_name(ResidualMode mode) noexcept;
[[nodiscard]] core::Result<ResidualMode> residual_mode_from_name(
    std::string_view name);

struct ResidualSurfaceCodecRecord {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U;

    AuthoredSourceIdentity source;
    RepresentationVariantId candidate;
    assets::Sha256Digest base_digest;
    ResidualMode residual_mode = ResidualMode::none;
    assets::Sha256Digest residual_digest;
    std::uint64_t base_bytes = 0U;
    std::uint64_t residual_bytes = 0U;
    double reconstruction_error = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

enum class PerceptualRole : std::uint8_t {
    silhouette,
    identity,
    contact,
    interaction,
    deformation,
    shading,
    radiance,
    microstructure,
};

[[nodiscard]] std::string_view perceptual_role_name(PerceptualRole role) noexcept;
[[nodiscard]] core::Result<PerceptualRole> perceptual_role_from_name(
    std::string_view name);

struct PerceptualRoleWeight {
    std::uint64_t region_id = 0U;
    PerceptualRole role = PerceptualRole::silhouette;
    double importance = 0.0;
    bool protected_invariant = false;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PerceptualRoleMap {
    static constexpr std::size_t kMaxRegions = 4096U;

    AuthoredSourceIdentity source;
    // Sorted by (region_id, role) and unique by that pair.
    std::vector<PerceptualRoleWeight> regions;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct VisualFeatureRecall {
    std::uint64_t feature_id = 0U;
    PerceptualRole role = PerceptualRole::silhouette;
    double importance = 0.0;
    double recall = 0.0;
    double false_suppression = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
};

struct VisualFeatureRecallMetrics {
    static constexpr std::size_t kMaxFeatures = 4096U;

    AuthoredSourceIdentity source;
    RepresentationVariantId candidate;
    assets::Sha256Digest observation_digest;
    std::uint32_t observation_count = 0U;
    // Sorted by feature_id. A missing feature is not silently treated as a
    // successful observation; callers must record its measured recall.
    std::vector<VisualFeatureRecall> features;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] double weighted_recall() const noexcept;
    [[nodiscard]] double weighted_false_suppression() const noexcept;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct MeasuredSurfaceCost {
    static constexpr std::size_t kMaxHardwareProfileBytes = 128U;

    RepresentationVariantId candidate;
    // Cost observations bind the complete authored source identity, not only
    // a revision number. Revisions are scoped to a source namespace and are
    // therefore not sufficient on their own for evidence reuse.
    std::string source_namespace;
    assets::Sha256Digest source_digest;
    std::string hardware_profile;
    assets::Sha256Digest profile_digest;
    core::Revision source_revision;
    EvidenceStatus status = EvidenceStatus::failed;
    // Optional values are explicit: an unavailable device metric is not
    // encoded as a fabricated zero. Resident bytes are always required.
    std::optional<std::uint64_t> gpu_time_ns;
    std::optional<std::uint64_t> host_time_ns;
    std::uint64_t resident_bytes = 0U;
    std::optional<std::uint64_t> bandwidth_bytes;
    std::optional<double> overdraw;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

enum class RepresentationPromotionState : std::uint8_t {
    experimental,
    certified,
    rejected,
};

[[nodiscard]] std::string_view promotion_state_name(
    RepresentationPromotionState state) noexcept;
[[nodiscard]] core::Result<RepresentationPromotionState> promotion_state_from_name(
    std::string_view name);

struct PerceptualFrontierCandidate {
    AuthoredSourceIdentity source;
    RepresentationVariantId candidate;
    RepresentationPurpose purpose = RepresentationPurpose::render;
    RepresentationErrorEnvelope error;
    VisualFeatureRecallMetrics feature_recall;
    MeasuredSurfaceCost cost;
    RepresentationPromotionState promotion = RepresentationPromotionState::experimental;
    // A certified state is only admissible when it points at a separately
    // validated promotion/certification receipt. The public layer carries the
    // receipt digest, never the private promotion algorithm or telemetry.
    assets::Sha256Digest promotion_evidence_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] double quality_score() const noexcept;
    [[nodiscard]] std::string canonical() const;
};

// The frontier is source anchored and contains only measured candidates that
// are not dominated on all declared quality and cost axes. It is a public
// evidence structure, not a runtime policy implementation.
class RepresentationFrontier final {
public:
    static constexpr std::size_t kMaxCandidates = 1024U;

    [[nodiscard]] static core::Result<RepresentationFrontier> build(
        std::span<const PerceptualFrontierCandidate> candidates);

    [[nodiscard]] const AuthoredSourceIdentity& source() const noexcept {
        return source_;
    }
    [[nodiscard]] std::string_view hardware_profile() const noexcept {
        return hardware_profile_;
    }
    [[nodiscard]] const assets::Sha256Digest& profile_digest() const noexcept {
        return profile_digest_;
    }
    [[nodiscard]] const std::vector<PerceptualFrontierCandidate>& candidates()
        const noexcept {
        return candidates_;
    }

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    AuthoredSourceIdentity source_;
    std::string hardware_profile_;
    assets::Sha256Digest profile_digest_;
    std::vector<PerceptualFrontierCandidate> candidates_;
};

struct PerceptualBudget {
    RepresentationPurpose purpose = RepresentationPurpose::render;
    RepresentationErrorEnvelope max_error;
    double min_feature_recall = 0.0;
    double max_false_suppression = 1.0;
    std::optional<std::uint64_t> max_gpu_time_ns;
    std::optional<std::uint64_t> max_host_time_ns;
    std::optional<std::uint64_t> max_resident_bytes;
    std::optional<std::uint64_t> max_bandwidth_bytes;
    bool require_certified = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct MeasuredCostDecision {
    static constexpr std::uint32_t kSchemaVersion = 1U;

    RepresentationVariantId selected_candidate;
    core::Revision source_revision;
    assets::Sha256Digest source_digest;
    std::string hardware_profile;
    assets::Sha256Digest profile_digest;
    assets::Sha256Digest frontier_digest;
    assets::Sha256Digest budget_digest;
    std::string algorithm = "cartographer-measured-cost-governor/0.1";
    assets::Sha256Digest decision_digest;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical_without_digest() const;
    [[nodiscard]] core::Result<void> refresh_digest();
};

class MeasuredCostGovernor final {
public:
    // Selects only a candidate with explicit measured evidence and an
    // admissible perceptual envelope. It never substitutes nominal tier,
    // triangle count, or a stale/failed observation for missing evidence.
    [[nodiscard]] static core::Result<MeasuredCostDecision> select(
        const RepresentationFrontier& frontier,
        const PerceptualBudget& budget);
};

} // namespace carto::representation
