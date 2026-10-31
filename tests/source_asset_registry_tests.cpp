#include <carto/assets/source_registry.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

assets::Sha256Digest digest(std::string_view text) {
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

assets::SourceImportRecord record(
    std::string identity = "asset.tree",
    std::string path = "Assets/trees/oak.glb",
    core::Revision generation = core::Revision{1U},
    std::string_view source = "oak-source-1",
    std::string_view geometry = "oak-geometry-1",
    std::string_view hierarchy = "oak-hierarchy-1",
    std::string_view material = "oak-material-1") {
    assets::SourceImportRecord value;
    value.identity = std::move(identity);
    value.relative_source_path = std::move(path);
    value.source_generation = generation;
    value.kind = assets::SourceAssetKind::mesh;
    value.source_digest = digest(source);
    value.importer_identity = "carto.import.gltf";
    value.importer_version = 3U;
    value.options.meters_per_source_unit = 0.01;
    value.options.up_axis = 2U;
    value.options.triangulate = true;
    value.artifact_identity = "mesh.oak-tree";
    value.artifact_digest = digest(std::string(geometry) + std::string(hierarchy) +
        std::string(material));
    value.domain_digests = {
        {assets::SourceAssetDomain::geometry, digest(geometry)},
        {assets::SourceAssetDomain::hierarchy, digest(hierarchy)},
        {assets::SourceAssetDomain::material, digest(material)}};
    return value;
}

void source_records_reject_hostile_paths_and_settings() {
    auto value = record();
    require(static_cast<bool>(value.validate()), "valid source record is admitted");
    const std::string encoded = value.serialize();
    const auto reopened = assets::SourceImportRecord::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "source record round-trips canonically");
    require(value.options.digest() == reopened.value().options.digest(),
        "typed import options have a stable digest");

    for (const std::string& invalid_path : std::vector<std::string>{
             std::string("C:") + "/Us" + "ers/artist/oak.glb",
             "/outside/oak.glb", "../oak.glb",
             "Assets/../oak.glb", "Assets\\oak.glb", "Assets//oak.glb",
             "Assets/NUL.txt", "Assets/CON", "Assets/file.", "Assets/file. ",
             std::string("Assets/") + '\0' + "oak.glb"}) {
        auto invalid = value;
        invalid.relative_source_path = invalid_path;
        require(!invalid.validate(), "absolute and traversing source paths are rejected");
    }
    auto invalid = value;
    invalid.source_digest = {};
    require(!invalid.validate(), "zero source digest is rejected");
    invalid = value;
    invalid.options.meters_per_source_unit = 0.0;
    require(!invalid.validate(), "invalid source units are rejected");
    invalid = value;
    invalid.options.weld_vertices = true;
    invalid.options.weld_tolerance_meters = 0.0;
    require(!invalid.validate(), "enabled vertex welding requires a positive tolerance");
    invalid = value;
    invalid.domain_digests[1U].domain = assets::SourceAssetDomain::geometry;
    require(!invalid.validate(), "duplicate or unsorted output-domain digests are rejected");
    require(!assets::SourceImportRecord::deserialize(encoded + "unexpected\n"),
        "unbound trailing source record data is rejected");
}

void authored_source_metadata_round_trips_and_rejects_invalid_boundaries() {
    const auto original = record();
    const std::string legacy = original.serialize();
    require(legacy.starts_with("SOURCE_IMPORT_RECORD 1\n"),
        "records without authored metadata retain the legacy format version");

    auto annotated = original;
    annotated.authored_tags = {"canopy", "exterior"};
    annotated.dependencies = {"asset.materials"};
    require(static_cast<bool>(annotated.validate()),
        "canonical authored tags and stable dependency identity are accepted");
    const std::string encoded = annotated.serialize();
    require(encoded.starts_with("SOURCE_IMPORT_RECORD 3\n"),
        "authored source metadata uses its explicit record schema version");
    const auto reopened = assets::SourceImportRecord::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded &&
            reopened.value().authored_tags == annotated.authored_tags &&
            reopened.value().dependencies == annotated.dependencies,
        "authored tags and dependency edges round-trip canonically");

    auto invalid = annotated;
    invalid.authored_tags = {"Exterior"};
    require(!invalid.validate(), "uppercase tags are rejected instead of silently normalized");
    invalid = annotated;
    invalid.authored_tags = {"exterior", "canopy"};
    require(!invalid.validate(), "unsorted authored tags are rejected");
    invalid = annotated;
    invalid.authored_tags = {"canopy", "canopy"};
    require(!invalid.validate(), "duplicate authored tags are rejected");
    invalid = annotated;
    invalid.dependencies = {invalid.identity};
    require(!invalid.validate(), "self-dependency edges are rejected");
    invalid = annotated;
    invalid.dependencies = {"asset:materials"};
    require(!invalid.validate(), "dependency identity syntax is bounded to registry tokens");
}

void dependencies_bind_to_the_complete_reviewed_registry() {
    assets::ProjectAssetRegistry registry;
    const auto source = record();
    auto material = record("asset.materials", "Assets/trees/oak-material.mtl");
    material.artifact_identity = "material.oak-tree";
    require(static_cast<bool>(registry.set_record(source)) &&
            static_cast<bool>(registry.set_record(material)),
        "source and dependency target enter the base registry independently");

    auto updated_source = source;
    updated_source.source_generation = core::Revision{2U};
    updated_source.authored_tags = {"canopy", "exterior"};
    updated_source.dependencies = {"asset.materials"};
    const auto reviewed = assets::plan_source_reimport(registry,
        {updated_source, material});
    require(reviewed && reviewed.value().entries.size() == 2U &&
            std::any_of(reviewed.value().entries.begin(), reviewed.value().entries.end(),
                [&source](const auto& entry) {
                    return entry.identity == source.identity &&
                        entry.change == assets::SourceReimportChange::metadata_only;
                }),
        "reviewed candidate resolves explicit dependencies against the complete active set");
    const auto adopted = assets::apply_source_reimport_plan(registry, reviewed.value());
    require(adopted && adopted.value().records().at(source.identity).dependencies ==
            updated_source.dependencies &&
            adopted.value().records().at(source.identity).authored_tags ==
            updated_source.authored_tags,
        "reviewed metadata is adopted as a new registry without mutating its base");
    const auto reopened = assets::ProjectAssetRegistry::deserialize(adopted.value().serialize());
    require(reopened && reopened.value().records().at(source.identity).dependencies ==
            updated_source.dependencies,
        "registry persistence retains cross-record dependency edges");

    auto missing_target = updated_source;
    missing_target.dependencies = {"asset.absent"};
    require(!assets::plan_source_reimport(registry, {missing_target, material}),
        "unknown dependency target is refused before adoption");
}

void botanical_generation_receipts_round_trip_without_private_identifiers() {
    botanical::Source source;
    source.identity = "pine-fixture";
    source.revision = core::Revision{7U};
    const std::string snapshot = source.serialize();
    const auto input_digest = digest(snapshot);

    auto value = record("botanical.generated.fixture", ".carto-assets/source-blobs/generated.gltf",
        core::Revision{1U}, "generated-gltf-bytes", "generated-geometry");
    value.importer_identity = "carto.import.generated-gltf";
    value.source_digest = digest("generated-gltf-bytes");
    value.artifact_identity = "mesh.botanical.fixture";
    value.botanical_generation = assets::BotanicalGenerationReceipt{
        snapshot, digest("opaque-runtime-job-id"), input_digest, value.source_digest};
    value.domain_digests = {{assets::SourceAssetDomain::geometry, value.artifact_digest}};

    require(static_cast<bool>(value.validate()),
        "exact canonical source and output bindings validate as a generated import");
    const std::string encoded = value.serialize();
    require(encoded.starts_with("SOURCE_IMPORT_RECORD 2\n") &&
            encoded.find("opaque-runtime-job-id") == std::string::npos &&
            encoded.find("https://") == std::string::npos,
        "serialized generation evidence omits raw job identity and service URL");
    const auto reopened = assets::SourceImportRecord::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded &&
            reopened.value().botanical_generation.has_value() &&
            reopened.value().botanical_generation->source_snapshot == snapshot &&
            reopened.value().botanical_generation->input_artifact_digest == input_digest &&
            reopened.value().botanical_generation->output_artifact_digest == value.source_digest,
        "project source record round-trips the exact generation receipt and digests");

    auto annotated = value;
    annotated.authored_tags = {"conifer", "exterior"};
    annotated.dependencies = {"asset.materials"};
    const std::string annotated_bytes = annotated.serialize();
    auto annotated_reopened = assets::SourceImportRecord::deserialize(annotated_bytes);
    require(annotated_bytes.starts_with("SOURCE_IMPORT_RECORD 3\n") &&
            annotated_reopened && annotated_reopened.value().serialize() == annotated_bytes &&
            annotated_reopened.value().authored_tags == annotated.authored_tags &&
            annotated_reopened.value().dependencies == annotated.dependencies &&
            annotated_reopened.value().botanical_generation.has_value() &&
            annotated_reopened.value().botanical_generation->serialize() ==
                value.botanical_generation->serialize(),
        "annotating a generated source preserves its complete generation receipt in version 3");
    annotated_reopened.value().authored_tags.clear();
    annotated_reopened.value().dependencies.clear();
    require(annotated_reopened.value().serialize() == encoded,
        "removing annotations restores the exact version-2 generation record");
    annotated.botanical_generation->output_artifact_digest = digest("wrong-annotated-output");
    require(!assets::SourceImportRecord::deserialize(annotated.serialize()),
        "version-3 annotations cannot hide a mismatched generation receipt");

    auto stale_source = value;
    stale_source.botanical_generation->source_snapshot += "\n";
    require(!stale_source.validate(),
        "source snapshot changes are rejected even when the outer record remains intact");
    auto mismatched_output = value;
    mismatched_output.botanical_generation->output_artifact_digest = digest("other-output");
    require(!mismatched_output.validate(),
        "output receipt cannot be detached from the exact imported source bytes");
    auto absent_job = value;
    absent_job.botanical_generation->job_identity_digest = {};
    require(!absent_job.validate(), "missing job identity digest fails closed");
    auto wrong_importer = value;
    wrong_importer.importer_identity = "carto.import.obj";
    require(!wrong_importer.validate(),
        "generation receipt cannot be attached to an unrelated importer");
}

void registry_retains_generation_tombstones_and_plans_without_mutation() {
    assets::ProjectAssetRegistry registry;
    const auto original = record();
    require(static_cast<bool>(registry.set_record(original)),
        "first source asset generation enters the registry");
    const std::string encoded = registry.serialize();
    const auto reopened = assets::ProjectAssetRegistry::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "registry save/reopen preserves active imports and lineage");
    const auto before_plan = registry.canonical_digest();
    require(static_cast<bool>(before_plan), "registry has a canonical digest");

    const auto unchanged = assets::plan_source_reimport(registry, {original});
    require(unchanged && unchanged.value().entries.size() == 1U &&
            unchanged.value().entries.front().change == assets::SourceReimportChange::unchanged,
        "same source import is classified unchanged");
    auto geometry_update = record("asset.tree", "Assets/trees/oak.glb",
        core::Revision{2U}, "oak-source-2", "oak-geometry-2");
    const auto changed_geometry = assets::plan_source_reimport(registry, {geometry_update});
    require(changed_geometry && changed_geometry.value().entries.front().change ==
            assets::SourceReimportChange::geometry_changed,
        "changed geometry is explicitly classified before adoption");
    const auto adopted = assets::apply_source_reimport_plan(registry, changed_geometry.value());
    require(adopted && adopted.value().records().at("asset.tree").serialize() ==
            geometry_update.serialize() &&
            registry.records().at("asset.tree").serialize() == original.serialize(),
        "reviewed reimport plan yields a new registry without mutating its base");
    require(!assets::apply_source_reimport_plan(adopted.value(), changed_geometry.value()),
        "a plan cannot be replayed after its base registry has advanced");
    auto forged = changed_geometry.value();
    forged.entries.front().change = assets::SourceReimportChange::unchanged;
    require(!assets::apply_source_reimport_plan(registry, forged),
        "a candidate change classification cannot be forged during adoption");
    auto conflict_candidate = record("asset.other", "Assets/trees/other.glb",
        core::Revision{1U}, "other-source", "other-geometry");
    const auto conflict_plan = assets::plan_source_reimport(registry, {conflict_candidate});
    require(conflict_plan && !assets::apply_source_reimport_plan(registry, conflict_plan.value()),
        "identity-conflict plans remain reviewable but cannot be adopted");
    require(registry.records().at("asset.tree").source_digest == original.source_digest &&
            registry.canonical_digest().value() == before_plan.value(),
        "building a reimport plan does not mutate current project state");

    auto hierarchy_update = record("asset.tree", "Assets/trees/oak.glb",
        core::Revision{2U}, "oak-source-2", "oak-geometry-1", "oak-hierarchy-2");
    const auto changed_hierarchy = assets::plan_source_reimport(registry, {hierarchy_update});
    require(changed_hierarchy && changed_hierarchy.value().entries.front().change ==
            assets::SourceReimportChange::hierarchy_changed,
        "changed hierarchy is kept distinct from geometry change");
    auto material_update = record("asset.tree", "Assets/trees/oak.glb",
        core::Revision{2U}, "oak-source-2", "oak-geometry-1", "oak-hierarchy-1",
        "oak-material-2");
    const auto changed_material = assets::plan_source_reimport(registry, {material_update});
    require(changed_material && changed_material.value().entries.front().change ==
            assets::SourceReimportChange::material_changed,
        "changed material assignment is kept distinct from geometry change");
    auto multi_update = record("asset.tree", "Assets/trees/oak.glb",
        core::Revision{2U}, "oak-source-2", "oak-geometry-2", "oak-hierarchy-2");
    const auto changed_multiple = assets::plan_source_reimport(registry, {multi_update});
    require(changed_multiple && changed_multiple.value().entries.front().change ==
            assets::SourceReimportChange::multiple_domains_changed,
        "simultaneous output changes are not flattened to one category");

    auto metadata_update = record("asset.tree", "Assets/trees/oak-v2.glb",
        core::Revision{2U}, "oak-source-1");
    const auto changed_metadata = assets::plan_source_reimport(registry, {metadata_update});
    require(changed_metadata && changed_metadata.value().entries.front().change ==
            assets::SourceReimportChange::metadata_only,
        "path-only changes are classified as metadata-only");

    auto wrong_generation = geometry_update;
    wrong_generation.source_generation = core::Revision{1U};
    require(!registry.set_record(wrong_generation),
        "stale source generation cannot replace the current import");
    require(static_cast<bool>(registry.set_record(geometry_update)),
        "explicit next generation can replace the reviewed source record");
    require(static_cast<bool>(registry.erase_record("asset.tree", core::Revision{2U})),
        "source removal requires the current exact generation");
    auto reused_generation = geometry_update;
    require(!registry.set_record(reused_generation),
        "deleted source identity cannot reuse its last generation");
    auto recreated = geometry_update;
    recreated.source_generation = core::Revision{3U};
    require(static_cast<bool>(registry.set_record(recreated)),
        "recreated source identity advances its retained generation");
    const auto round_trip = assets::ProjectAssetRegistry::deserialize(registry.serialize());
    require(round_trip && round_trip.value().lineage().at("asset.tree") ==
            core::Revision{3U},
        "tombstone lineage survives registry serialization");
}

void conflicting_artifact_identity_is_reported_not_adopted() {
    assets::ProjectAssetRegistry registry;
    const auto existing = record();
    require(static_cast<bool>(registry.set_record(existing)), "conflict fixture enters registry");
    auto reassigned = record("asset.other", "Assets/trees/other.glb");
    const auto plan = assets::plan_source_reimport(registry, {reassigned});
    require(plan && plan.value().entries.size() == 2U &&
            std::all_of(plan.value().entries.begin(), plan.value().entries.end(),
                [](const auto& entry) {
                    return entry.change == assets::SourceReimportChange::identity_conflict;
                }),
        "two source identities cannot silently claim the same imported artifact");
    require(static_cast<bool>(plan.value().canonical_digest()),
        "identity conflicts remain traceable in a canonical review plan");

    auto duplicate_candidate = reassigned;
    duplicate_candidate.identity = "asset.third";
    require(!assets::plan_source_reimport(registry, {reassigned, duplicate_candidate}),
        "duplicate artifact identities within one incoming set fail closed");
}

void moved_source_relink_binds_new_locator_without_changing_identity() {
    assets::ProjectAssetRegistry registry;
    const auto original = record();
    require(static_cast<bool>(registry.set_record(original)), "relink fixture enters registry");
    const auto staged_digest = original.source_digest;

    require(!assets::plan_source_relink(registry, "asset.missing", core::Revision{1U},
            "Assets/trees/oak-moved.glb", staged_digest),
        "relinking a missing source identity fails closed");
    require(!assets::plan_source_relink(registry, "asset.tree", core::Revision{2U},
            "Assets/trees/oak-moved.glb", staged_digest),
        "relinking against a stale active generation fails closed");
    require(!assets::plan_source_relink(registry, "asset.tree", core::Revision{1U},
            "../outside/oak.glb", staged_digest),
        "a traversing staged locator is rejected");
    require(!assets::plan_source_relink(registry, "asset.tree", core::Revision{1U},
            "Assets/trees/oak-moved.glb", digest("oak-source-2")),
        "digest-changed content cannot relink and must use the reimport flow");
    require(!assets::plan_source_relink(registry, "asset.tree", core::Revision{1U},
            "Assets/trees/oak.glb", staged_digest),
        "rebinding the already-bound locator is rejected");
    require(!assets::plan_source_relink(registry, "asset.tree", core::Revision{1U},
            "Assets/trees/oak-moved.glb", assets::Sha256Digest{}),
        "a zero staged digest is rejected");

    const auto plan = assets::plan_source_relink(registry, "asset.tree",
        core::Revision{1U}, "Assets/trees/oak-moved.glb", staged_digest);
    require(plan && plan.value().disposition == assets::SourceRelinkDisposition::moved &&
            plan.value().incoming.identity == "asset.tree" &&
            plan.value().incoming.relative_source_path == "Assets/trees/oak-moved.glb" &&
            plan.value().incoming.source_digest == original.source_digest &&
            plan.value().incoming.artifact_identity == original.artifact_identity &&
            plan.value().incoming.artifact_digest == original.artifact_digest &&
            plan.value().incoming.source_generation == core::Revision{2U},
        "digest-identical staged bytes classify as a pure move with advanced lineage");

    const auto relinked = assets::apply_source_relink_plan(registry, plan.value());
    require(relinked && relinked.value().records().at("asset.tree").identity == "asset.tree" &&
            relinked.value().records().at("asset.tree").relative_source_path ==
                "Assets/trees/oak-moved.glb" &&
            relinked.value().records().at("asset.tree").source_digest == original.source_digest &&
            relinked.value().records().at("asset.tree").artifact_identity ==
                original.artifact_identity &&
            relinked.value().records().at("asset.tree").source_generation ==
                core::Revision{2U} &&
            relinked.value().lineage().at("asset.tree") == core::Revision{2U} &&
            registry.records().at("asset.tree").relative_source_path == "Assets/trees/oak.glb",
        "pure move rebinds the locator in a new registry without mutating its base");
    const auto round_trip = assets::ProjectAssetRegistry::deserialize(
        relinked.value().serialize());
    require(round_trip && round_trip.value().serialize() == relinked.value().serialize() &&
            round_trip.value().lineage().at("asset.tree") == core::Revision{2U},
        "relinked identity and lineage survive registry serialization");

    require(!assets::apply_source_relink_plan(relinked.value(), plan.value()),
        "a reviewed relink cannot replay after its base registry advanced");
    auto forged = plan.value();
    forged.incoming.source_generation = core::Revision{9U};
    require(!assets::apply_source_relink_plan(registry, forged),
        "a relink candidate cannot be forged during adoption");
    auto stolen_path = record("asset.other", "Assets/trees/oak-moved.glb",
        core::Revision{1U}, "other-source", "other-geometry");
    stolen_path.artifact_identity = "mesh.other-tree";
    require(static_cast<bool>(registry.set_record(stolen_path)),
        "second fixture enters registry");
    require(!assets::plan_source_relink(registry, "asset.tree", core::Revision{1U},
            "Assets/trees/oak-moved.glb", staged_digest),
        "a staged locator owned by another source asset is rejected");

    const auto second = assets::plan_source_relink(relinked.value(), "asset.tree",
        core::Revision{2U}, "Assets/trees/oak-final.glb", staged_digest);
    require(second && second.value().incoming.source_generation == core::Revision{3U} &&
            static_cast<bool>(assets::apply_source_relink_plan(relinked.value(), second.value())),
        "repeated pure moves keep advancing the retained generation monotonically");
}

void annotated_source_relink_preserves_reviewed_metadata_and_dependency_scope() {
    assets::ProjectAssetRegistry registry;
    auto material = record("asset.materials", "Assets/materials/bark.mtl");
    material.artifact_identity = "material.bark";
    auto source = record();
    source.authored_tags = {"conifer", "exterior"};
    source.dependencies = {material.identity};
    require(static_cast<bool>(registry.set_record(material)) &&
            static_cast<bool>(registry.set_record(source)) && registry.validate(),
        "annotated relink fixture resolves its dependency in the active registry");
    const std::string base = registry.serialize();
    const auto plan = assets::plan_source_relink(registry, source.identity,
        source.source_generation, "Assets/trees/oak-moved.glb", source.source_digest);
    require(static_cast<bool>(plan), "annotated source can prepare an exact-byte relink");
    const auto relinked = assets::apply_source_relink_plan(registry, plan.value());
    require(relinked && registry.serialize() == base &&
            relinked.value().records().at(source.identity).authored_tags == source.authored_tags &&
            relinked.value().records().at(source.identity).dependencies == source.dependencies &&
            relinked.value().records().at(material.identity).serialize() == material.serialize(),
        "relink preserves authored metadata and the dependency target without changing the base");
    auto forged = plan.value();
    forged.incoming.authored_tags = {"unreviewed"};
    require(!assets::apply_source_relink_plan(registry, forged) && registry.serialize() == base,
        "a relink cannot smuggle an unreviewed annotation edit into the registry");
    require(!assets::plan_source_reimport(registry, {source}) && registry.serialize() == base,
        "removing a still-referenced dependency is refused without mutation");
    auto detached = source;
    detached.source_generation = source.source_generation.next();
    detached.dependencies.clear();
    const auto removal = assets::plan_source_reimport(registry, {detached});
    require(static_cast<bool>(removal), "explicitly detaching the edge permits target removal review");
    const auto adopted = assets::apply_source_reimport_plan(registry, removal.value());
    require(adopted && adopted.value().validate() && adopted.value().records().size() == 1U &&
            adopted.value().records().at(source.identity).dependencies.empty() &&
            adopted.value().records().at(source.identity).authored_tags == source.authored_tags &&
            adopted.value().lineage().at(material.identity) == material.source_generation &&
            registry.serialize() == base,
        "reviewed removal clears only the explicit edge and retains target lineage");
}

} // namespace

int main() {
    source_records_reject_hostile_paths_and_settings();
    authored_source_metadata_round_trips_and_rejects_invalid_boundaries();
    dependencies_bind_to_the_complete_reviewed_registry();
    botanical_generation_receipts_round_trip_without_private_identifiers();
    registry_retains_generation_tombstones_and_plans_without_mutation();
    conflicting_artifact_identity_is_reported_not_adopted();
    moved_source_relink_binds_new_locator_without_changing_identity();
    annotated_source_relink_preserves_reviewed_metadata_and_dependency_scope();
    std::cout << "source asset registry tests passed\n";
    return EXIT_SUCCESS;
}
