#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/capability_profile.hpp>
#include <carto/production/closure.hpp>
#include <carto/production/preparation_scope.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::production {

inline constexpr std::uint32_t kProductionRecipeVersion = 1U;
inline constexpr std::size_t kMaxProductionRecipeBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaxRecipeProductRequests = 256U;
inline constexpr std::size_t kMaxRecipeSources = 4'096U;

// One demanded product family inside a recipe. The algorithm and settings
// digest name the exact derivation; the representation kind optionally
// distinguishes consumer representations without exposing selection policy.
struct RecipeProductRequest {
    ArtifactKind kind = ArtifactKind::render_mesh;
    std::string algorithm;
    assets::Sha256Digest settings_digest;
    std::string representation_kind;
    bool required = true;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string canonical() const;
};

// Declarative, digest-bound cooking demand. A recipe names which product
// families the destination needs, over which exact authoritative sources,
// under which destination capability profile and budget policy, and within
// which preparation scope. It contains no runtime handles and no mutation
// authority; expansion crosses sources with product requests to deterministic
// ProductionCookKeys and refuses when a required capability is unsupported by
// the bound destination profile.
class ProductionRecipe final {
public:
    static constexpr std::uint32_t kSchemaVersion = kProductionRecipeVersion;
    static constexpr std::size_t kMaxSerializedBytes = kMaxProductionRecipeBytes;
    static constexpr std::size_t kMaxProducts = kMaxRecipeProductRequests;
    static constexpr std::size_t kMaxSources = kMaxRecipeSources;

    std::string identity;
    std::string source_namespace;
    core::Revision source_revision;
    PreparationScopeRequest scope;
    // Canonically sorted and unique by source identity.
    std::vector<AuthoritativeSource> sources;
    // Canonically sorted and unique by (kind, algorithm, settings, representation).
    std::vector<RecipeProductRequest> products;
    std::string destination_profile_identity;
    assets::Sha256Digest destination_profile_digest;
    // Digest of the AdmissionPolicy bounding this demand (see closure.hpp).
    assets::Sha256Digest budget_policy_digest;
    ExecutionForm form = ExecutionForm::cached;
    std::string platform_profile;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<ProductionRecipe> deserialize(
        std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// The deterministic capability feature name a product request checks against
// a DestinationCapabilityProfile: "destination.artifact.<kind>".
[[nodiscard]] std::string artifact_capability_feature(ArtifactKind kind);

// Expands a recipe against the exact destination profile it is bound to.
// The profile digest must equal recipe.destination_profile_digest. Every
// required product request whose capability feature the profile does not
// offer fails with unsupported evidence; optional unsupported requests are
// skipped. The returned cook keys are sorted by their canonical form and
// unique, so identical demand always expands to identical cache identities.
[[nodiscard]] core::Result<std::vector<ProductionCookKey>> expand_production_recipe(
    const ProductionRecipe& recipe,
    const DestinationCapabilityProfile& destination);

} // namespace carto::production
