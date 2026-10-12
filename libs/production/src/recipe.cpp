#include <carto/production/recipe.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <span>
#include <type_traits>
#include <utility>

namespace carto::production {

namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PRODUCTION_RECIPE";
constexpr std::size_t kMaxScopeItems = 65'536U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= 192U &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-' || byte == ':' || byte == '/';
        });
}

bool valid_artifact_kind(ArtifactKind kind) {
    switch (kind) {
    case ArtifactKind::render_mesh:
    case ArtifactKind::material_package:
    case ArtifactKind::residency_page:
    case ArtifactKind::publication_package:
    case ArtifactKind::evidence_bundle:
    case ArtifactKind::baked_texture:
    case ArtifactKind::cloth_simulation:
    case ArtifactKind::cloth_rest_pose:
    case ArtifactKind::multires_hierarchy:
        return true;
    }
    return false;
}

bool valid_execution_form(ExecutionForm form) {
    switch (form) {
    case ExecutionForm::interactive:
    case ExecutionForm::cached:
    case ExecutionForm::offline:
    case ExecutionForm::learned_proposal:
    case ExecutionForm::temporal:
        return true;
    }
    return false;
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t maximum, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum ||
        encoded > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    value = static_cast<std::size_t>(encoded);
    return true;
}

bool read_bool(std::istream& input, bool& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > 1U) return false;
    value = encoded != 0U;
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

core::Result<assets::Sha256Digest> read_digest(std::istream& input, std::string_view field) {
    std::string encoded;
    if (!(input >> encoded)) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "production recipe is missing " + std::string(field)));
    }
    const auto parsed = assets::Sha256Digest::from_hex(encoded);
    if (!parsed || parsed.value().is_zero()) {
        return core::Result<assets::Sha256Digest>::failure(validation(
            "production recipe contains an invalid " + std::string(field)));
    }
    return parsed;
}

bool read_id_list(std::istream& input, std::size_t count, std::vector<std::uint64_t>& ids) {
    ids.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint64_t id = 0U;
        if (!read_unsigned(input, id)) return false;
        ids.push_back(id);
    }
    return true;
}

assets::Sha256Digest digest_text(std::string_view value) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

} // namespace

core::Result<void> RecipeProductRequest::validate() const {
    if (!valid_artifact_kind(kind) || !safe_text(algorithm, 128U) ||
        settings_digest.is_zero() ||
        (!representation_kind.empty() && !safe_text(representation_kind, 128U))) {
        return core::Result<void>::failure(invalid(
            "recipe product request kind, algorithm, settings, or representation is invalid"));
    }
    return core::Result<void>::success();
}

std::string RecipeProductRequest::canonical() const {
    std::ostringstream output;
    output << "kind=" << artifact_kind_name(kind) << ";algorithm=" << std::quoted(algorithm)
           << ";settings=" << settings_digest.hex()
           << ";representation=" << std::quoted(representation_kind)
           << ";required=" << (required ? 1U : 0U);
    return output.str();
}

core::Result<void> ProductionRecipe::validate() const {
    if (!safe_identity(identity) || !safe_identity(source_namespace) ||
        source_revision.exhausted()) {
        return core::Result<void>::failure(invalid(
            "production recipe identity, source namespace, or revision is invalid"));
    }
    if (auto result = scope.validate(); !result) return result;
    if (sources.size() > kMaxSources || products.size() > kMaxProducts) {
        return core::Result<void>::failure(validation(
            "production recipe exceeds a bounded collection size"));
    }
    if (!safe_identity(destination_profile_identity) ||
        destination_profile_digest.is_zero() || budget_policy_digest.is_zero() ||
        !valid_execution_form(form) || !safe_text(platform_profile, 128U)) {
        return core::Result<void>::failure(invalid(
            "production recipe destination, budget, form, or platform binding is invalid"));
    }
    for (const auto& source : sources) {
        if (auto result = source.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < sources.size(); ++index) {
        if (!(sources[index - 1U].identity < sources[index].identity)) {
            return core::Result<void>::failure(validation(
                "production recipe sources must be canonically sorted and unique"));
        }
    }
    for (const auto& product : products) {
        if (auto result = product.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < products.size(); ++index) {
        if (!(products[index - 1U].canonical() < products[index].canonical())) {
            return core::Result<void>::failure(validation(
                "production recipe product requests must be canonically sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

std::string ProductionRecipe::serialize() const {
    std::ostringstream output;
    output << kMagic << ' ' << kSchemaVersion << '\n';
    output << "IDENTITY " << std::quoted(identity) << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << source_revision.value() << '\n';
    output << "SCOPE " << static_cast<unsigned>(scope.kind) << ' '
           << scope.expected_source_revision.value() << ' '
           << scope.asset_ids.size();
    for (const auto id : scope.asset_ids) output << ' ' << id;
    output << ' ' << scope.node_ids.size();
    for (const auto id : scope.node_ids) output << ' ' << id;
    output << '\n';
    output << "PROFILE " << std::quoted(destination_profile_identity) << ' '
           << destination_profile_digest.hex() << '\n';
    output << "BUDGET " << budget_policy_digest.hex() << '\n';
    output << "FORM " << static_cast<unsigned>(form) << '\n';
    output << "PLATFORM " << std::quoted(platform_profile) << '\n';
    output << "SOURCES " << sources.size() << '\n';
    for (const auto& source : sources) {
        output << "ORIGIN " << std::quoted(source.identity) << ' '
               << source.revision.value() << ' ' << source.content_digest.hex() << '\n';
    }
    output << "PRODUCTS " << products.size() << '\n';
    for (const auto& product : products) {
        output << "PRODUCT " << static_cast<unsigned>(product.kind) << ' '
               << (product.required ? 1U : 0U) << ' ' << std::quoted(product.algorithm)
               << ' ' << product.settings_digest.hex() << ' '
               << std::quoted(product.representation_kind) << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<ProductionRecipe> ProductionRecipe::deserialize(std::string_view text) {
    if (text.size() > kMaxSerializedBytes) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kMagic) || !read_unsigned(input, version)) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe header is invalid"));
    }
    if (version != kSchemaVersion) {
        return core::Result<ProductionRecipe>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "production recipe schema version is unsupported"));
    }

    ProductionRecipe recipe;
    std::uint64_t revision = 0U;
    if (!require_token(input, "IDENTITY") || !(input >> std::quoted(recipe.identity))) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe identity is invalid"));
    }
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(recipe.source_namespace)) ||
        !read_unsigned(input, revision)) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe source record is invalid"));
    }
    recipe.source_revision = core::Revision{revision};

    unsigned scope_kind = 0U;
    std::uint64_t scope_revision = 0U;
    std::size_t asset_count = 0U;
    std::size_t node_count = 0U;
    if (!require_token(input, "SCOPE") ||
        !read_unsigned(input, scope_kind) ||
        scope_kind > static_cast<unsigned>(PreparationScopeKind::scene) ||
        !read_unsigned(input, scope_revision) ||
        !read_count(input, kMaxScopeItems, asset_count) ||
        !read_id_list(input, asset_count, recipe.scope.asset_ids) ||
        !read_count(input, kMaxScopeItems, node_count) ||
        !read_id_list(input, node_count, recipe.scope.node_ids)) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe scope record is invalid"));
    }
    recipe.scope.kind = static_cast<PreparationScopeKind>(scope_kind);
    recipe.scope.expected_source_revision = core::Revision{scope_revision};

    if (!require_token(input, "PROFILE") ||
        !(input >> std::quoted(recipe.destination_profile_identity))) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe destination identity is invalid"));
    }
    auto digest = read_digest(input, "destination profile digest");
    if (!digest) return core::Result<ProductionRecipe>::failure(digest.error());
    recipe.destination_profile_digest = digest.value();
    if (!require_token(input, "BUDGET")) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe budget record is invalid"));
    }
    digest = read_digest(input, "budget policy digest");
    if (!digest) return core::Result<ProductionRecipe>::failure(digest.error());
    recipe.budget_policy_digest = digest.value();

    unsigned form = 0U;
    if (!require_token(input, "FORM") ||
        !read_unsigned(input, form) ||
        form > static_cast<unsigned>(ExecutionForm::temporal)) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe execution form is invalid"));
    }
    recipe.form = static_cast<ExecutionForm>(form);
    if (!require_token(input, "PLATFORM") ||
        !(input >> std::quoted(recipe.platform_profile))) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe platform is invalid"));
    }

    std::size_t count = 0U;
    if (!require_token(input, "SOURCES") || !read_count(input, kMaxSources, count)) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe source count is invalid"));
    }
    recipe.sources.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        AuthoritativeSource source;
        std::uint64_t source_revision = 0U;
        if (!require_token(input, "ORIGIN") ||
            !(input >> std::quoted(source.identity)) ||
            !read_unsigned(input, source_revision)) {
            return core::Result<ProductionRecipe>::failure(validation(
                "production recipe source record is invalid"));
        }
        const auto source_digest = read_digest(input, "source digest");
        if (!source_digest) return core::Result<ProductionRecipe>::failure(source_digest.error());
        source.revision = core::Revision{source_revision};
        source.content_digest = source_digest.value();
        recipe.sources.push_back(std::move(source));
    }

    if (!require_token(input, "PRODUCTS") || !read_count(input, kMaxProducts, count)) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe product count is invalid"));
    }
    recipe.products.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        RecipeProductRequest product;
        unsigned kind = 0U;
        if (!require_token(input, "PRODUCT") ||
            !read_unsigned(input, kind) ||
            kind > static_cast<unsigned>(ArtifactKind::multires_hierarchy) ||
            !read_bool(input, product.required) ||
            !(input >> std::quoted(product.algorithm))) {
            return core::Result<ProductionRecipe>::failure(validation(
                "production recipe product record is invalid"));
        }
        const auto settings = read_digest(input, "product settings digest");
        if (!settings) return core::Result<ProductionRecipe>::failure(settings.error());
        if (!(input >> std::quoted(product.representation_kind))) {
            return core::Result<ProductionRecipe>::failure(validation(
                "production recipe product representation is invalid"));
        }
        product.kind = static_cast<ArtifactKind>(kind);
        product.settings_digest = settings.value();
        recipe.products.push_back(std::move(product));
    }
    if (!require_token(input, "END")) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<ProductionRecipe>::failure(validation(
            "production recipe contains trailing data"));
    }
    if (auto result = recipe.validate(); !result) {
        return core::Result<ProductionRecipe>::failure(result.error());
    }
    return core::Result<ProductionRecipe>::success(std::move(recipe));
}

core::Result<assets::Sha256Digest> ProductionRecipe::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_text(serialize()));
}

std::string artifact_capability_feature(ArtifactKind kind) {
    return std::string{"destination.artifact."} + artifact_kind_name(kind);
}

core::Result<std::vector<ProductionCookKey>> expand_production_recipe(
    const ProductionRecipe& recipe,
    const DestinationCapabilityProfile& destination) {
    if (auto result = recipe.validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    if (auto result = destination.validate(); !result) {
        return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
    }
    const auto profile_digest = destination.canonical_digest();
    if (!profile_digest || profile_digest.value() != recipe.destination_profile_digest) {
        return core::Result<std::vector<ProductionCookKey>>::failure(stale(
            "destination capability profile digest does not match the recipe binding"));
    }

    // One capability question per distinct artifact kind; a kind is required
    // when any of its product requests is required.
    std::vector<RequestedCapability> requested;
    for (const auto& product : recipe.products) {
        const auto feature = artifact_capability_feature(product.kind);
        const auto found = std::find_if(requested.begin(), requested.end(),
            [&feature](const RequestedCapability& capability) {
                return capability.feature == feature;
            });
        if (found == requested.end()) {
            requested.push_back(RequestedCapability{feature, product.required});
        } else if (product.required) {
            found->required = true;
        }
    }
    const auto checked = check_destination_capabilities(destination, requested);
    if (!checked) {
        return core::Result<std::vector<ProductionCookKey>>::failure(checked.error());
    }
    std::set<std::string> refused;
    std::set<std::string> admitted_kinds;
    for (std::size_t index = 0U; index < requested.size(); ++index) {
        const auto& result = checked.value()[index];
        if (result.evidence == PreparedCapabilityEvidence::unsupported) {
            if (requested[index].required) refused.insert(result.feature);
        } else {
            admitted_kinds.insert(result.feature);
        }
    }
    if (!refused.empty()) {
        auto diagnostic = unsupported(
            "destination profile does not offer required artifact capabilities");
        for (const auto& feature : refused) {
            diagnostic = diagnostic.with_context("unsupported: " + feature);
        }
        return core::Result<std::vector<ProductionCookKey>>::failure(std::move(diagnostic));
    }

    std::vector<ProductionCookKey> keys;
    keys.reserve(recipe.sources.size() * recipe.products.size());
    for (const auto& source : recipe.sources) {
        for (const auto& product : recipe.products) {
            if (!admitted_kinds.contains(artifact_capability_feature(product.kind))) {
                continue;
            }
            ProductionCookKey key;
            key.source = source;
            key.kind = product.kind;
            key.form = recipe.form;
            key.algorithm = product.algorithm;
            key.settings_digest = product.settings_digest;
            key.platform_profile = recipe.platform_profile;
            key.representation_kind = product.representation_kind;
            if (auto result = key.validate(); !result) {
                return core::Result<std::vector<ProductionCookKey>>::failure(result.error());
            }
            keys.push_back(std::move(key));
        }
    }
    std::sort(keys.begin(), keys.end(),
        [](const ProductionCookKey& left, const ProductionCookKey& right) {
            return left.canonical() < right.canonical();
        });
    for (std::size_t index = 1U; index < keys.size(); ++index) {
        if (!(keys[index - 1U].canonical() < keys[index].canonical())) {
            return core::Result<std::vector<ProductionCookKey>>::failure(validation(
                "recipe expansion produced duplicate cook keys"));
        }
    }
    return core::Result<std::vector<ProductionCookKey>>::success(std::move(keys));
}

} // namespace carto::production
