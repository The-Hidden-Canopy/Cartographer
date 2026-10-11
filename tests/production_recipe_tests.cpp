#include <carto/assets/blob_store.hpp>
#include <carto/core/revision.hpp>
#include <carto/production/recipe.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

carto::assets::Sha256Digest digest(std::string_view value) {
    return carto::assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

using namespace carto;

production::DestinationCapabilityProfile make_profile() {
    production::DestinationCapabilityProfile profile;
    profile.identity = "vanta.destination.handheld.v1";
    profile.profile_revision = core::Revision{2U};
    profile.capabilities.push_back(production::DestinationCapability{
        "destination.artifact.material-package",
        production::PreparedCapabilityEvidence::native_realization,
        "native material graph realized on device"});
    profile.capabilities.push_back(production::DestinationCapability{
        "destination.artifact.render-mesh",
        production::PreparedCapabilityEvidence::runtime_validated,
        "render mesh path validated on device"});
    require(static_cast<bool>(profile.validate()), "fixture profile validates");
    return profile;
}

production::AuthoritativeSource make_source(std::string identity) {
    production::AuthoritativeSource source;
    source.identity = std::move(identity);
    source.revision = core::Revision{4U};
    source.content_digest = digest("content-" + source.identity);
    return source;
}

production::RecipeProductRequest make_request(
    production::ArtifactKind kind,
    std::string algorithm,
    std::string representation,
    bool required = true) {
    production::RecipeProductRequest request;
    request.kind = kind;
    request.algorithm = std::move(algorithm);
    request.settings_digest = digest("settings-" + request.algorithm);
    request.representation_kind = std::move(representation);
    request.required = required;
    return request;
}

production::ProductionRecipe make_recipe(
    const assets::Sha256Digest& profile_digest) {
    production::ProductionRecipe recipe;
    recipe.identity = "recipe.camp-outpost.v1";
    recipe.source_namespace = "torture.world";
    recipe.source_revision = core::Revision{4U};
    recipe.scope.kind = production::PreparationScopeKind::scene;
    recipe.scope.expected_source_revision = core::Revision{4U};
    recipe.sources.push_back(make_source("source.mesh.a"));
    recipe.sources.push_back(make_source("source.mesh.b"));
    // Product requests are canonically sorted by (kind, algorithm, settings).
    recipe.products.push_back(make_request(production::ArtifactKind::material_package,
        "cook.material-v1", ""));
    recipe.products.push_back(make_request(production::ArtifactKind::render_mesh,
        "cook.static-v1", "lod0"));
    recipe.destination_profile_identity = "vanta.destination.handheld.v1";
    recipe.destination_profile_digest = profile_digest;
    recipe.budget_policy_digest = digest("budget-policy-v1");
    recipe.form = production::ExecutionForm::cached;
    recipe.platform_profile = "handheld.d3d12";
    require(static_cast<bool>(recipe.validate()), "fixture recipe validates");
    return recipe;
}

void recipe_round_trips_with_stable_digest() {
    const auto profile = make_profile();
    const auto profile_digest = profile.canonical_digest();
    require(static_cast<bool>(profile_digest), "fixture profile digest computes");
    const auto recipe = make_recipe(profile_digest.value());

    const std::string encoded = recipe.serialize();
    const auto reopened = production::ProductionRecipe::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "production recipe survives deterministic round trip");
    const auto first = recipe.canonical_digest();
    const auto second = reopened.value().canonical_digest();
    require(first && second && first.value() == second.value(),
        "production recipe identity survives reopen");

    auto rebuilt = make_recipe(profile_digest.value());
    require(rebuilt.canonical_digest() && first &&
            rebuilt.canonical_digest().value() == first.value(),
        "identically constructed recipes share one digest");
    rebuilt.budget_policy_digest = digest("budget-policy-v2");
    const auto changed = rebuilt.canonical_digest();
    require(changed && first && changed.value() != first.value(),
        "budget policy drift changes the recipe digest");
}

void expansion_is_deterministic_and_complete() {
    const auto profile = make_profile();
    const auto profile_digest = profile.canonical_digest();
    require(static_cast<bool>(profile_digest), "fixture profile digest computes");
    const auto recipe = make_recipe(profile_digest.value());

    const auto first = production::expand_production_recipe(recipe, profile);
    const auto second = production::expand_production_recipe(recipe, profile);
    require(first && second && first.value().size() == 4U && second.value().size() == 4U,
        "two sources crossed with two product requests expand to four cook keys");
    require(first.value().size() == second.value().size() &&
            std::equal(first.value().begin(), first.value().end(), second.value().begin(),
                [](const production::ProductionCookKey& left,
                    const production::ProductionCookKey& right) {
                    return left.canonical() == right.canonical();
                }),
        "identical demand expands to identical cache identities");

    // Canonical ordering: source identity first, then artifact kind name.
    require(first.value()[0].source.identity == "source.mesh.a" &&
            first.value()[0].kind == production::ArtifactKind::material_package &&
            first.value()[1].source.identity == "source.mesh.a" &&
            first.value()[1].kind == production::ArtifactKind::render_mesh &&
            first.value()[2].source.identity == "source.mesh.b" &&
            first.value()[2].kind == production::ArtifactKind::material_package &&
            first.value()[3].source.identity == "source.mesh.b" &&
            first.value()[3].kind == production::ArtifactKind::render_mesh,
        "expansion order is canonical and deterministic");
    for (const auto& key : first.value()) {
        require(static_cast<bool>(key.validate()),
            "every expanded cook key validates");
    }
    require(first.value()[1].representation_kind == "lod0" &&
            first.value()[1].platform_profile == "handheld.d3d12" &&
            first.value()[1].form == production::ExecutionForm::cached,
        "expansion carries the recipe's representation, platform, and form");
    require(first.value()[0].canonical() < first.value()[1].canonical() &&
            first.value()[1].canonical() < first.value()[2].canonical() &&
            first.value()[2].canonical() < first.value()[3].canonical(),
        "cook keys are strictly sorted by canonical form");

    production::DestinationCapabilityProfile other = profile;
    other.identity = "vanta.destination.console.v1";
    require(!production::expand_production_recipe(recipe, other),
        "a destination whose digest differs from the recipe binding is stale");
}

void unsupported_capabilities_refuse_or_skip() {
    const auto profile = make_profile();
    const auto profile_digest = profile.canonical_digest();
    require(static_cast<bool>(profile_digest), "fixture profile digest computes");

    // Required demand the destination cannot meet refuses the expansion.
    auto refusing = make_recipe(profile_digest.value());
    refusing.products.push_back(make_request(
        production::ArtifactKind::publication_package, "cook.package-v1", ""));
    std::sort(refusing.products.begin(), refusing.products.end(),
        [](const auto& left, const auto& right) {
            return left.canonical() < right.canonical();
        });
    const auto refused = production::expand_production_recipe(refusing, profile);
    require(!refused && refused.error().code == core::ErrorCode::unsupported,
        "a required unsupported capability refuses with explicit evidence");

    // Optional demand the destination cannot meet is skipped, not fatal.
    auto skipping = make_recipe(profile_digest.value());
    skipping.products.push_back(make_request(
        production::ArtifactKind::evidence_bundle, "cook.evidence-v1", "",
        false));
    std::sort(skipping.products.begin(), skipping.products.end(),
        [](const auto& left, const auto& right) {
            return left.canonical() < right.canonical();
        });
    const auto skipped = production::expand_production_recipe(skipping, profile);
    require(skipped && skipped.value().size() == 4U,
        "an optional unsupported capability is skipped while demand proceeds");
    require(std::none_of(skipped.value().begin(), skipped.value().end(),
            [](const production::ProductionCookKey& key) {
                return key.kind == production::ArtifactKind::evidence_bundle;
            }),
        "skipped capabilities never produce cook keys");

    require(production::artifact_capability_feature(production::ArtifactKind::render_mesh) ==
            "destination.artifact.render-mesh",
        "capability feature names are deterministic per artifact kind");
}

void hostile_recipes_fail_closed() {
    const auto profile = make_profile();
    const auto profile_digest = profile.canonical_digest();
    require(static_cast<bool>(profile_digest), "fixture profile digest computes");
    const auto recipe = make_recipe(profile_digest.value());
    const std::string encoded = recipe.serialize();

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_PRODUCTION_RECIPE 1"}.size(),
        "CARTOGRAPHER_PRODUCTION_RECIPE 99");
    require(!production::ProductionRecipe::deserialize(future),
        "future recipe versions fail precisely");
    require(!production::ProductionRecipe::deserialize(encoded + "trailing"),
        "recipe parser rejects trailing data");
    require(!production::ProductionRecipe::deserialize(
                "CARTOGRAPHER_PRODUCTION_RECIPE 1\n"
                "IDENTITY \"recipe.camp-outpost.v1\"\n"
                "SOURCE \"torture.world\" 4\n"
                "SCOPE 3 4 0 0\n"
                "PROFILE \"vanta.destination.handheld.v1\" 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "BUDGET 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
                "FORM 1\n"
                "PLATFORM \"handheld.d3d12\"\n"
                "SOURCES 4097\n"),
        "hostile source counts are rejected before allocation");

    auto duplicate_product = recipe;
    duplicate_product.products.push_back(duplicate_product.products.front());
    require(!duplicate_product.validate(),
        "duplicate product requests fail closed");
    auto conflicting_product = recipe;
    // Same kind, algorithm, settings, and representation: identical canonical
    // demand that only differs in its required flag.
    conflicting_product.products.push_back(production::RecipeProductRequest{
        production::ArtifactKind::render_mesh, "cook.static-v1",
        digest("settings-cook.static-v1"), "lod0", false});
    require(!conflicting_product.validate(),
        "requests that collide under canonical ordering fail closed");
    auto unsorted_sources = recipe;
    std::swap(unsorted_sources.sources.front(), unsorted_sources.sources.back());
    require(!unsorted_sources.validate(),
        "unsorted sources fail closed");
    auto zero_budget = recipe;
    zero_budget.budget_policy_digest = {};
    require(!zero_budget.validate(),
        "a recipe must bind a non-zero budget policy digest");
    auto bad_scope = recipe;
    bad_scope.scope.asset_ids = {11U, 12U};
    require(!bad_scope.validate(),
        "a scene scope cannot carry a mutable selection");
    require(!production::expand_production_recipe(zero_budget, profile),
        "invalid recipes fail closed before expansion");
}

} // namespace

int main() {
    recipe_round_trips_with_stable_digest();
    expansion_is_deterministic_and_complete();
    unsupported_capabilities_refuse_or_skip();
    hostile_recipes_fail_closed();
    std::cout << "production recipe tests passed\n";
    return EXIT_SUCCESS;
}
