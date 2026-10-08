#include <carto/production/prepared_scene.hpp>

#include <carto/project/project.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <utility>

namespace carto::production {
namespace {

constexpr std::string_view kMagic = "CARTOGRAPHER_PREPARED_SCENE";
constexpr std::size_t kMaxIdentityBytes = 192U;
constexpr std::size_t kMaxNameBytes = 512U;
constexpr std::size_t kMaxPathBytes = 1'024U;
constexpr std::size_t kMaxDetailBytes = 2'048U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

bool safe_identity(std::string_view value) {
    return !value.empty() && value.size() <= kMaxIdentityBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
                byte == '-' || byte == ':' || byte == '/';
        });
}

bool safe_relative_path(std::string_view value) {
    if (!safe_text(value, kMaxPathBytes) || value.find("://") != std::string_view::npos ||
        value.find('\\') != std::string_view::npos || value.find(':') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& component : path) {
        if (component == "." || component == ".." || component.empty()) return false;
    }
    return true;
}

std::string case_fold(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

bool valid_handedness(PreparedHandedness value) {
    return value == PreparedHandedness::right_handed ||
        value == PreparedHandedness::left_handed;
}

bool valid_axis(PreparedAxis value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(PreparedAxis::negative_z);
}

unsigned axis_dimension(PreparedAxis value) {
    return static_cast<unsigned>(value) % 3U;
}

bool valid_product_kind(PreparedProductKind value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(PreparedProductKind::texture_source);
}

bool valid_dependency_disposition(PreparedDependencyDisposition value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(PreparedDependencyDisposition::unresolved);
}

bool valid_capability_evidence(PreparedCapabilityEvidence value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(PreparedCapabilityEvidence::unsupported);
}

bool valid_source_kind(PreparedSourceEntityKind value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(PreparedSourceEntityKind::material_region);
}

bool valid_prepared_kind(PreparedEntityKind value) {
    return static_cast<unsigned>(value) <=
        static_cast<unsigned>(PreparedEntityKind::material_binding);
}

template <typename T, typename Projection>
bool strictly_sorted_unique(const std::vector<T>& values, Projection projection) {
    for (std::size_t index = 1U; index < values.size(); ++index) {
        if (!(projection(values[index - 1U]) < projection(values[index]))) return false;
    }
    return true;
}

template <typename T>
bool read_unsigned(std::istream& input, T& value) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    std::string token;
    if (!(input >> token)) return false;
    T parsed = 0U;
    const auto result = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    value = parsed;
    return true;
}

bool read_count(std::istream& input, std::size_t& value) {
    std::uint64_t encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > kMaxPreparedSceneRecords ||
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

template <typename Enum>
bool read_enum(std::istream& input, unsigned maximum, Enum& value) {
    unsigned encoded = 0U;
    if (!read_unsigned(input, encoded) || encoded > maximum) return false;
    value = static_cast<Enum>(encoded);
    return true;
}

bool require_token(std::istream& input, std::string_view expected) {
    std::string token;
    return static_cast<bool>(input >> token) && token == expected;
}

core::Result<assets::Sha256Digest> parse_digest(std::string_view encoded) {
    const auto digest = assets::Sha256Digest::from_hex(encoded);
    if (!digest || digest.value().is_zero()) {
        return core::Result<assets::Sha256Digest>::failure(
            validation("prepared-scene digest is invalid"));
    }
    return digest;
}

assets::Sha256Digest digest_bytes(std::span<const std::uint8_t> bytes) {
    return assets::sha256(bytes);
}

assets::Sha256Digest digest_text(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::uint32_t infer_channel_mask(const RenderMeshArtifact& mesh) {
    std::uint32_t channels = prepared_positions | prepared_normals |
        prepared_source_correspondence;
    const auto every = [&mesh](auto member) {
        return !mesh.vertices.empty() &&
            std::all_of(mesh.vertices.begin(), mesh.vertices.end(),
                [member](const RenderMeshVertex& vertex) {
                    return (vertex.*member).has_value();
                });
    };
    if (every(&RenderMeshVertex::uv)) channels |= prepared_uv0;
    if (every(&RenderMeshVertex::uv1)) channels |= prepared_uv1;
    if (every(&RenderMeshVertex::color)) channels |= prepared_colors;
    if (every(&RenderMeshVertex::tangent)) channels |= prepared_tangents;
    if (std::any_of(mesh.submeshes.begin(), mesh.submeshes.end(),
            [](const RenderMeshSubmesh& submesh) {
                return submesh.material_slot.has_value();
            })) {
        channels |= prepared_material_regions;
    }
    return channels;
}

std::vector<std::uint32_t> infer_material_regions(const RenderMeshArtifact& mesh) {
    std::vector<std::uint32_t> regions;
    for (const auto& submesh : mesh.submeshes) {
        if (submesh.material_slot.has_value()) {
            regions.push_back(submesh.material_slot.value());
        }
    }
    std::sort(regions.begin(), regions.end());
    regions.erase(std::unique(regions.begin(), regions.end()), regions.end());
    return regions;
}

void collect_texture_binding(
    const render::TextureRef& binding,
    std::set<assets::TextureAssetId>& textures,
    std::set<render::SamplerId>& samplers) {
    if (!binding) return;
    textures.insert(binding.asset);
    if (binding.sampler != 0U) samplers.insert(binding.sampler);
}

void collect_material_dependencies(
    const render::StandardMaterial& material,
    std::set<assets::TextureAssetId>& textures,
    std::set<render::SamplerId>& samplers) {
    collect_texture_binding(material.base_color, textures, samplers);
    collect_texture_binding(material.metallic_roughness, textures, samplers);
    collect_texture_binding(material.normal, textures, samplers);
    collect_texture_binding(material.occlusion, textures, samplers);
    collect_texture_binding(material.emissive, textures, samplers);
}

std::string texture_extension(assets::TextureSourceFormat format) {
    switch (format) {
    case assets::TextureSourceFormat::png: return "png";
    case assets::TextureSourceFormat::jpeg: return "jpg";
    case assets::TextureSourceFormat::tga: return "tga";
    case assets::TextureSourceFormat::dds: return "dds";
    case assets::TextureSourceFormat::exr: return "exr";
    }
    return "bin";
}

} // namespace

core::Result<project::MaterialCatalog> compose_prepared_material_catalog(
    const project::ProjectDocument& document,
    const ResolvedPreparationScope& scope) {
    if (auto result = document.validate(); !result) {
        return core::Result<project::MaterialCatalog>::failure(
            result.error().with_context("prepared material source document"));
    }
    if (auto result = scope.validate(document); !result) {
        return core::Result<project::MaterialCatalog>::failure(
            result.error().with_context("prepared material scope"));
    }

    const auto& source = document.material_catalog();
    std::vector<project::MaterialRegionAssignment> assignments;
    std::set<render::MaterialId> material_ids;
    std::set<project::MaterialVariantId> variant_ids;
    for (const auto& [key, assignment] : source.assignments()) {
        if (!std::binary_search(
                scope.asset_ids.begin(), scope.asset_ids.end(), key.mesh_asset)) {
            continue;
        }
        assignments.push_back(assignment);
        material_ids.insert(assignment.material);
        if (assignment.variant.has_value()) variant_ids.insert(assignment.variant.value());
    }

    std::set<assets::TextureAssetId> texture_ids;
    std::set<render::SamplerId> sampler_ids;
    for (const auto material_id : material_ids) {
        const auto material = source.materials().find(material_id);
        if (material == source.materials().end()) {
            return core::Result<project::MaterialCatalog>::failure(validation(
                "prepared material assignment references a missing source material"));
        }
        collect_material_dependencies(
            material->second.definition, texture_ids, sampler_ids);
    }
    for (const auto variant_id : variant_ids) {
        const auto variant = source.variants().find(variant_id);
        if (variant == source.variants().end()) {
            return core::Result<project::MaterialCatalog>::failure(validation(
                "prepared material assignment references a missing source variant"));
        }
        const auto resolved = source.resolve_material(
            variant->second.base_material, variant_id);
        if (!resolved) {
            return core::Result<project::MaterialCatalog>::failure(resolved.error());
        }
        collect_material_dependencies(resolved.value(), texture_ids, sampler_ids);
    }

    project::MaterialCatalog prepared;
    for (const auto texture_id : texture_ids) {
        const auto texture = source.textures().find(texture_id);
        if (texture == source.textures().end() ||
            !prepared.insert_texture(texture->second)) {
            return core::Result<project::MaterialCatalog>::failure(validation(
                "prepared material closure is missing a source texture"));
        }
    }
    for (const auto sampler_id : sampler_ids) {
        const auto sampler = source.samplers().find(sampler_id);
        if (sampler == source.samplers().end() ||
            !prepared.insert_sampler(sampler->second)) {
            return core::Result<project::MaterialCatalog>::failure(validation(
                "prepared material closure is missing a sampler intent"));
        }
    }
    for (const auto material_id : material_ids) {
        if (auto result = prepared.insert_material(source.materials().at(material_id));
            !result) {
            return core::Result<project::MaterialCatalog>::failure(result.error());
        }
    }
    for (const auto variant_id : variant_ids) {
        project::MaterialVariant variant = source.variants().at(variant_id);
        if (!variant.permitted_mesh_assets.empty()) {
            std::vector<std::uint64_t> scoped;
            std::set_intersection(
                variant.permitted_mesh_assets.begin(),
                variant.permitted_mesh_assets.end(),
                scope.asset_ids.begin(), scope.asset_ids.end(),
                std::back_inserter(scoped));
            variant.permitted_mesh_assets = std::move(scoped);
        }
        if (auto result = prepared.insert_variant(std::move(variant)); !result) {
            return core::Result<project::MaterialCatalog>::failure(result.error());
        }
    }
    for (auto assignment : assignments) {
        if (auto result = prepared.insert_assignment(std::move(assignment)); !result) {
            return core::Result<project::MaterialCatalog>::failure(result.error());
        }
    }
    if (auto result = prepared.validate(); !result) {
        return core::Result<project::MaterialCatalog>::failure(result.error());
    }
    return core::Result<project::MaterialCatalog>::success(std::move(prepared));
}

core::Result<void> PreparedCoordinateConvention::validate() const {
    if (!std::isfinite(meters_per_unit) || meters_per_unit <= 0.0 ||
        !valid_handedness(handedness) || !valid_axis(up) || !valid_axis(forward) ||
        axis_dimension(up) == axis_dimension(forward)) {
        return core::Result<void>::failure(invalid(
            "prepared coordinate convention is incomplete or degenerate"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedProductReference::validate() const {
    if (!safe_identity(identity) || !valid_product_kind(kind) || format_version == 0U ||
        content_digest.is_zero() || !safe_relative_path(relative_path)) {
        return core::Result<void>::failure(invalid(
            "prepared product identity, kind, version, digest, or path is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedAssetDefinition::validate() const {
    if (id == 0U || !safe_text(name, kMaxNameBytes) ||
        !safe_identity(render_mesh_product) ||
        (lookdev_product.has_value() && !safe_identity(lookdev_product.value())) ||
        channel_mask == 0U || (channel_mask & ~kKnownPreparedMeshChannels) != 0U ||
        (channel_mask & prepared_positions) == 0U ||
        (channel_mask & prepared_normals) == 0U ||
        (channel_mask & prepared_source_correspondence) == 0U ||
        material_regions.size() > kMaxPreparedSceneRecords ||
        !std::is_sorted(material_regions.begin(), material_regions.end()) ||
        std::adjacent_find(material_regions.begin(), material_regions.end()) !=
            material_regions.end() ||
        std::any_of(material_regions.begin(), material_regions.end(),
            [](std::uint32_t region) { return region == 0U; }) ||
        (((channel_mask & prepared_material_regions) != 0U) !=
            !material_regions.empty())) {
        return core::Result<void>::failure(invalid(
            "prepared asset identity, products, channels, or material regions are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedSceneNode::validate() const {
    constexpr double kMinimumScale = 1e-12;
    if (id == 0U || !safe_text(name, kMaxNameBytes) ||
        (parent.has_value() && (parent.value() == 0U || parent.value() == id)) ||
        (asset.has_value() && asset.value() == 0U) || !local_transform.finite() ||
        std::abs(local_transform.scale.x) <= kMinimumScale ||
        std::abs(local_transform.scale.y) <= kMinimumScale ||
        std::abs(local_transform.scale.z) <= kMinimumScale) {
        return core::Result<void>::failure(invalid(
            "prepared scene node identity, relation, or transform is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedMaterialBinding::validate() const {
    if (asset == 0U || region == 0U || material == 0U ||
        (variant.has_value() && variant.value() == 0U)) {
        return core::Result<void>::failure(invalid(
            "prepared material binding identities must be non-zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedDependencyReference::validate() const {
    if (!safe_identity(identity) || !valid_dependency_disposition(disposition) ||
        revision.exhausted() ||
        (content_digest.has_value() && content_digest->is_zero()) ||
        (!relative_path.empty() && !safe_relative_path(relative_path)) ||
        ((disposition == PreparedDependencyDisposition::source_input ||
          disposition == PreparedDependencyDisposition::included_product) &&
            (!content_digest.has_value() || relative_path.empty())) ||
        (required && disposition == PreparedDependencyDisposition::unresolved)) {
        return core::Result<void>::failure(invalid(
            "prepared dependency identity, disposition, digest, path, or requirement is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedCapabilityResult::validate() const {
    if (!safe_identity(feature) || !valid_capability_evidence(evidence) ||
        !safe_text(detail, kMaxDetailBytes) ||
        (required && evidence == PreparedCapabilityEvidence::unsupported)) {
        return core::Result<void>::failure(invalid(
            "prepared capability identity, evidence, detail, or requirement is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedSourceCorrespondence::validate() const {
    const bool region_source = source_kind == PreparedSourceEntityKind::material_region;
    const bool binding_target = prepared_kind == PreparedEntityKind::material_binding;
    if (!valid_source_kind(source_kind) || !valid_prepared_kind(prepared_kind) ||
        source_primary == 0U || prepared_primary == 0U ||
        (region_source != (source_secondary != 0U)) ||
        (binding_target != (prepared_secondary != 0U))) {
        return core::Result<void>::failure(invalid(
            "prepared source correspondence is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> PreparedSceneEnvelope::validate() const {
    if (!safe_identity(source_namespace) || source_revision.exhausted() ||
        source_digest.is_zero() || !safe_identity(profile_identity) ||
        profile_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "prepared-scene source or profile identity is invalid"));
    }
    if (auto result = coordinates.validate(); !result) return result;
    if (products.size() > kMaxPreparedSceneRecords ||
        assets.size() > kMaxPreparedSceneRecords ||
        nodes.size() > kMaxPreparedSceneRecords ||
        material_bindings.size() > kMaxPreparedSceneRecords ||
        dependencies.size() > kMaxPreparedSceneRecords ||
        capabilities.size() > kMaxPreparedSceneRecords ||
        correspondences.size() > kMaxPreparedSceneRecords) {
        return core::Result<void>::failure(validation(
            "prepared scene exceeds a bounded collection size"));
    }
    if (!strictly_sorted_unique(products,
            [](const auto& value) { return value.identity; }) ||
        !strictly_sorted_unique(assets,
            [](const auto& value) { return value.id; }) ||
        !strictly_sorted_unique(nodes,
            [](const auto& value) { return value.id; }) ||
        !strictly_sorted_unique(material_bindings, [](const auto& value) {
            return std::tuple{value.asset, value.region};
        }) ||
        !strictly_sorted_unique(dependencies,
            [](const auto& value) { return value.identity; }) ||
        !strictly_sorted_unique(capabilities,
            [](const auto& value) { return value.feature; }) ||
        !strictly_sorted_unique(correspondences, [](const auto& value) {
            return std::tuple{
                static_cast<unsigned>(value.source_kind), value.source_primary,
                value.source_secondary, static_cast<unsigned>(value.prepared_kind),
                value.prepared_primary, value.prepared_secondary};
        })) {
        return core::Result<void>::failure(validation(
            "prepared-scene records must be canonically sorted and unique"));
    }

    std::map<std::string, const PreparedProductReference*> product_map;
    std::set<std::string> portable_paths;
    std::size_t material_catalog_products = 0U;
    for (const auto& product : products) {
        if (auto result = product.validate(); !result) return result;
        product_map.emplace(product.identity, &product);
        if (product.kind == PreparedProductKind::material_catalog) {
            ++material_catalog_products;
        }
        if (!portable_paths.insert(case_fold(product.relative_path)).second) {
            return core::Result<void>::failure(validation(
                "prepared products collide under case-insensitive paths"));
        }
    }

    std::map<std::uint64_t, const PreparedAssetDefinition*> asset_map;
    for (const auto& asset : assets) {
        if (auto result = asset.validate(); !result) return result;
        const auto render_product = product_map.find(asset.render_mesh_product);
        if (render_product == product_map.end() ||
            render_product->second->kind != PreparedProductKind::render_mesh) {
            return core::Result<void>::failure(validation(
                "prepared asset render product is missing or has the wrong kind"));
        }
        if (asset.lookdev_product.has_value()) {
            const auto lookdev = product_map.find(asset.lookdev_product.value());
            if (lookdev == product_map.end() ||
                lookdev->second->kind != PreparedProductKind::lookdev_draw_package) {
                return core::Result<void>::failure(validation(
                    "prepared asset lookdev product is missing or has the wrong kind"));
            }
        }
        asset_map.emplace(asset.id, &asset);
    }
    if (material_catalog_products > 1U ||
        (!material_bindings.empty() && material_catalog_products != 1U)) {
        return core::Result<void>::failure(validation(
            "prepared material bindings require exactly one scoped material catalog"));
    }

    std::map<std::uint64_t, const PreparedSceneNode*> node_map;
    for (const auto& node : nodes) {
        if (auto result = node.validate(); !result) return result;
        node_map.emplace(node.id, &node);
    }
    for (const auto& node : nodes) {
        if (node.parent.has_value() && !node_map.contains(node.parent.value())) {
            return core::Result<void>::failure(validation(
                "prepared scene node parent does not resolve"));
        }
        if (node.asset.has_value() && !asset_map.contains(node.asset.value())) {
            return core::Result<void>::failure(validation(
                "prepared scene node asset does not resolve"));
        }
        std::set<std::uint64_t> ancestry;
        const PreparedSceneNode* cursor = &node;
        while (cursor->parent.has_value()) {
            if (!ancestry.insert(cursor->id).second) {
                return core::Result<void>::failure(validation(
                    "prepared scene hierarchy contains a cycle"));
            }
            cursor = node_map.at(cursor->parent.value());
        }
    }

    std::set<std::pair<std::uint64_t, std::uint32_t>> binding_keys;
    for (const auto& binding : material_bindings) {
        if (auto result = binding.validate(); !result) return result;
        const auto asset = asset_map.find(binding.asset);
        if (asset == asset_map.end() ||
            !std::binary_search(asset->second->material_regions.begin(),
                asset->second->material_regions.end(), binding.region)) {
            return core::Result<void>::failure(validation(
                "prepared material binding does not resolve to an asset region"));
        }
        binding_keys.emplace(binding.asset, binding.region);
    }
    for (const auto& asset : assets) {
        for (const auto region : asset.material_regions) {
            if (!binding_keys.contains({asset.id, region})) {
                return core::Result<void>::failure(validation(
                    "prepared material region has no source binding"));
            }
        }
    }

    for (const auto& dependency : dependencies) {
        if (auto result = dependency.validate(); !result) return result;
        if (dependency.disposition == PreparedDependencyDisposition::included_product) {
            const auto product = product_map.find(dependency.identity);
            if (product == product_map.end() ||
                !dependency.content_digest.has_value() ||
                product->second->content_digest != dependency.content_digest.value() ||
                product->second->relative_path != dependency.relative_path) {
                return core::Result<void>::failure(validation(
                    "included dependency does not match its prepared product"));
            }
        }
    }
    for (const auto& product : products) {
        const auto dependency = std::lower_bound(
            dependencies.begin(), dependencies.end(), product.identity,
            [](const PreparedDependencyReference& candidate, std::string_view identity) {
                return candidate.identity < identity;
            });
        if (dependency == dependencies.end() || dependency->identity != product.identity ||
            dependency->disposition != PreparedDependencyDisposition::included_product) {
            return core::Result<void>::failure(validation(
                "prepared product is absent from dependency closure"));
        }
    }

    for (const auto& capability : capabilities) {
        if (auto result = capability.validate(); !result) return result;
    }

    std::set<std::uint64_t> covered_assets;
    std::set<std::uint64_t> covered_nodes;
    std::set<std::pair<std::uint64_t, std::uint32_t>> covered_bindings;
    for (const auto& correspondence : correspondences) {
        if (auto result = correspondence.validate(); !result) return result;
        switch (correspondence.prepared_kind) {
        case PreparedEntityKind::asset:
            if (!asset_map.contains(correspondence.prepared_primary)) {
                return core::Result<void>::failure(validation(
                    "source correspondence references a missing prepared asset"));
            }
            covered_assets.insert(correspondence.prepared_primary);
            break;
        case PreparedEntityKind::node:
            if (!node_map.contains(correspondence.prepared_primary)) {
                return core::Result<void>::failure(validation(
                    "source correspondence references a missing prepared node"));
            }
            covered_nodes.insert(correspondence.prepared_primary);
            break;
        case PreparedEntityKind::material_binding: {
            const auto key = std::pair{
                correspondence.prepared_primary,
                static_cast<std::uint32_t>(correspondence.prepared_secondary)};
            if (correspondence.prepared_secondary >
                    std::numeric_limits<std::uint32_t>::max() ||
                !binding_keys.contains(key)) {
                return core::Result<void>::failure(validation(
                    "source correspondence references a missing material binding"));
            }
            covered_bindings.insert(key);
            break;
        }
        }
    }
    if (covered_assets.size() != assets.size() || covered_nodes.size() != nodes.size() ||
        covered_bindings.size() != material_bindings.size()) {
        return core::Result<void>::failure(validation(
            "prepared entities do not all have explicit source correspondence"));
    }
    return core::Result<void>::success();
}

std::string PreparedSceneEnvelope::serialize() const {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << kMagic << ' ' << kPreparedSceneVersion << '\n';
    output << "SOURCE " << std::quoted(source_namespace) << ' '
           << source_revision.value() << ' ' << source_digest.hex() << '\n';
    output << "PROFILE " << std::quoted(profile_identity) << ' '
           << profile_digest.hex() << '\n';
    output << "COORDINATES " << coordinates.meters_per_unit << ' '
           << static_cast<unsigned>(coordinates.handedness) << ' '
           << static_cast<unsigned>(coordinates.up) << ' '
           << static_cast<unsigned>(coordinates.forward) << '\n';
    output << "PRODUCTS " << products.size() << '\n';
    for (const auto& product : products) {
        output << "PRODUCT " << std::quoted(product.identity) << ' '
               << static_cast<unsigned>(product.kind) << ' ' << product.format_version << ' '
               << product.content_digest.hex() << ' '
               << std::quoted(product.relative_path) << '\n';
    }
    output << "ASSETS " << assets.size() << '\n';
    for (const auto& asset : assets) {
        output << "ASSET " << asset.id << ' ' << std::quoted(asset.name) << ' '
               << std::quoted(asset.render_mesh_product) << ' '
               << std::quoted(asset.lookdev_product.value_or("")) << ' '
               << asset.channel_mask << ' ' << asset.material_regions.size();
        for (const auto region : asset.material_regions) output << ' ' << region;
        output << '\n';
    }
    output << "NODES " << nodes.size() << '\n';
    for (const auto& node : nodes) {
        output << "NODE " << node.id << ' ' << node.parent.value_or(0U) << ' '
               << node.asset.value_or(0U) << ' ' << (node.visible ? 1U : 0U) << ' '
               << std::quoted(node.name) << ' '
               << node.local_transform.translation.x << ' '
               << node.local_transform.translation.y << ' '
               << node.local_transform.translation.z << ' '
               << node.local_transform.rotation.x << ' '
               << node.local_transform.rotation.y << ' '
               << node.local_transform.rotation.z << ' '
               << node.local_transform.rotation.w << ' '
               << node.local_transform.scale.x << ' '
               << node.local_transform.scale.y << ' '
               << node.local_transform.scale.z << '\n';
    }
    output << "MATERIAL_BINDINGS " << material_bindings.size() << '\n';
    for (const auto& binding : material_bindings) {
        output << "MATERIAL_BINDING " << binding.asset << ' ' << binding.region << ' '
               << binding.material << ' ' << binding.variant.value_or(0U) << '\n';
    }
    output << "DEPENDENCIES " << dependencies.size() << '\n';
    for (const auto& dependency : dependencies) {
        output << "DEPENDENCY " << std::quoted(dependency.identity) << ' '
               << static_cast<unsigned>(dependency.disposition) << ' '
               << (dependency.required ? 1U : 0U) << ' ' << dependency.revision.value() << ' '
               << (dependency.content_digest.has_value()
                       ? dependency.content_digest->hex() : std::string{"-"}) << ' '
               << std::quoted(dependency.relative_path) << '\n';
    }
    output << "CAPABILITIES " << capabilities.size() << '\n';
    for (const auto& capability : capabilities) {
        output << "CAPABILITY " << std::quoted(capability.feature) << ' '
               << static_cast<unsigned>(capability.evidence) << ' '
               << (capability.required ? 1U : 0U) << ' '
               << std::quoted(capability.detail) << '\n';
    }
    output << "CORRESPONDENCES " << correspondences.size() << '\n';
    for (const auto& correspondence : correspondences) {
        output << "CORRESPONDENCE "
               << static_cast<unsigned>(correspondence.source_kind) << ' '
               << correspondence.source_primary << ' ' << correspondence.source_secondary << ' '
               << static_cast<unsigned>(correspondence.prepared_kind) << ' '
               << correspondence.prepared_primary << ' '
               << correspondence.prepared_secondary << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> PreparedSceneEnvelope::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    const std::string encoded = serialize();
    return core::Result<assets::Sha256Digest>::success(digest_text(encoded));
}

core::Result<PreparedSceneEnvelope> PreparedSceneEnvelope::deserialize(
    std::string_view text) {
    if (text.size() > kMaxPreparedSceneBytes) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared scene exceeds its serialized size limit"));
    }
    std::istringstream input{std::string(text)};
    std::uint32_t version = 0U;
    if (!require_token(input, kMagic) || !read_unsigned(input, version)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene header is invalid"));
    }
    if (version != kPreparedSceneVersion) {
        return core::Result<PreparedSceneEnvelope>::failure(core::Diagnostic(
            core::ErrorCode::version_mismatch,
            "prepared-scene schema version is unsupported"));
    }

    PreparedSceneEnvelope scene;
    std::uint64_t revision = 0U;
    std::string encoded_digest;
    if (!require_token(input, "SOURCE") ||
        !(input >> std::quoted(scene.source_namespace)) ||
        !read_unsigned(input, revision) || !(input >> encoded_digest)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene source record is invalid"));
    }
    scene.source_revision = core::Revision{revision};
    auto digest = parse_digest(encoded_digest);
    if (!digest) return core::Result<PreparedSceneEnvelope>::failure(digest.error());
    scene.source_digest = digest.value();

    if (!require_token(input, "PROFILE") ||
        !(input >> std::quoted(scene.profile_identity) >> encoded_digest)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene profile record is invalid"));
    }
    digest = parse_digest(encoded_digest);
    if (!digest) return core::Result<PreparedSceneEnvelope>::failure(digest.error());
    scene.profile_digest = digest.value();

    if (!require_token(input, "COORDINATES") ||
        !(input >> scene.coordinates.meters_per_unit) ||
        !read_enum(input, static_cast<unsigned>(PreparedHandedness::left_handed),
            scene.coordinates.handedness) ||
        !read_enum(input, static_cast<unsigned>(PreparedAxis::negative_z),
            scene.coordinates.up) ||
        !read_enum(input, static_cast<unsigned>(PreparedAxis::negative_z),
            scene.coordinates.forward)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene coordinate record is invalid"));
    }

    std::size_t count = 0U;
    if (!require_token(input, "PRODUCTS") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene product count is invalid"));
    }
    scene.products.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedProductReference product;
        if (!require_token(input, "PRODUCT") ||
            !(input >> std::quoted(product.identity)) ||
            !read_enum(input, static_cast<unsigned>(PreparedProductKind::texture_source),
                product.kind) ||
            !read_unsigned(input, product.format_version) ||
            !(input >> encoded_digest >> std::quoted(product.relative_path))) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene product record is invalid"));
        }
        digest = parse_digest(encoded_digest);
        if (!digest) return core::Result<PreparedSceneEnvelope>::failure(digest.error());
        product.content_digest = digest.value();
        scene.products.push_back(std::move(product));
    }

    if (!require_token(input, "ASSETS") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene asset count is invalid"));
    }
    scene.assets.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedAssetDefinition asset;
        std::string lookdev;
        std::size_t region_count = 0U;
        if (!require_token(input, "ASSET") || !read_unsigned(input, asset.id) ||
            !(input >> std::quoted(asset.name) >> std::quoted(asset.render_mesh_product) >>
              std::quoted(lookdev)) || !read_unsigned(input, asset.channel_mask) ||
            !read_count(input, region_count)) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene asset record is invalid"));
        }
        if (!lookdev.empty()) asset.lookdev_product = std::move(lookdev);
        asset.material_regions.reserve(region_count);
        for (std::size_t region_index = 0U; region_index < region_count; ++region_index) {
            std::uint32_t region = 0U;
            if (!read_unsigned(input, region)) {
                return core::Result<PreparedSceneEnvelope>::failure(validation(
                    "prepared-scene material-region list is invalid"));
            }
            asset.material_regions.push_back(region);
        }
        scene.assets.push_back(std::move(asset));
    }

    if (!require_token(input, "NODES") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene node count is invalid"));
    }
    scene.nodes.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedSceneNode node;
        std::uint64_t parent = 0U;
        std::uint64_t asset = 0U;
        if (!require_token(input, "NODE") || !read_unsigned(input, node.id) ||
            !read_unsigned(input, parent) || !read_unsigned(input, asset) ||
            !read_bool(input, node.visible) || !(input >> std::quoted(node.name)) ||
            !(input >> node.local_transform.translation.x >>
              node.local_transform.translation.y >> node.local_transform.translation.z >>
              node.local_transform.rotation.x >> node.local_transform.rotation.y >>
              node.local_transform.rotation.z >> node.local_transform.rotation.w >>
              node.local_transform.scale.x >> node.local_transform.scale.y >>
              node.local_transform.scale.z)) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene node record is invalid"));
        }
        if (parent != 0U) node.parent = parent;
        if (asset != 0U) node.asset = asset;
        scene.nodes.push_back(std::move(node));
    }

    if (!require_token(input, "MATERIAL_BINDINGS") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene material-binding count is invalid"));
    }
    scene.material_bindings.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedMaterialBinding binding;
        std::uint64_t variant = 0U;
        if (!require_token(input, "MATERIAL_BINDING") ||
            !read_unsigned(input, binding.asset) || !read_unsigned(input, binding.region) ||
            !read_unsigned(input, binding.material) || !read_unsigned(input, variant)) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene material-binding record is invalid"));
        }
        if (variant != 0U) binding.variant = variant;
        scene.material_bindings.push_back(std::move(binding));
    }

    if (!require_token(input, "DEPENDENCIES") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene dependency count is invalid"));
    }
    scene.dependencies.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedDependencyReference dependency;
        std::uint64_t dependency_revision = 0U;
        if (!require_token(input, "DEPENDENCY") ||
            !(input >> std::quoted(dependency.identity)) ||
            !read_enum(input,
                static_cast<unsigned>(PreparedDependencyDisposition::unresolved),
                dependency.disposition) || !read_bool(input, dependency.required) ||
            !read_unsigned(input, dependency_revision) ||
            !(input >> encoded_digest >> std::quoted(dependency.relative_path))) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene dependency record is invalid"));
        }
        dependency.revision = core::Revision{dependency_revision};
        if (encoded_digest != "-") {
            digest = parse_digest(encoded_digest);
            if (!digest) return core::Result<PreparedSceneEnvelope>::failure(digest.error());
            dependency.content_digest = digest.value();
        }
        scene.dependencies.push_back(std::move(dependency));
    }

    if (!require_token(input, "CAPABILITIES") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene capability count is invalid"));
    }
    scene.capabilities.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedCapabilityResult capability;
        if (!require_token(input, "CAPABILITY") ||
            !(input >> std::quoted(capability.feature)) ||
            !read_enum(input,
                static_cast<unsigned>(PreparedCapabilityEvidence::unsupported),
                capability.evidence) || !read_bool(input, capability.required) ||
            !(input >> std::quoted(capability.detail))) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene capability record is invalid"));
        }
        scene.capabilities.push_back(std::move(capability));
    }

    if (!require_token(input, "CORRESPONDENCES") || !read_count(input, count)) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared-scene correspondence count is invalid"));
    }
    scene.correspondences.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        PreparedSourceCorrespondence correspondence;
        if (!require_token(input, "CORRESPONDENCE") ||
            !read_enum(input,
                static_cast<unsigned>(PreparedSourceEntityKind::material_region),
                correspondence.source_kind) ||
            !read_unsigned(input, correspondence.source_primary) ||
            !read_unsigned(input, correspondence.source_secondary) ||
            !read_enum(input,
                static_cast<unsigned>(PreparedEntityKind::material_binding),
                correspondence.prepared_kind) ||
            !read_unsigned(input, correspondence.prepared_primary) ||
            !read_unsigned(input, correspondence.prepared_secondary)) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared-scene correspondence record is invalid"));
        }
        scene.correspondences.push_back(std::move(correspondence));
    }
    if (!require_token(input, "END")) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared scene is missing its terminal record"));
    }
    std::string trailing;
    if (input >> trailing) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared scene contains trailing data"));
    }
    if (auto result = scene.validate(); !result) {
        return core::Result<PreparedSceneEnvelope>::failure(result.error());
    }
    return core::Result<PreparedSceneEnvelope>::success(std::move(scene));
}

core::Result<PreparedSceneEnvelope> compose_prepared_scene(
    std::string source_namespace,
    const project::ProjectDocument& document,
    std::string profile_identity,
    assets::Sha256Digest profile_digest,
    std::span<const PreparedAssetInput> asset_inputs) {
    const auto scope = resolve_preparation_scope(document, PreparationScopeRequest{
        PreparationScopeKind::scene, document.revision(), {}, {}});
    if (!scope) {
        return core::Result<PreparedSceneEnvelope>::failure(scope.error());
    }
    return compose_prepared_scene(
        std::move(source_namespace), document, scope.value(),
        std::move(profile_identity), profile_digest, asset_inputs);
}

core::Result<PreparedSceneEnvelope> compose_prepared_scene(
    std::string source_namespace,
    const project::ProjectDocument& document,
    const ResolvedPreparationScope& scope,
    std::string profile_identity,
    assets::Sha256Digest profile_digest,
    std::span<const PreparedAssetInput> asset_inputs) {
    if (auto result = document.validate(); !result) {
        return core::Result<PreparedSceneEnvelope>::failure(
            result.error().with_context("prepared-scene source document"));
    }
    if (auto result = scope.validate(document); !result) {
        return core::Result<PreparedSceneEnvelope>::failure(
            result.error().with_context("prepared-scene resolved scope"));
    }
    const std::string source_text = document.serialize();
    const assets::Sha256Digest source_digest = digest_text(source_text);

    std::map<std::uint64_t, const PreparedAssetInput*> inputs;
    for (const auto& input : asset_inputs) {
        if (input.mesh_asset == 0U || input.render_mesh == nullptr ||
            !inputs.emplace(input.mesh_asset, &input).second) {
            return core::Result<PreparedSceneEnvelope>::failure(invalid(
                "prepared asset input is missing, zero, or duplicated"));
        }
    }
    if (inputs.size() != scope.asset_ids.size() ||
        !std::equal(scope.asset_ids.begin(), scope.asset_ids.end(), inputs.begin(),
            [](std::uint64_t scoped, const auto& input) {
                return scoped == input.first;
            })) {
        return core::Result<PreparedSceneEnvelope>::failure(validation(
            "prepared asset inputs do not exactly match the resolved stable-ID scope"));
    }

    PreparedSceneEnvelope scene;
    scene.source_namespace = std::move(source_namespace);
    scene.source_revision = document.revision();
    scene.source_digest = source_digest;
    scene.profile_identity = std::move(profile_identity);
    scene.profile_digest = profile_digest;

    bool every_materialized_asset_has_lookdev = true;
    for (const auto& [mesh_asset, input] : inputs) {
        const auto source_mesh = document.meshes().find(mesh_asset);
        if (source_mesh == document.meshes().end()) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared asset input references a missing project mesh"));
        }
        const auto& render_mesh = *input->render_mesh;
        if (auto result = render_mesh.validate(); !result) {
            return core::Result<PreparedSceneEnvelope>::failure(
                result.error().with_context("prepared render mesh"));
        }
        if (render_mesh.source_revision != document.revision() ||
            render_mesh.source_digest != source_digest ||
            render_mesh.mesh_revision != source_mesh->second.revision()) {
            return core::Result<PreparedSceneEnvelope>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "prepared render mesh does not match the exact project snapshot"));
        }
        const auto render_bytes = render_mesh.serialize();
        if (!render_bytes) {
            return core::Result<PreparedSceneEnvelope>::failure(render_bytes.error());
        }
        const assets::Sha256Digest render_digest = digest_bytes(render_bytes.value());
        const std::string render_identity =
            "mesh." + std::to_string(mesh_asset) + ".render";
        const std::string render_path =
            "products/mesh-" + std::to_string(mesh_asset) + ".render-mesh-v" +
            std::to_string(kRenderMeshArtifactFormatVersion) + ".bin";
        scene.products.push_back(PreparedProductReference{
            render_identity, PreparedProductKind::render_mesh,
            kRenderMeshArtifactFormatVersion, render_digest, render_path});

        PreparedAssetDefinition asset;
        asset.id = mesh_asset;
        asset.name = "Mesh " + std::to_string(mesh_asset);
        asset.render_mesh_product = render_identity;
        asset.channel_mask = infer_channel_mask(render_mesh);
        asset.material_regions = infer_material_regions(render_mesh);

        if (input->lookdev != nullptr) {
            const auto& lookdev = *input->lookdev;
            if (auto result = lookdev.validate(); !result) {
                return core::Result<PreparedSceneEnvelope>::failure(
                    result.error().with_context("prepared lookdev package"));
            }
            if (lookdev.mesh_source_revision != document.revision() ||
                lookdev.mesh_revision != render_mesh.mesh_revision ||
                lookdev.mesh_source_digest != source_digest ||
                lookdev.mesh_artifact_digest != render_digest ||
                lookdev.draws.size() != render_mesh.submeshes.size()) {
                return core::Result<PreparedSceneEnvelope>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "prepared lookdev package does not match its exact render mesh"));
            }
            for (const auto& draw : lookdev.draws) {
                if (!draw.material_slot.has_value()) continue;
                const project::MaterialRegionKey key{mesh_asset, draw.material_slot.value()};
                const auto assignment = document.material_catalog().assignments().find(key);
                if (assignment == document.material_catalog().assignments().end() ||
                    assignment->second.material != draw.material_id) {
                    return core::Result<PreparedSceneEnvelope>::failure(validation(
                        "lookdev draw material does not match the durable source assignment"));
                }
            }
            const auto lookdev_bytes = serialize_lookdev_draw_package(lookdev);
            if (!lookdev_bytes) {
                return core::Result<PreparedSceneEnvelope>::failure(lookdev_bytes.error());
            }
            const std::string lookdev_identity =
                "mesh." + std::to_string(mesh_asset) + ".lookdev";
            const std::string lookdev_path =
                "products/mesh-" + std::to_string(mesh_asset) + ".lookdev-v" +
                std::to_string(kLookdevDrawPackageVersion) + ".bin";
            scene.products.push_back(PreparedProductReference{
                lookdev_identity, PreparedProductKind::lookdev_draw_package,
                kLookdevDrawPackageVersion, digest_bytes(lookdev_bytes.value()),
                lookdev_path});
            asset.lookdev_product = lookdev_identity;
        } else if (!asset.material_regions.empty()) {
            every_materialized_asset_has_lookdev = false;
        }
        scene.assets.push_back(std::move(asset));
        scene.correspondences.push_back(PreparedSourceCorrespondence{
            PreparedSourceEntityKind::mesh_asset, mesh_asset, 0U,
            PreparedEntityKind::asset, mesh_asset, 0U});
    }

    for (const auto& object : document.scene().objects_sorted()) {
        if (!std::binary_search(
                scope.node_ids.begin(), scope.node_ids.end(), object.id.value)) {
            continue;
        }
        if (object.mesh_asset.has_value() && !inputs.contains(object.mesh_asset.value())) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "prepared scene scope is missing a mesh referenced by a scene node"));
        }
        scene.nodes.push_back(PreparedSceneNode{
            object.id.value,
            object.name,
            object.parent.has_value()
                ? std::optional<std::uint64_t>{object.parent->value} : std::nullopt,
            object.mesh_asset,
            object.local_transform,
            object.visible,
        });
        scene.correspondences.push_back(PreparedSourceCorrespondence{
            PreparedSourceEntityKind::scene_node, object.id.value, 0U,
            PreparedEntityKind::node, object.id.value, 0U});
    }

    for (const auto& [key, assignment] : document.material_catalog().assignments()) {
        if (!inputs.contains(key.mesh_asset)) continue;
        const auto asset = std::find_if(scene.assets.begin(), scene.assets.end(),
            [&key](const PreparedAssetDefinition& candidate) {
                return candidate.id == key.mesh_asset;
            });
        if (asset == scene.assets.end() ||
            !std::binary_search(asset->material_regions.begin(),
                asset->material_regions.end(), key.region_id)) {
            return core::Result<PreparedSceneEnvelope>::failure(validation(
                "durable material assignment is absent from the prepared mesh regions"));
        }
        scene.material_bindings.push_back(PreparedMaterialBinding{
            key.mesh_asset, key.region_id, assignment.material, assignment.variant});
        scene.correspondences.push_back(PreparedSourceCorrespondence{
            PreparedSourceEntityKind::material_region, key.mesh_asset, key.region_id,
            PreparedEntityKind::material_binding, key.mesh_asset, key.region_id});
    }

    const auto prepared_materials = compose_prepared_material_catalog(document, scope);
    if (!prepared_materials) {
        return core::Result<PreparedSceneEnvelope>::failure(
            prepared_materials.error().with_context("prepared material catalog"));
    }
    if (!prepared_materials.value().empty()) {
        const std::string material_catalog_text = prepared_materials.value().serialize();
        scene.products.push_back(PreparedProductReference{
            "source.material-catalog",
            PreparedProductKind::material_catalog,
            project::MaterialCatalog::kSchemaVersion,
            digest_text(material_catalog_text),
            "products/material-catalog-v" +
                std::to_string(project::MaterialCatalog::kSchemaVersion) + ".txt"});
        for (const auto& [texture_id, texture] : prepared_materials.value().textures()) {
            scene.products.push_back(PreparedProductReference{
                "source.texture." + std::to_string(texture_id),
                PreparedProductKind::texture_source,
                1U,
                texture.content_digest,
                "products/textures/texture-" + std::to_string(texture_id) + "." +
                    texture_extension(texture.format)});
        }
    }

    if (scope.kind == PreparationScopeKind::scene && !document.world_model().empty()) {
        const auto world_digest = document.world_model().canonical_digest();
        if (!world_digest) {
            return core::Result<PreparedSceneEnvelope>::failure(world_digest.error());
        }
        scene.products.push_back(PreparedProductReference{
            "world.model", PreparedProductKind::world_model,
            world::WorldModel::kSchemaVersion, world_digest.value(),
            "products/world-model-v" +
                std::to_string(world::WorldModel::kSchemaVersion) + ".txt"});
    }

    for (const auto& product : scene.products) {
        scene.dependencies.push_back(PreparedDependencyReference{
            product.identity,
            PreparedDependencyDisposition::included_product,
            true,
            document.revision(),
            product.content_digest,
            product.relative_path,
        });
    }

    scene.capabilities.push_back(PreparedCapabilityResult{
        "scene.geometry",
        scene.assets.empty() ? PreparedCapabilityEvidence::unsupported
                             : PreparedCapabilityEvidence::derived_product,
        !scene.assets.empty(),
        scene.assets.empty()
            ? "The resolved scope contains no geometry assets."
            : "Validated render-mesh products are bound to the exact source snapshot."});
    scene.capabilities.push_back(PreparedCapabilityResult{
        "scene.hierarchy", PreparedCapabilityEvidence::preserved_source,
        !scene.nodes.empty(),
        "Stable nodes, parents, local transforms, and reusable asset references are preserved."});
    if (!scene.material_bindings.empty()) {
        scene.capabilities.push_back(PreparedCapabilityResult{
            "scene.material-authoring", PreparedCapabilityEvidence::preserved_source, true,
            "Stable source materials, texture intent, variants, and region assignments are preserved."});
        scene.capabilities.push_back(PreparedCapabilityResult{
            "scene.material-lookdev",
            every_materialized_asset_has_lookdev
                ? PreparedCapabilityEvidence::derived_product
                : PreparedCapabilityEvidence::unsupported,
            false,
            every_materialized_asset_has_lookdev
                ? "Every prepared material region has an exact lookdev draw product."
                : "Source material meaning is preserved, but one or more scoped assets lack a lookdev product."});
    }
    if (scope.kind == PreparationScopeKind::scene && !document.world_model().empty()) {
        scene.capabilities.push_back(PreparedCapabilityResult{
            "scene.world-semantics", PreparedCapabilityEvidence::preserved_source, false,
            "The provider-neutral world model is content-addressed without claiming native execution."});
    }

    std::sort(scene.products.begin(), scene.products.end(),
        [](const auto& left, const auto& right) { return left.identity < right.identity; });
    std::sort(scene.assets.begin(), scene.assets.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(scene.nodes.begin(), scene.nodes.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    std::sort(scene.material_bindings.begin(), scene.material_bindings.end(),
        [](const auto& left, const auto& right) {
            return std::tuple{left.asset, left.region} <
                std::tuple{right.asset, right.region};
        });
    std::sort(scene.dependencies.begin(), scene.dependencies.end(),
        [](const auto& left, const auto& right) { return left.identity < right.identity; });
    std::sort(scene.capabilities.begin(), scene.capabilities.end(),
        [](const auto& left, const auto& right) { return left.feature < right.feature; });
    std::sort(scene.correspondences.begin(), scene.correspondences.end(),
        [](const auto& left, const auto& right) {
            return std::tuple{
                static_cast<unsigned>(left.source_kind), left.source_primary,
                left.source_secondary, static_cast<unsigned>(left.prepared_kind),
                left.prepared_primary, left.prepared_secondary} <
                std::tuple{
                    static_cast<unsigned>(right.source_kind), right.source_primary,
                    right.source_secondary, static_cast<unsigned>(right.prepared_kind),
                    right.prepared_primary, right.prepared_secondary};
        });
    if (auto result = scene.validate(); !result) {
        return core::Result<PreparedSceneEnvelope>::failure(result.error());
    }
    return core::Result<PreparedSceneEnvelope>::success(std::move(scene));
}

} // namespace carto::production
