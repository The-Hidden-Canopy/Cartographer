#include <algorithm>
#include <carto/application/application.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <limits>
#include <stdexcept>
namespace carto::application {
core::Result<DispatchReceipt>
ApplicationSession::import_gltf_material_library(const ImportGltfMaterialLibraryAction &action) {
    if (active_source_ != OperationSource::human || !action.ownership_or_license_confirmed)
        return failure({core::ErrorCode::unsupported,
                        "material reimport requires human confirmation of local "
                        "ownership or reuse license"});
    if (project_generation_ != action.expected_project_generation ||
        document_.revision() != action.expected_project_revision)
        return failure({core::ErrorCode::stale_data, "material reimport snapshot is stale"});
    if (!project_path_)
        return failure({core::ErrorCode::invalid_state,
                        "save the project before importing a managed material library"});
    auto imported = io::import_gltf_materials(action.source_bytes);
    if (!imported)
        return failure(imported.error());
    const auto identity = std::string("gltf.materials.") +
                          imported.value().artifact_digest.hex();
    if (document_.source_asset_registry().records().contains(identity))
        return failure({core::ErrorCode::invalid_state,
                        "this exact material source is already imported; reuse its "
                        "existing materials"});
    auto catalog = document_.material_catalog();
    const auto &input = imported.value().catalog;
    const auto reserve = [](const auto &records, std::size_t count) -> std::uint64_t {
        const auto last = records.empty() ? 0 : records.rbegin()->first;
        if (last > std::numeric_limits<std::uint64_t>::max() - count)
            throw std::runtime_error("material import identity space exhausted");
        return last + 1;
    };
    try {
        auto texture_id = reserve(catalog.textures(), input.textures().size()),
             sampler_id = reserve(catalog.samplers(), input.samplers().size()),
             material_id = reserve(catalog.materials(), input.materials().size());
        const auto project_root = project_path_->parent_path().empty()
                                      ? std::filesystem::current_path()
                                      : project_path_->parent_path();
        assets::BlobStore store(project_root / ".carto-assets" / "source-blobs");
        std::map<std::uint64_t, std::uint64_t> texture_ids, sampler_ids;
        for (const auto &[id, source] : input.textures()) {
            const auto previous = std::find_if(
                catalog.textures().begin(), catalog.textures().end(), [&](const auto &entry) {
                    return entry.second.content_digest == source.content_digest;
                });
            if (previous != catalog.textures().end()) {
                if(previous->second.width!=source.width||previous->second.height!=source.height||previous->second.format!=source.format)return failure({core::ErrorCode::validation_failed,"existing texture metadata disagrees with the independently decoded matching source digest"});
                texture_ids[id] = previous->first;
                continue;
            }
            auto record = source;
            record.id = texture_id++;
            record.relative_locator = store.path_for(source.content_digest)
                                          .lexically_relative(project_root)
                                          .generic_string();
            auto added = catalog.insert_texture(record);
            if (!added)
                return failure(added.error());
            texture_ids[id] = record.id;
        }
        for (const auto &[id, source] : input.samplers()) {
            auto record = source;
            record.id = sampler_id++;
            auto added = catalog.insert_sampler(record);
            if (!added)
                return failure(added.error());
            sampler_ids[id] = record.id;
        }
        for (const auto &[id, source] : input.materials()) {
            auto record = source;
            record.id = material_id++;
            record.name += " [" + imported.value().artifact_digest.hex().substr(0, 12) + "]";
            for (auto *ref : {&record.definition.base_color, &record.definition.metallic_roughness,
                              &record.definition.normal, &record.definition.occlusion,
                              &record.definition.emissive})
                if (*ref) {
                    ref->asset = texture_ids.at(ref->asset);
                    if (ref->sampler)
                        ref->sampler = sampler_ids.at(ref->sampler);
                }
            auto added = catalog.insert_material(record);
            if (!added)
                return failure(added.error());
        }
        auto candidate = document_;
        if (auto changed = candidate.set_material_catalog(catalog); !changed)
            return failure(changed.error());
        assets::SourceImportRecord provenance;
        provenance.identity = identity;
        provenance.relative_source_path = store.path_for(imported.value().artifact_digest)
                                              .lexically_relative(project_root)
                                              .generic_string();
        provenance.source_generation = core::Revision{1};
        provenance.kind = assets::SourceAssetKind::material;
        provenance.source_digest = imported.value().artifact_digest;
        provenance.importer_identity = "cartographer.gltf-material-library.v1";
        provenance.artifact_identity = identity;
        provenance.artifact_digest = imported.value().catalog.canonical_digest().value();
        provenance.domain_digests = {
            {assets::SourceAssetDomain::material, provenance.artifact_digest}};
        provenance.options.import_hierarchy = false;
        provenance.options.import_normals = false;
        if (auto bound = candidate.source_asset_registry_.set_record(std::move(provenance)); !bound)
            return failure(bound.error());
        if (auto valid = candidate.validate(); !valid)
            return failure(valid.error());
        // Immutable additions precede the single admitted project commit. On
        // failure, no old blob is replaced; unreferenced additions stay local.
        for (std::size_t i = 0; i < imported.value().encoded_images.size(); ++i) {
            auto saved =
                store.put(imported.value().encoded_images[i],
                          assets::texture_source_media_type(input.textures().at(i + 1).format));
            if (!saved)
                return failure(saved.error());
        }
        if (auto saved = store.put(action.source_bytes, "application/octet-stream"); !saved)
            return failure(saved.error());
        editor::ProjectCommandAdmission admission;
        const std::string label = "Import GLTF Material Library";
        auto receipt = execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(
                                           admission, document_, std::move(candidate), label),
                                       label + " " + imported.value().artifact_digest.hex(), {});
        return receipt;
    } catch (const std::exception &e) {
        return failure({core::ErrorCode::validation_failed, e.what()});
    }
}
} // namespace carto::application
