#include <carto/application/application.hpp>
#include <carto/assets/texture_image_decoder.hpp>
#include <carto/production/bake_graph.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace carto::application {
namespace {
template<class T> T need(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
void need(core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}
bool has_text(std::string_view text) {
    return text.size() <= 1024U && text.find_first_not_of(" \t\r\n") != std::string_view::npos &&
        text.find('\0') == std::string_view::npos;
}
// All existing viewport/image decoders address row zero at v=0. Preserve the
// bake's UV-row order explicitly, rather than applying a file-origin flip.
std::vector<std::uint8_t> tga(std::uint32_t width, std::uint32_t height,
                            std::span<const std::uint8_t> rgba) {
    if (!width || !height || width > 4096 || height > 4096 ||
        rgba.size() != std::size_t(width) * height * 4U)
        throw std::runtime_error("generated map exceeds the viewport's 4096 extent or pixel bounds");
    std::vector<std::uint8_t> out(18U);
    out[2] = 2; out[12] = static_cast<std::uint8_t>(width);
    out[13] = static_cast<std::uint8_t>(width >> 8U);
    out[14] = static_cast<std::uint8_t>(height);
    out[15] = static_cast<std::uint8_t>(height >> 8U);
    out[16] = 32; out[17] = 0x28;
    out.reserve(18U + rgba.size());
    for (std::size_t i = 0; i < rgba.size(); i += 4U)
        out.insert(out.end(), {rgba[i+2], rgba[i+1], rgba[i], rgba[i+3]});
    return out;
}
assets::TextureAssetId register_map(project::MaterialCatalog& catalog,
    assets::BlobStore& store, const std::filesystem::path& root,
    const std::vector<std::uint8_t>& bytes, std::string name, std::string provenance,
    std::uint32_t width, std::uint32_t height) {
    const auto digest = assets::sha256(bytes);
    // Reuse identical pixels only if the lineage is identical as well.
    for (const auto& [id, record] : catalog.textures())
        if (record.content_digest == digest && record.provenance == provenance) {
            // An explicit regeneration must also restore missing derived bytes.
            // put() verifies an existing blob and refuses corrupt replacements.
            static_cast<void>(need(store.put(bytes, "image/x-tga")));
            return id;
        }
    if (catalog.textures().size() >= project::MaterialCatalog::kMaxRecordsPerKind ||
        (!catalog.textures().empty() && catalog.textures().rbegin()->first ==
            std::numeric_limits<assets::TextureAssetId>::max()))
        throw std::runtime_error("generated texture identity budget exhausted");
    const auto blob = need(store.put(bytes, "image/x-tga"));
    project::TextureSourceRecord record;
    record.id = catalog.textures().empty() ? 1U : catalog.textures().rbegin()->first + 1U;
    record.name = std::move(name);
    record.relative_locator = store.path_for(blob.digest).lexically_relative(root).generic_string();
    record.content_digest = blob.digest;
    record.format = assets::TextureSourceFormat::tga;
    record.width = width; record.height = height;
    record.provenance = std::move(provenance); record.revision = core::Revision{1U};
    const auto id = record.id;
    need(catalog.insert_texture(std::move(record)));
    return id;
}
} // namespace

core::Result<void> ApplicationSession::copy_managed_sources_for_save(
    const std::filesystem::path& destination) const {
    if (!project_path_) return core::Result<void>::success();
    try {
        const auto from = std::filesystem::weakly_canonical(std::filesystem::absolute(*project_path_).parent_path());
        const auto to = std::filesystem::weakly_canonical(std::filesystem::absolute(destination).parent_path());
        if (from == to) return core::Result<void>::success();
        assets::BlobStore source_store(from / ".carto-assets" / "source-blobs");
        assets::BlobStore target_store(to / ".carto-assets" / "source-blobs");
        std::set<assets::Sha256Digest> digests;
        for (const auto& [id, texture] : document_.material_catalog().textures()) {
            static_cast<void>(id);
            if (texture.relative_locator != source_store.path_for(texture.content_digest).lexically_relative(from).generic_string())
                throw std::runtime_error("Save As requires managed texture sources; external texture locators must be imported first");
            digests.insert(texture.content_digest);
            const auto lineage = need(texture.lineage_digests());
            digests.insert(lineage.begin(), lineage.end());
        }
        for (const auto& [identity, record] : document_.source_asset_registry().records()) {
            static_cast<void>(identity);
            if (record.relative_source_path == source_store.path_for(record.source_digest).lexically_relative(from).generic_string())
                digests.insert(record.source_digest);
        }
        for (const auto& [identity, plane] : document_.presentation_catalog().reference_planes()) {
            static_cast<void>(identity); digests.insert(plane.source_digest);
        }
        // Copies are immutable. Failed publication can leave harmless cached
        // blobs, while the current project and destination document stay intact.
        for (const auto& digest : digests) {
            const auto bytes = need(source_store.read(digest));
            const auto stored = need(target_store.put(bytes, "application/octet-stream"));
            if (stored.digest != digest) throw std::runtime_error("Save As source digest changed while copying");
        }
        return core::Result<void>::success();
    } catch (const std::exception& error) {
        return core::Result<void>::failure({core::ErrorCode::io_error, error.what()});
    }
}

core::Result<DispatchReceipt> ApplicationSession::adopt_baked_material_map(
    const AdoptBakedMaterialMapAction& action) {
    if (active_source_ != OperationSource::human)
        return failure({core::ErrorCode::unsupported, "bake adoption requires human admission"});
    if (action.expected_project_generation != project_generation_ ||
        action.expected_project_revision != document_.revision())
        return failure({core::ErrorCode::stale_data, "bake adoption project snapshot is stale"});
    if (!project_path_ || !has_text(action.justification))
        return failure({core::ErrorCode::invalid_argument, "save the project and supply a bake adoption justification"});
    try {
        const auto* object = document_.scene().find(action.object);
        if (!object || !object->mesh_asset || object->locked || !object->visible)
            throw std::runtime_error("bake target must be a visible, unlocked mesh object");
        const auto& mesh = document_.meshes().at(*object->mesh_asset);
        auto catalog = document_.material_catalog();
        auto material = catalog.materials().at(action.material);
        if (material.revision != action.expected_material_revision || material.revision.exhausted())
            return failure({core::ErrorCode::stale_data, "bake adoption material snapshot is stale"});
        if (action.artifact_bytes.empty() || action.artifact_bytes.size() > 68U * 1024U * 1024U ||
            action.receipt.output_bytes != action.artifact_bytes.size() ||
            action.receipt.output_digest != assets::sha256(action.artifact_bytes))
            throw std::runtime_error("bake artifact bytes do not match the published receipt");
        need(action.receipt.validate());
        production::BakeRecipe recipe;
        std::vector<std::uint8_t> rgba;
        bool normal = false;
        production::ProductionCookKey key;
        if (action.receipt.key.algorithm == production::kBakeAmbientOcclusionAlgorithm) {
            auto artifact = need(production::AmbientOcclusionMapArtifact::deserialize(action.artifact_bytes));
            recipe = artifact.recipe.binding; rgba = std::move(artifact.rgba8);
            key = need(production::bake_ambient_occlusion_cook_key(
                artifact.recipe, action.receipt.key.platform_profile));
        } else if (action.receipt.key.algorithm == production::kBakeNormalAlgorithm) {
            auto artifact = need(production::NormalMapArtifact::deserialize(action.artifact_bytes));
            if (artifact.normal_space != production::BakeNormalSpace::tangent || artifact.projection_misses)
                throw std::runtime_error("only complete tangent-space normal bakes can be adopted");
            recipe = artifact.recipe; rgba = std::move(artifact.rgba8); normal = true;
            key = need(production::bake_production_cook_key(
                recipe, production::BakeNormalSpace::tangent, action.receipt.key.platform_profile));
        } else throw std::runtime_error("this bake output has no supported material slot; adopt AO or tangent normal");
        if (key != action.receipt.key)
            throw std::runtime_error("bake receipt key does not match the artifact recipe");
        need(production::validate_publication(action.receipt, key.source, {}));
        const auto identity = document_.source_namespace() + "/mesh/" + std::to_string(*object->mesh_asset);
        if (recipe.source.identity != identity || recipe.target.identity != identity)
            throw std::runtime_error("bake belongs to a different project or mesh; cross-project adoption refused");
        need(production::validate_bake_sources(recipe, mesh, mesh));
        const auto tiles = need(production::enumerate_bake_tiles(mesh, recipe.target_uv_set));
        if (recipe.tile_u || recipe.tile_v || tiles.size() != 1U || tiles.front().tile_u || tiles.front().tile_v)
            throw std::runtime_error("UDIM baking is preserved, but multi-tile material binding is unsupported; no tile substituted");
        for (const auto* ref : {&material.definition.base_color, &material.definition.metallic_roughness,
                &material.definition.normal, &material.definition.occlusion, &material.definition.emissive})
            if (*ref && ref->uv_set != recipe.target_uv_set)
                throw std::runtime_error("material maps must use the same UV identity as the bake");
        const auto bytes = tga(recipe.width, recipe.height, rgba);
        const auto root = std::filesystem::absolute(*project_path_).parent_path();
        assets::BlobStore store(root / ".carto-assets" / "source-blobs");
        // Preserve the complete artifact and receipt, not only a flattened image.
        const auto artifact_blob = need(store.put(action.artifact_bytes, "application/x-carto-bake"));
        production::ProductionReceiptLedger ledger;
        need(ledger.append(action.receipt));
        const auto receipt_text = ledger.serialize();
        const auto receipt_blob = need(store.put(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(receipt_text.data()), receipt_text.size()),
            "application/x-carto-production-receipts"));
        const auto reason_blob = need(store.put(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(action.justification.data()), action.justification.size()), "text/plain"));
        const std::string provenance = "cartographer.bake_adoption.v1;artifact=" + artifact_blob.digest.hex() +
            ";receipt=" + receipt_blob.digest.hex() + ";reason=" + reason_blob.digest.hex();
        const auto id = register_map(catalog, store, root, bytes,
            normal ? "Baked tangent normal" : "Baked ambient occlusion", provenance, recipe.width, recipe.height);
        auto& ref = normal ? material.definition.normal : material.definition.occlusion;
        ref = {}; ref.asset = id; ref.uv_set = recipe.target_uv_set;
        ref.interpretation = render::TextureInterpretation::linear;
        ref.normal_convention = recipe.normal_convention == production::BakeNormalConvention::opengl_y_plus
            ? render::NormalConvention::opengl_y_plus : render::NormalConvention::directx_y_minus;
        material.revision = material.revision.next();
        need(catalog.replace_material(std::move(material)));
        return commit_material_catalog(std::move(catalog), action.expected_project_revision, "Adopt Baked Material Map");
    } catch (const std::exception& error) {
        return failure({core::ErrorCode::validation_failed, error.what()});
    }
}

core::Result<DispatchReceipt> ApplicationSession::pack_material_maps(const PackMaterialMapsAction& action) {
    if (active_source_ != OperationSource::human)
        return failure({core::ErrorCode::unsupported, "map packing requires human admission"});
    if (action.expected_project_generation != project_generation_ ||
        action.expected_project_revision != document_.revision())
        return failure({core::ErrorCode::stale_data, "map packing project snapshot is stale"});
    if (!project_path_) return failure({core::ErrorCode::invalid_state, "save before packing maps"});
    try {
        auto catalog = document_.material_catalog();
        auto material = catalog.materials().at(action.material);
        if (material.revision != action.expected_material_revision || material.revision.exhausted())
            return failure({core::ErrorCode::stale_data, "map packing material snapshot is stale"});
        const auto root = std::filesystem::absolute(*project_path_).parent_path();
        assets::BlobStore store(root / ".carto-assets" / "source-blobs");
        const auto decode = [&](assets::TextureAssetId id) {
            const auto& record = catalog.textures().at(id);
            const auto bytes = need(store.read(record.content_digest));
            auto image = need(assets::decode_texture_rgba8(bytes, production::authoring_texture_limits()));
            if (image.admission.source.digest != record.content_digest || image.admission.format != record.format ||
                image.admission.width != record.width || image.admission.height != record.height)
                throw std::runtime_error("packing source metadata differs from stored bytes");
            return image;
        };
        auto roughness = decode(action.roughness), metallic = decode(action.metallic);
        if (roughness.admission.width != metallic.admission.width || roughness.admission.height != metallic.admission.height)
            throw std::runtime_error("roughness and metallic maps must have matching dimensions; no implicit resampling");
        std::vector<std::uint8_t> rgba(roughness.pixels.size());
        for (std::size_t i = 0; i < rgba.size(); i += 4U) {
            rgba[i] = 255; rgba[i+1] = roughness.pixels[i];
            rgba[i+2] = metallic.pixels[i]; rgba[i+3] = 255;
        }
        const auto width = roughness.admission.width, height = roughness.admission.height;
        const auto id = register_map(catalog, store, root, tga(width, height, rgba), "Packed roughness + metallic",
            "cartographer.pbr_pack.v1;roughness.R=" + roughness.admission.source.digest.hex() +
            ";metallic.R=" + metallic.admission.source.digest.hex(), width, height);
        // Keep the authored mapping when replacing a source, but reset channels
        // to the packed G/B convention and force linear data interpretation.
        material.definition.metallic_roughness.asset = id;
        material.definition.metallic_roughness.channels = {};
        material.definition.metallic_roughness.interpretation = render::TextureInterpretation::linear;
        material.revision = material.revision.next();
        need(catalog.replace_material(std::move(material)));
        return commit_material_catalog(std::move(catalog), action.expected_project_revision, "Pack Roughness and Metallic Maps");
    } catch (const std::exception& error) {
        return failure({core::ErrorCode::validation_failed, error.what()});
    }
}

core::Result<TexturingReview> ApplicationSession::review_texturing(const TexturingCommand& command) const {
    ApplicationSession speculative;
    speculative.document_ = document_;
    speculative.project_generation_ = project_generation_;
    speculative.project_path_ = project_path_;
    const auto result = std::visit([&](const auto& action) {
        return HumanApplicationAccess::dispatch(speculative, action);
    }, command);
    if (!result) return core::Result<TexturingReview>::failure(result.error());
    TexturingReview review;
    review.command_ = command; review.generation_ = project_generation_;
    review.owner_ = this;
    review.before_ = document_.serialize(); review.after_ = speculative.document_.serialize();
    return core::Result<TexturingReview>::success(std::move(review));
}

core::Result<DispatchReceipt> HumanApplicationAccess::accept_texturing(
    ApplicationSession& session, TexturingReview& review, std::string justification) {
    return session.accept_texturing(HumanActionAdmission{}, review, std::move(justification));
}
core::Result<DispatchReceipt> ApplicationSession::accept_texturing(
    HumanActionAdmission admission, TexturingReview& review, std::string justification) {
    if (!review.active_ || review.owner_ != this || review.generation_ != project_generation_ || review.before_ != document_.serialize())
        return failure({core::ErrorCode::stale_data, "texturing review is consumed, stale, or belongs to another project"});
    if (!has_text(justification))
        return failure({core::ErrorCode::invalid_argument, "texturing acceptance requires a justification"});
    // Revalidate with the same native command before the single journaled commit.
    const auto verified = review_texturing(review.command_);
    if (!verified) return failure(verified.error());
    if (verified.value().after_ != review.after_)
        return failure({core::ErrorCode::stale_data, "texturing review no longer reproduces its proposed result"});
    auto result = std::visit([&](const auto& action) {
        return dispatch(admission, action, OperationSource::human, justification);
    }, review.command_);
    if (result) review.active_ = false;
    return result;
}
} // namespace carto::application
