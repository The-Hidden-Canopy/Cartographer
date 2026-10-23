#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/semantic/lowering.hpp>
#include <carto/semantic/schema.hpp>
#include <carto/semantic/sidecar.hpp>
#include <carto/world/model.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::semantic {

// Verification tiers never silently promote into one another: structural
// validation, reference behavior, native backend evidence, and full-game
// acceptance are distinct claims with distinct evidence obligations.
enum class VerificationTier : std::uint8_t {
    structural,
    reference,
    native,
    full_game,
};

[[nodiscard]] constexpr bool operator<(
    VerificationTier left, VerificationTier right) noexcept {
    return static_cast<std::uint8_t>(left) < static_cast<std::uint8_t>(right);
}

[[nodiscard]] std::string_view verification_tier_name(VerificationTier tier) noexcept;

// Resolution of one bound authored identity against document context.
struct BindingResolution {
    world::AuthoringBinding binding;
    bool resolved = false;
    std::string detail;
};

// One schema disagreement discovered during inspection.
struct SchemaMismatch {
    SchemaIdentity schema;
    SchemaVersion expected;
    SchemaVersion found;
    std::string detail;

    [[nodiscard]] auto operator<=>(const SchemaMismatch&) const noexcept = default;
};

// Serializable inspection evidence over one project world model, one semantic
// sidecar, and the schema registry. Authored tags without a lowering binding
// are reported as dropped; bound tags are reported as lowered.
struct SemanticInspectionReport {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxResolutions = 65'536U;
    static constexpr std::size_t kMaxTags = 65'536U;
    static constexpr std::size_t kMaxMismatches = 65'536U;

    VerificationTier tier = VerificationTier::structural;
    std::vector<BindingResolution> binding_resolutions;
    // Sorted, unique.
    std::vector<std::string> dropped_tags;
    // Sorted, unique.
    std::vector<std::string> lowered_tags;
    std::vector<SchemaMismatch> schema_mismatches;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SemanticInspectionReport> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Resolves each record's bound authored identities through the supplied
// document-context predicate, reconciles every record against the registry,
// and classifies authored world tags using the lowering map (null lowering
// maps report every authored tag as dropped).
[[nodiscard]] SemanticInspectionReport inspect_semantic_bindings(
    const world::WorldModel& model,
    const SemanticSidecar& sidecar,
    const SchemaRegistry& registry,
    const TagLoweringMap* lowering,
    VerificationTier tier,
    const std::function<bool(const world::AuthoringBinding&)>& resolve_binding);

} // namespace carto::semantic
