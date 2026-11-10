#include <carto/application/application.hpp>
#include <carto/io/gltf_scene_import.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <tuple>

namespace carto::application {

core::Result<core::Revision> ApplicationSession::source_asset_generation(
    std::string_view identity, std::string_view expected_source_namespace,
    std::uint64_t expected_project_generation,
    core::Revision expected_project_revision) const {
    if (identity.empty() || identity.size() > assets::SourceImportRecord::kMaxIdentityBytes) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument, "source generation lookup requires a bounded identity"));
    }
    if (project_generation_ != expected_project_generation ||
        document_.revision() != expected_project_revision ||
        document_.source_namespace() != expected_source_namespace) {
        return core::Result<core::Revision>::failure(core::Diagnostic(
            core::ErrorCode::stale_data, "source generation lookup belongs to another project context"));
    }
    const auto& lineage = document_.source_asset_registry().lineage();
    const auto found = lineage.find(std::string(identity));
    return core::Result<core::Revision>::success(
        found == lineage.end() ? core::Revision{} : found->second);
}

core::Result<DispatchReceipt> ApplicationSession::import_gltf_scene(
    const ImportGltfSceneAction& action) {
    const auto refuse = [this](core::ErrorCode code, std::string message) {
        return failure(core::Diagnostic(code, std::move(message)));
    };
    if (active_source_ != OperationSource::human || !action.rights_confirmed ||
        action.justification.empty() || action.justification.size() > 1024U ||
        std::any_of(action.justification.begin(), action.justification.end(), [](char c) {
            return std::iscntrl(static_cast<unsigned char>(c)) != 0;
        }) || std::all_of(action.justification.begin(), action.justification.end(), [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) != 0;
        })) {
        return refuse(core::ErrorCode::unsupported,
            "glTF scene import requires human rights confirmation and justification");
    }
    if (!project_path_ || !journal_.has_value() || project_generation_ != action.expected_project_generation ||
        document_.revision() != action.expected_project_revision ||
        action.expected_source_namespace != document_.source_namespace() || action.source_digest.is_zero()) {
        return refuse(core::ErrorCode::stale_data,
            "glTF import requires a saved journaled project and current namespace, generation, revision and source digest");
    }
    const auto& registry = document_.source_asset_registry();
    const auto lineage = registry.lineage().find(action.source_identity);
    const core::Revision retained = lineage == registry.lineage().end()
        ? core::Revision{} : lineage->second;
    if (registry.records().contains(action.source_identity) || retained != action.expected_source_generation ||
        retained.exhausted()) {
        return refuse(core::ErrorCode::stale_data,
            "glTF source identity is active, stale or has exhausted generation");
    }
    std::error_code error;
    const auto directory = project_path_->parent_path().empty()
        ? std::filesystem::current_path(error) : project_path_->parent_path();
    if (error) return refuse(core::ErrorCode::io_error, "cannot resolve project directory");
    const auto root = std::filesystem::weakly_canonical(directory, error);
    if (error || root.empty()) return refuse(core::ErrorCode::io_error, "cannot resolve project directory");
    assets::BlobStore store(root / ".carto-assets" / "source-blobs",
                            assets::BlobStoreLimits{128ULL * 1024ULL * 1024ULL, 256U});
    auto bytes = store.read(action.source_digest);
    if (!bytes) return failure(bytes.error().with_context("glTF source blob"));
    auto imported = io::import_gltf_scene(bytes.value(),
        {.allow_default_material_projection = action.allow_default_material_projection});
    if (!imported) return failure(imported.error().with_context("glTF scene import"));
    if (imported.value().materials.artifact_digest != action.source_digest) {
        return refuse(core::ErrorCode::stale_data, "decoded glTF digest differs from staged source");
    }

    project::ProjectDocument candidate(document_);
    std::vector<std::pair<std::uint64_t, geometry::EditableMesh>> meshes;
    std::vector<scene::SceneObject> nodes;
    std::map<std::uint64_t, std::uint64_t> mesh_ids, node_ids, material_ids, texture_ids, sampler_ids;
    std::vector<assets::SourceDocumentProduct> products;
    using Product = assets::SourceDocumentProductKind;
    for (std::size_t i = 0; i < imported.value().meshes.size(); ++i) {
        auto added = candidate.add_mesh(imported.value().meshes[i]);
        if (!added) return failure(added.error().with_context("glTF mesh allocation"));
        const std::uint64_t source_id = i + 1U;
        mesh_ids.emplace(source_id, added.value());
        meshes.emplace_back(added.value(), imported.value().meshes[i]);
        products.push_back({Product::mesh, source_id, added.value()});
    }
    for (const auto& source : imported.value().nodes) {
        auto id = candidate.create_object(source.name, source.local_transform);
        if (!id) return failure(id.error().with_context("glTF node allocation"));
        node_ids.emplace(source.id.value, id.value().value);
        if (auto removed = candidate.remove_object(id.value()); !removed)
            return failure(removed.error().with_context("glTF node allocation"));
        products.push_back({Product::node, source.id.value, id.value().value});
    }
    for (const auto& source : imported.value().nodes) {
        auto node = source;
        node.id = scene::ObjectId{node_ids.at(source.id.value)};
        if (source.parent) node.parent = scene::ObjectId{node_ids.at(source.parent->value)};
        if (source.mesh_asset) node.mesh_asset = mesh_ids.at(*source.mesh_asset);
        nodes.push_back(node);
        node.parent.reset();
        if (auto inserted = candidate.insert_object(std::move(node)); !inserted)
            return failure(inserted.error().with_context("glTF node staging"));
    }
    for (const auto& node : nodes) {
        if (!node.parent) continue;
        if (candidate.revision().exhausted()) return refuse(core::ErrorCode::invalid_state, "project revision exhausted");
        if (auto parented = candidate.scene_.set_parent(node.id, node.parent); !parented)
            return failure(parented.error().with_context("glTF hierarchy staging"));
        candidate.bump_revision();
    }

    auto catalog = document_.material_catalog();
    const auto& source_catalog = imported.value().materials.catalog;
    const auto allocate = [](const auto& records, std::uint64_t count) -> std::optional<std::uint64_t> {
        const std::uint64_t last = records.empty() ? 0U : records.rbegin()->first;
        if (count == 0U) return 1U;
        if (count > std::numeric_limits<std::uint64_t>::max() - last) return std::nullopt;
        return last + 1U;
    };
    const auto texture_start = allocate(catalog.textures(), source_catalog.textures().size());
    const auto sampler_start = allocate(catalog.samplers(), source_catalog.samplers().size());
    const auto material_start = allocate(catalog.materials(), source_catalog.materials().size());
    if (!texture_start || !sampler_start || !material_start)
        return refuse(core::ErrorCode::invalid_state, "glTF material identity space exhausted");
    std::uint64_t next = *texture_start;
    for (const auto& [old_id, source] : source_catalog.textures()) {
        if (old_id == 0U || old_id > imported.value().materials.encoded_images.size())
            return refuse(core::ErrorCode::validation_failed, "glTF image identity is invalid");
        const auto& image = imported.value().materials.encoded_images[old_id - 1U];
        if (assets::sha256(image) != source.content_digest)
            return refuse(core::ErrorCode::validation_failed, "glTF image digest mismatch");
        auto staged = store.read(source.content_digest);
        if (!staged) return failure(staged.error().with_context("staged glTF image"));
        if (staged.value() != image) return refuse(core::ErrorCode::validation_failed, "staged glTF image differs");
        auto record = source;
        record.id = next++;
        record.relative_locator = store.path_for(record.content_digest).lexically_relative(root).generic_string();
        texture_ids.emplace(old_id, record.id);
        products.push_back({Product::texture, old_id, record.id});
        if (auto added = catalog.insert_texture(std::move(record)); !added)
            return failure(added.error().with_context("glTF image staging"));
    }
    next = *sampler_start;
    for (const auto& [old_id, source] : source_catalog.samplers()) {
        auto record = source;
        record.id = next++;
        sampler_ids.emplace(old_id, record.id);
        products.push_back({Product::sampler, old_id, record.id});
        if (auto added = catalog.insert_sampler(std::move(record)); !added)
            return failure(added.error().with_context("glTF sampler staging"));
    }
    next = *material_start;
    for (const auto& [old_id, source] : source_catalog.materials()) {
        auto record = source;
        record.id = next++;
        for (auto* ref : {&record.definition.base_color, &record.definition.metallic_roughness,
                          &record.definition.normal, &record.definition.occlusion, &record.definition.emissive}) {
            if (ref->asset) {
                const auto image = texture_ids.find(ref->asset);
                if (image == texture_ids.end()) return refuse(core::ErrorCode::validation_failed, "glTF image binding is missing");
                ref->asset = image->second;
            }
            if (ref->sampler) {
                const auto sampler = sampler_ids.find(ref->sampler);
                if (sampler == sampler_ids.end()) return refuse(core::ErrorCode::validation_failed, "glTF sampler binding is missing");
                ref->sampler = sampler->second;
            }
        }
        material_ids.emplace(old_id, record.id);
        if (imported.value().projected_default_material != old_id)
            products.push_back({Product::material, old_id, record.id});
        if (auto added = catalog.insert_material(std::move(record)); !added)
            return failure(added.error().with_context("glTF material staging"));
    }
    for (const auto& [key, source] : source_catalog.assignments()) {
        const auto mesh = mesh_ids.find(key.mesh_asset);
        const auto material = material_ids.find(source.material);
        if (mesh == mesh_ids.end() || material == material_ids.end() || source.variant)
            return refuse(core::ErrorCode::validation_failed, "glTF material assignment is invalid");
        auto assignment = source;
        assignment.key.mesh_asset = mesh->second;
        assignment.material = material->second;
        if (auto added = catalog.insert_assignment(std::move(assignment)); !added)
            return failure(added.error().with_context("glTF material assignment staging"));
    }
    if (auto set = candidate.set_material_catalog(catalog); !set)
        return failure(set.error().with_context("glTF catalog staging"));
    std::sort(products.begin(), products.end(), [](const auto& a, const auto& b) {
        return std::tie(a.kind, a.source_index) < std::tie(b.kind, b.source_index);
    });
    assets::SourceImportRecord source;
    source.identity = action.source_identity;
    source.relative_source_path = store.path_for(action.source_digest).lexically_relative(root).generic_string();
    source.source_generation = retained.next();
    source.kind = assets::SourceAssetKind::other;
    source.source_digest = action.source_digest;
    source.importer_identity = "carto.import.gltf-scene";
    source.importer_version = imported.value().projected_default_material ? 2U : 1U;
    source.artifact_identity = "gltf.scene." + action.source_identity;
    const auto material_digest = source_catalog.canonical_digest();
    if (!material_digest) return failure(material_digest.error().with_context("glTF material lineage"));
    // Until domain-specific source hashing is available, geometry and hierarchy
    // both conservatively bind the entire captured source rather than implying
    // independent change detection.
    source.domain_digests = {
        {assets::SourceAssetDomain::geometry, source.source_digest},
        {assets::SourceAssetDomain::hierarchy, source.source_digest},
        {assets::SourceAssetDomain::material, material_digest.value()},
    };
    source.document_products = std::move(products);
    source.artifact_digest = assets::source_document_product_digest(
        source.source_digest, source.document_products);
    if (auto valid = source.validate(); !valid) return failure(valid.error().with_context("glTF source binding"));
    auto plan = preview_source_asset_addition(source);
    if (!plan) return failure(plan.error().with_context("glTF source admission"));
    if (auto applied = candidate.apply_source_reimport_plan(plan.value()); !applied)
        return failure(applied.error().with_context("glTF source staging"));
    if (auto valid = candidate.validate(); !valid) return failure(valid.error().with_context("glTF project staging"));
    if (project_generation_ != action.expected_project_generation || document_.revision() != action.expected_project_revision)
        return refuse(core::ErrorCode::stale_data, "project changed during glTF import preparation");
    editor::ProjectCommandAdmission admission;
    auto admitted = execute_command(std::make_unique<editor::ImportGltfSceneCommand>(
        admission, document_, std::move(meshes), std::move(nodes), std::move(catalog), std::move(source),
        store.root()),
        std::string(imported.value().projected_default_material
            ? "Import glTF Scene (two-sided default projection): " : "Import glTF Scene: ") +
            action.justification);
    if (admitted) {
        for (const auto& warning : imported.value().warnings) {
            admitted.value().warnings.emplace_back(core::ErrorCode::unsupported, warning);
        }
    }
    return admitted;
}

} // namespace carto::application
