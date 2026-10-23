#include <carto/assets/blob_store.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include <carto/world/model.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-world-model-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::world::WorldModel world_fixture(
    carto::world::AuthoringBinding geometry = {},
    carto::core::Revision base_revision = {},
    bool reverse_variant_edits = false) {
    using namespace carto::world;
    WorldModel model;
    require(static_cast<bool>(model.insert_area(SemanticArea{
                .id = AreaId{1U},
                .semantic_type = "building",
                .geometry = geometry,
                .parent = std::nullopt,
                .tags = {"occupied", "public"},
            })),
            "root semantic area enters fixture");
    require(static_cast<bool>(model.insert_area(SemanticArea{
                .id = AreaId{2U},
                .semantic_type = "room",
                .geometry = {},
                .parent = AreaId{1U},
                .tags = {"interior", "room"},
            })),
            "child semantic area enters fixture");
    require(static_cast<bool>(model.insert_path(SemanticPath{
                .id = PathId{1U},
                .semantic_type = "corridor",
                .control_points = {{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {4.0, 0.0, 1.0}},
                .start_area = AreaId{2U},
                .end_area = AreaId{1U},
                .width = 1.8,
                .closed = false,
            })),
            "semantic path enters fixture");

    StructureGraph graph;
    graph.id = StructureGraphId{1U};
    graph.semantic_type = "building-access";
    graph.nodes.emplace(1U, StructureNode{1U, "building", AreaId{1U}});
    graph.nodes.emplace(2U, StructureNode{2U, "room", AreaId{2U}});
    graph.edges.emplace(1U, StructureEdge{1U, 1U, 2U, "contains-access", PathId{1U}, true});
    require(static_cast<bool>(model.insert_structure_graph(std::move(graph))),
            "structure graph enters fixture");

    require(static_cast<bool>(model.insert_instance_field(InstanceField{
                .id = InstanceFieldId{1U},
                .semantic_type = "vegetation",
                .area = AreaId{1U},
                .bounds = {{0.0, 0.0, 0.0}, {10.0, 3.0, 10.0}},
                .recipe_identity = "public/vegetation-grid/v1",
                .recipe_digest = digest("vegetation-grid-v1"),
                .seed = 42U,
                .maximum_instances = 1'000U,
                .suppressed_instances = {2U, 19U},
                .geometry_evidence = std::nullopt,
                .physical_material_intent = "",
                .construction_profile = "",
                .surface_domain_identity = "",
            })),
            "instance field enters fixture");

    VariantLayer layer;
    layer.id = VariantLayerId{1U};
    layer.name = "wet-weather";
    layer.base_project_revision = base_revision;
    layer.edits = {
        VariantEdit{
            VariantTargetKind::instance_field, 1U, VariantOperation::set_value,
            "density", "0.75"},
        VariantEdit{
            VariantTargetKind::semantic_area, 2U, VariantOperation::set_value,
            "surface-state", "wet"},
    };
    if (reverse_variant_edits) {
        std::reverse(layer.edits.begin(), layer.edits.end());
    }
    require(static_cast<bool>(model.insert_variant_layer(std::move(layer))),
            "variant layer enters fixture");
    return model;
}

carto::world::WorldModel world_region_fixture() {
    using namespace carto::world;
    WorldModel model;
    require(static_cast<bool>(model.insert_area(SemanticArea{
                .id = AreaId{101U},
                .semantic_type = "north-district",
                .geometry = {},
                .parent = std::nullopt,
                .tags = {},
            })), "first large-world area enters the fixture");
    require(static_cast<bool>(model.insert_area(SemanticArea{
                .id = AreaId{202U},
                .semantic_type = "south-district",
                .geometry = {},
                .parent = std::nullopt,
                .tags = {},
            })), "second large-world area enters the fixture");
    require(static_cast<bool>(model.insert_path(SemanticPath{
                .id = PathId{303U},
                .semantic_type = "inter-region-connector",
                .control_points = {{-10.0, 0.0, 0.0}, {10.0, 0.0, 0.0}},
                .start_area = AreaId{101U},
                .end_area = AreaId{202U},
                .width = 2.0,
                .closed = false,
            })), "cross-region path enters the fixture");
    const auto regions = model.apply_region_edits({
        WorldRegionEdit{
            .id = WorldRegionId{1U},
            .expected_revision = 0U,
            .cell_x = -9'000'000,
            .cell_y = 12,
            .area_ids = {AreaId{101U}},
            .path_ids = {PathId{303U}},
            .neighbor_ids = {WorldRegionId{2U}},
            .cross_region_path_ids = {PathId{303U}},
        },
        WorldRegionEdit{
            .id = WorldRegionId{2U},
            .expected_revision = 0U,
            .cell_x = 9'000'000,
            .cell_y = 12,
            .area_ids = {AreaId{202U}},
            .path_ids = {PathId{303U}},
            .neighbor_ids = {WorldRegionId{1U}},
            .cross_region_path_ids = {PathId{303U}},
        },
    });
    require(static_cast<bool>(regions), "two linked regions enter as one edit batch");
    require(static_cast<bool>(model.validate()), "large-world region fixture validates");
    return model;
}

carto::world::WorldRegionEdit region_edit(
    const carto::world::WorldRegion& source,
    std::uint64_t expected_revision,
    std::int64_t cell_x,
    std::int64_t cell_y) {
    return carto::world::WorldRegionEdit{
        .id = source.id,
        .expected_revision = expected_revision,
        .cell_x = cell_x,
        .cell_y = cell_y,
        .area_ids = source.area_ids,
        .path_ids = source.path_ids,
        .neighbor_ids = source.neighbor_ids,
        .cross_region_path_ids = source.cross_region_path_ids,
    };
}

void five_primitives_round_trip_deterministically() {
    const auto model = world_fixture();
    require(static_cast<bool>(model.validate()), "five-primitives fixture validates");
    require(model.areas().size() == 2U && model.paths().size() == 1U &&
                model.structure_graphs().size() == 1U &&
                model.instance_fields().size() == 1U &&
                model.variant_layers().size() == 1U,
            "fixture contains all five universal primitive kinds");

    const auto first_digest = model.canonical_digest();
    const auto reordered_digest = world_fixture({}, {}, true).canonical_digest();
    require(first_digest && reordered_digest &&
                first_digest.value() == reordered_digest.value(),
            "variant authoring order does not alter the canonical world digest");

    const std::string encoded = model.serialize();
    const auto reopened = carto::world::WorldModel::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
            "world model survives deterministic save and reopen");
    const auto reopened_digest = reopened.value().canonical_digest();
    require(reopened_digest && reopened_digest.value() == first_digest.value(),
            "reopened world model preserves its canonical digest");
}

void large_world_regions_preserve_identity_and_explicit_cross_region_paths() {
    using namespace carto::world;
    const auto model = world_region_fixture();
    require(model.regions().size() == 2U &&
                model.regions().at(WorldRegionId{1U}).revision == 1U &&
                model.regions().at(WorldRegionId{1U}).cell_x == -9'000'000 &&
                model.regions().at(WorldRegionId{2U}).cell_x == 9'000'000,
            "stable region IDs bind signed large-world cell coordinates");
    require(model.regions().at(WorldRegionId{1U}).neighbor_ids ==
                std::vector<WorldRegionId>{WorldRegionId{2U}} &&
                model.regions().at(WorldRegionId{2U}).cross_region_path_ids ==
                    std::vector<PathId>{PathId{303U}},
            "neighbor and cross-region path references are explicit on both sides");

    const auto encoded = model.serialize();
    require(encoded.rfind("CARTOGRAPHER_WORLD_MODEL 3\n", 0U) == 0U,
            "large-world region data uses schema-v3 framing");
    const auto reopened = WorldModel::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded &&
                reopened.value().regions() == model.regions(),
            "region identities, revisions, coordinates, and cross-links survive reopen");

    auto missing_neighbor = model;
    auto broken = region_edit(model.regions().at(WorldRegionId{2U}), 1U, 9'000'000, 13);
    broken.neighbor_ids.clear();
    require(!missing_neighbor.apply_region_edits({std::move(broken)}) &&
                missing_neighbor.serialize() == model.serialize(),
            "one-sided neighbor edits fail atomically");

    auto missing_connector = region_edit(model.regions().at(WorldRegionId{2U}), 1U,
        9'000'000, 13);
    missing_connector.cross_region_path_ids.clear();
    auto unchanged = model;
    require(!unchanged.apply_region_edits({std::move(missing_connector)}) &&
                unchanged.serialize() == model.serialize(),
            "cross-region path edits cannot silently drop the other endpoint reference");

    auto duplicate_cell = region_edit(model.regions().at(WorldRegionId{2U}), 1U,
        -9'000'000, 12);
    require(!unchanged.apply_region_edits({std::move(duplicate_cell)}) &&
                unchanged.serialize() == model.serialize(),
            "distinct region identities cannot claim the same authored cell coordinate");

    std::vector<WorldRegionEdit> oversized_batch(WorldModel::kMaxRegionEditBatch + 1U);
    require(!unchanged.apply_region_edits(std::move(oversized_batch)) &&
                unchanged.serialize() == model.serialize(),
            "oversized region edit batches are rejected without partial publication");

    std::string v2 = WorldModel{}.serialize();
    v2.replace(0U, std::string{"CARTOGRAPHER_WORLD_MODEL 3"}.size(),
        "CARTOGRAPHER_WORLD_MODEL 2");
    const auto region_table = v2.find("REGIONS 0\n");
    require(region_table != std::string::npos,
            "schema-v3 fixture contains the optional region table");
    v2.erase(region_table, std::string{"REGIONS 0\n"}.size());
    const auto migrated = WorldModel::deserialize(v2);
    require(migrated && migrated.value().regions().empty() &&
                migrated.value().serialize().rfind("CARTOGRAPHER_WORLD_MODEL 3\n", 0U) == 0U,
            "schema-v2 documents migrate with an explicit empty region table");
}

void large_world_region_edits_are_bounded_and_rebase_by_stable_identity() {
    using namespace carto::world;
    const auto base = world_region_fixture();
    auto local = base;
    const auto local_edit = region_edit(base.regions().at(WorldRegionId{1U}),
        1U, -8'000'000, 12);
    const auto local_receipt = local.apply_region_edits({local_edit});
    require(local_receipt && local_receipt.value().changes ==
                std::vector<WorldRegionRevisionChange>{{WorldRegionId{1U}, 1U, 2U}},
            "region edit increments only the addressed stable identity");

    auto stale = region_edit(base.regions().at(WorldRegionId{1U}), 1U,
        -7'000'000, 12);
    const auto stale_result = local.apply_region_edits({std::move(stale)});
    require(!stale_result && local.regions().at(WorldRegionId{1U}).cell_x == -8'000'000 &&
                local.regions().at(WorldRegionId{1U}).revision == 2U,
            "stale edits to the same region fail without overwriting newer data");

    auto remote = region_edit(base.regions().at(WorldRegionId{2U}),
        1U, 9'000'000, 20);
    const auto rebased = local.apply_region_edits({std::move(remote)});
    require(rebased && local.regions().at(WorldRegionId{1U}).cell_x == -8'000'000 &&
                local.regions().at(WorldRegionId{1U}).revision == 2U &&
                local.regions().at(WorldRegionId{2U}).cell_y == 20 &&
                local.regions().at(WorldRegionId{2U}).revision == 2U,
            "disjoint region edits rebase onto the latest model without flattening either edit");

    auto oversized = region_edit(local.regions().at(WorldRegionId{1U}), 2U,
        -8'000'000, 12);
    oversized.area_ids.clear();
    oversized.area_ids.reserve(WorldModel::kMaxRegionMemberships + 1U);
    for (std::size_t index = 1U; index <= WorldModel::kMaxRegionMemberships + 1U; ++index) {
        oversized.area_ids.push_back(AreaId{static_cast<std::uint64_t>(index)});
    }
    const auto before_oversized = local.serialize();
    require(!local.apply_region_edits({std::move(oversized)}) &&
                local.serialize() == before_oversized,
            "oversized per-region membership edits are rejected atomically");
}

void large_world_region_project_reopen_and_transaction_rebase_preserve_both_sides() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "region-journal.log");
    auto seed = project::ProjectTransaction::begin(document, journal, "region-test");
    require(seed && seed.value().set_world_model(world_region_fixture()) &&
                seed.value().commit("world.region.seed"),
            "region graph enters through a journaled project transaction");
    const auto project_path = temp.path() / "regions.carto";
    require(static_cast<bool>(document.save_atomic(project_path)),
            "region graph project snapshot saves");
    auto reopened = project::ProjectDocument::load(project_path);
    require(reopened && reopened.value().world_model().regions().size() == 2U,
            "project reopen restores cross-region identities");

    auto local_transaction = project::ProjectTransaction::begin(
        reopened.value(), journal, "region-local");
    auto remote_transaction = project::ProjectTransaction::begin(
        reopened.value(), journal, "region-remote");
    require(local_transaction && remote_transaction,
            "concurrent region edits stage from the reopened project revision");

    auto local_model = reopened.value().world_model();
    auto local_edit = region_edit(local_model.regions().at(world::WorldRegionId{1U}),
        1U, -8'500'000, 12);
    require(local_model.apply_region_edits({local_edit}) &&
                local_transaction.value().set_world_model(std::move(local_model)),
            "local region edit stages against its exact region revision");

    auto remote_model = reopened.value().world_model();
    auto remote_edit = region_edit(remote_model.regions().at(world::WorldRegionId{2U}),
        1U, 9'000'000, 24);
    require(remote_model.apply_region_edits({remote_edit}) &&
                remote_transaction.value().set_world_model(std::move(remote_model)) &&
                remote_transaction.value().commit("world.region.remote"),
            "independent region edit commits through the project journal");
    require(!local_transaction.value().commit("world.region.stale") &&
                local_transaction.value().rollback(),
            "stale project transaction is refused before it can overwrite remote work");

    auto rebased_model = reopened.value().world_model();
    const auto rebase_receipt = rebased_model.apply_region_edits({local_edit});
    require(rebase_receipt && rebased_model.regions().at(world::WorldRegionId{1U}).cell_x ==
                -8'500'000 &&
                rebased_model.regions().at(world::WorldRegionId{2U}).cell_y == 24,
            "local region patch rebases onto the concurrent project snapshot");
    auto rebase_transaction = project::ProjectTransaction::begin(
        reopened.value(), journal, "region-rebase");
    require(rebase_transaction &&
                rebase_transaction.value().set_world_model(std::move(rebased_model)) &&
                rebase_transaction.value().commit("world.region.rebase"),
            "rebased region snapshot commits through the journal");

    require(static_cast<bool>(reopened.value().save_atomic(project_path)),
            "rebased region snapshot saves");
    const auto final_reopen = project::ProjectDocument::load(project_path);
    require(final_reopen &&
                final_reopen.value().world_model().regions().at(world::WorldRegionId{1U}).cell_x ==
                    -8'500'000 &&
                final_reopen.value().world_model().regions().at(world::WorldRegionId{2U}).cell_y ==
                    24 &&
                final_reopen.value().world_model().regions().at(world::WorldRegionId{1U}).revision ==
                    2U &&
                final_reopen.value().world_model().regions().at(world::WorldRegionId{2U}).revision ==
                    2U &&
                final_reopen.value().world_model().regions().at(world::WorldRegionId{1U})
                    .cross_region_path_ids == std::vector<world::PathId>{world::PathId{303U}},
            "final save/reopen preserves the merged edits and cross-region references");
    const auto events = journal.read_all();
    require(events && events.value().size() == 3U &&
                events.value()[1].event_type == "world.region.remote" &&
                events.value()[2].event_type == "world.region.rebase",
            "rejected stale edit emits no journal event and both accepted edits are traced");
}

void cross_reference_and_population_failures_are_blocked() {
    using namespace carto::world;
    WorldModel cyclic;
    require(static_cast<bool>(
                cyclic.insert_area({AreaId{1U}, "room", {}, AreaId{2U}, {}})),
            "first cycle member is locally valid");
    require(static_cast<bool>(
                cyclic.insert_area({AreaId{2U}, "room", {}, AreaId{1U}, {}})),
            "second cycle member is locally valid");
    require(!cyclic.validate(), "semantic area hierarchy cycles are rejected");

    WorldModel missing_terminal;
    require(static_cast<bool>(missing_terminal.insert_path(SemanticPath{
                .id = PathId{1U},
                .semantic_type = "road",
                .control_points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}},
                .start_area = AreaId{99U},
                .end_area = std::nullopt,
            })),
            "path with unresolved terminal is locally shape-valid");
    require(!missing_terminal.validate(),
            "path terminal must resolve in the complete world model");

    auto bad_field = InstanceField{
        .id = InstanceFieldId{1U},
        .semantic_type = "trees",
        .area = std::nullopt,
        .bounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}},
        .recipe_identity = "trees/v1",
        .recipe_digest = digest("trees"),
        .seed = 1U,
        .maximum_instances = 4U,
        .suppressed_instances = {3U, 2U},
        .geometry_evidence = std::nullopt,
        .physical_material_intent = "",
        .construction_profile = "",
        .surface_domain_identity = "",
    };
    require(!bad_field.validate(),
            "unsorted reconstructible-population exceptions are rejected");

    auto model = world_fixture();
    VariantLayer missing_target;
    missing_target.id = VariantLayerId{2U};
    missing_target.name = "missing-target";
    missing_target.edits.push_back({
        VariantTargetKind::semantic_area, 999U, VariantOperation::set_value,
        "state", "changed"});
    require(static_cast<bool>(model.insert_variant_layer(std::move(missing_target))),
            "missing target remains locally valid before aggregate resolution");
    require(!model.validate(), "variant edits cannot target absent primitives");
}

void malformed_world_bytes_fail_closed() {
    using carto::world::WorldModel;
    const std::string valid = world_fixture().serialize();
    require(!WorldModel::deserialize(valid + "unexpected"),
            "trailing world-model data is rejected");

    std::string bad_magic = valid;
    bad_magic.front() = 'X';
    require(!WorldModel::deserialize(bad_magic),
            "unknown world-model framing is rejected");

    require(!WorldModel::deserialize(
                "CARTOGRAPHER_WORLD_MODEL 1\nAREAS 65537\n"),
            "hostile primitive count is rejected before allocation");
    require(!WorldModel::deserialize(
                "CARTOGRAPHER_WORLD_MODEL 1\nAREAS -1\n"),
            "negative unsigned counts are rejected without wraparound");

    std::string unsafe = valid;
    const auto type = unsafe.find("\"building\"");
    require(type != std::string::npos, "unsafe-text fixture finds semantic type");
    unsafe.replace(type, std::string("\"building\"").size(), "\"build\nprivate\"");
    require(!WorldModel::deserialize(unsafe),
            "escaped control text cannot enter a semantic identifier");

    std::string oversized(WorldModel::kMaxSerializedBytes + 1U, 'x');
    require(!WorldModel::deserialize(oversized),
            "oversized world model is rejected before parsing");
}

void project_transaction_journals_world_authoring_and_preserves_bindings() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "journal.log");

    auto transaction = project::ProjectTransaction::begin(document, journal, "world-test");
    require(static_cast<bool>(transaction), "world transaction begins at the journal tail");
    const auto object = transaction.value().create_object("Bound Building");
    require(static_cast<bool>(object), "bound scene object is staged");
    const auto model = world_fixture(
        {world::AuthoringBindingKind::scene_object, object.value().value});
    require(static_cast<bool>(transaction.value().set_world_model(model)),
            "world model is staged through the governed project boundary");
    const auto receipt = transaction.value().commit("world.model.bind");
    require(receipt && receipt.value().revision_before == core::Revision{} &&
                receipt.value().revision_after == core::Revision{2U},
            "one journal event binds both object and world source revisions");
    require(document.world_model().areas().size() == 2U,
            "committed document exposes immutable world authoring state");

    const auto events = journal.read_all();
    require(events && events.value().size() == 1U &&
                events.value().front().event_type == "world.model.bind",
            "world mutation emits exactly one durable journal event");

    const auto path = temp.path() / "world.carto";
    require(static_cast<bool>(document.save_atomic(path)),
            "schema-v7 project saves with world authoring state");
    const auto reopened = project::ProjectDocument::load(path);
    require(reopened && reopened.value().serialize() == document.serialize() &&
                reopened.value().world_model().structure_graphs().size() == 1U,
            "saved project reopens with exact universal primitives");

    auto removal = project::ProjectTransaction::begin(document, journal, "world-test");
    require(static_cast<bool>(removal), "bound-object removal transaction begins");
    require(!removal.value().remove_object(object.value()),
            "authoritative object cannot be removed behind its semantic binding");
    require(static_cast<bool>(removal.value().rollback()),
            "rejected removal transaction rolls back");
    const auto after_rejection = journal.read_all();
    require(after_rejection && after_rejection.value().size() == 1U,
            "rejected background mutation emits no false domain event");

    project::ProjectDocument mesh_document;
    journal::Journal mesh_journal(temp.path() / "mesh-journal.log");
    auto mesh_binding = project::ProjectTransaction::begin(
        mesh_document, mesh_journal, "world-test");
    require(static_cast<bool>(mesh_binding), "mesh-binding transaction begins");
    auto box = geometry::make_box({1.0, 1.0, 1.0});
    require(static_cast<bool>(box), "mesh-binding fixture geometry is valid");
    const auto mesh_asset = mesh_binding.value().add_mesh(std::move(box.value()));
    require(static_cast<bool>(mesh_asset), "mesh-binding fixture enters the project");
    require(static_cast<bool>(mesh_binding.value().set_world_model(world_fixture(
                {world::AuthoringBindingKind::mesh_asset, mesh_asset.value()}))),
            "world model accepts an existing authoritative mesh binding");
    require(static_cast<bool>(mesh_binding.value().commit("world.mesh.bind")),
            "mesh binding commits through the project journal");

    auto mesh_removal = project::ProjectTransaction::begin(
        mesh_document, mesh_journal, "world-test");
    require(static_cast<bool>(mesh_removal), "bound-mesh removal transaction begins");
    require(!mesh_removal.value().remove_mesh(mesh_asset.value()),
            "authoritative mesh cannot be removed behind its semantic binding");
    require(static_cast<bool>(mesh_removal.value().rollback()),
            "rejected mesh-removal transaction rolls back");
    const auto mesh_events = mesh_journal.read_all();
    require(mesh_events && mesh_events.value().size() == 1U &&
                mesh_events.value().front().event_type == "world.mesh.bind",
            "rejected mesh removal emits no false domain event");

    project::ProjectDocument isolated;
    journal::Journal isolated_journal(temp.path() / "isolated-journal.log");
    auto missing_binding = project::ProjectTransaction::begin(
        isolated, isolated_journal, "world-test");
    require(static_cast<bool>(missing_binding), "missing-binding transaction begins");
    require(!missing_binding.value().set_world_model(world_fixture(
                {world::AuthoringBindingKind::scene_object, 999U})),
            "world admission rejects a missing authoritative geometry binding");
    require(static_cast<bool>(missing_binding.value().rollback()),
            "missing-binding transaction rolls back");

    auto missing_mesh_binding = project::ProjectTransaction::begin(
        isolated, isolated_journal, "world-test");
    require(static_cast<bool>(missing_mesh_binding),
            "missing-mesh-binding transaction begins");
    require(!missing_mesh_binding.value().set_world_model(world_fixture(
                {world::AuthoringBindingKind::mesh_asset, 999U})),
            "world admission rejects a missing authoritative mesh binding");
    require(static_cast<bool>(missing_mesh_binding.value().rollback()),
            "missing-mesh-binding transaction rolls back");
    const auto isolated_events = isolated_journal.read_all();
    require(isolated_events && isolated_events.value().empty(),
            "failed world admission does not append an audit event");
}

void project_schema_migration_is_explicit_and_fail_closed() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "schema-journal.log");

    auto transaction = project::ProjectTransaction::begin(document, journal, "schema-test");
    require(transaction && transaction.value().set_world_model(world_fixture()) &&
                transaction.value().commit("world.schema.fixture"),
            "schema fixture commits a world model");
    const std::string current = document.serialize();

    std::string missing = current;
    const auto world_start = missing.find("WORLD_MODEL ");
    const auto world_end = missing.rfind("\nEND\n");
    require(world_start != std::string::npos && world_end != std::string::npos,
            "schema fixture contains the mandatory world record");
    missing.erase(world_start, world_end - world_start + 1U);
    require(!project::ProjectDocument::deserialize(missing),
            "schema-v7 project cannot omit its world model");

    std::string legacy = missing;
    const std::string current_header = "CARTOGRAPHER_PROJECT " +
        std::to_string(project::ProjectDocument::kSchemaVersion);
    const auto header = legacy.find(current_header);
    require(header == 0U, "legacy fixture finds the current project header");
    legacy.replace(header, current_header.size(), "CARTOGRAPHER_PROJECT 6");
    const auto mesh_patches = legacy.find("MESH_PATCHES 0\n");
    require(mesh_patches != std::string::npos,
            "legacy fixture finds the schema-v29 mesh-patch table");
    legacy.erase(mesh_patches, std::string("MESH_PATCHES 0\n").size());
    const auto namespace_start = legacy.find("SOURCE_NAMESPACE ");
    const auto namespace_end = legacy.find('\n', namespace_start);
    require(namespace_start != std::string::npos && namespace_end != std::string::npos,
            "legacy fixture finds the current source namespace record");
    legacy.erase(namespace_start, namespace_end - namespace_start + 1U);
    const auto reopened_legacy = project::ProjectDocument::deserialize(legacy);
    require(reopened_legacy && reopened_legacy.value().world_model().empty(),
            "schema-v6 projects remain readable with an explicit empty world model");

    auto future = world_fixture({}, core::Revision{99U});
    project::ProjectDocument stale_document;
    journal::Journal stale_journal(temp.path() / "stale-journal.log");
    auto stale = project::ProjectTransaction::begin(
        stale_document, stale_journal, "schema-test");
    require(stale && !stale.value().set_world_model(std::move(future)),
            "variant layers cannot cite a future project revision");
    require(static_cast<bool>(stale.value().rollback()),
            "future-revision transaction rolls back");
}

void schema_v1_world_documents_still_load_with_defaulted_material_intent() {
    using namespace carto::world;
    const std::string recipe = digest("vegetation-grid-v1").hex();
    const std::string v1 =
        "CARTOGRAPHER_WORLD_MODEL 1\n"
        "AREAS 1\n"
        "AREA 1 0 0 0 \"building\" 0\n"
        "PATHS 0\n"
        "STRUCTURE_GRAPHS 0\n"
        "INSTANCE_FIELDS 1\n"
        "INSTANCE_FIELD 1 1 0 0 0 10 3 10 42 1000 \"vegetation\" "
        "\"public/vegetation-grid/v1\" " + recipe + " 2 2 19\n"
        "VARIANT_LAYERS 0\n"
        "END\n";

    const auto reopened = WorldModel::deserialize(v1);
    require(static_cast<bool>(reopened), "schema-v1 world document still loads");
    require(reopened.value().areas().size() == 1U &&
                reopened.value().instance_fields().size() == 1U,
            "schema-v1 primitives survive the versioned read path");
    const auto& field = reopened.value().instance_fields().at(InstanceFieldId{1U});
    require(field.seed == 42U && field.maximum_instances == 1'000U &&
                field.suppressed_instances == std::vector<std::uint64_t>({2U, 19U}),
            "schema-v1 instance field population is preserved exactly");
    require(!field.geometry_evidence && field.physical_material_intent.empty() &&
                field.construction_profile.empty() && field.surface_domain_identity.empty(),
            "schema-v1 instance fields load with defaulted materialization intent");

    const std::string upgraded = reopened.value().serialize();
    require(upgraded.rfind("CARTOGRAPHER_WORLD_MODEL 3\n", 0U) == 0U,
            "re-saved schema-v1 document upgrades to the current schema framing");
    const auto rereopened = WorldModel::deserialize(upgraded);
    require(rereopened && rereopened.value().serialize() == upgraded,
            "upgraded current-schema document round trips deterministically");

    WorldModel equivalent;
    require(static_cast<bool>(equivalent.insert_area(SemanticArea{
                .id = AreaId{1U},
                .semantic_type = "building",
                .geometry = {},
                .parent = std::nullopt,
                .tags = {},
            })),
            "equivalent current-schema area enters through the governed boundary");
    require(static_cast<bool>(equivalent.insert_instance_field(InstanceField{
                .id = InstanceFieldId{1U},
                .semantic_type = "vegetation",
                .area = AreaId{1U},
                .bounds = {{0.0, 0.0, 0.0}, {10.0, 3.0, 10.0}},
                .recipe_identity = "public/vegetation-grid/v1",
                .recipe_digest = digest("vegetation-grid-v1"),
                .seed = 42U,
                .maximum_instances = 1'000U,
                .suppressed_instances = {2U, 19U},
                .geometry_evidence = std::nullopt,
                .physical_material_intent = "",
                .construction_profile = "",
                .surface_domain_identity = "",
            })),
            "equivalent current-schema instance field enters with defaulted intent");
    const auto v1_digest = reopened.value().canonical_digest();
    const auto v2_digest = equivalent.canonical_digest();
    require(v1_digest && v2_digest && v1_digest.value() == v2_digest.value(),
            "schema-v1 content digests identically to the equivalent current authoring state");
}

void schema_v2_material_intent_round_trips_and_fails_closed() {
    using namespace carto::world;
    WorldModel model;
    require(static_cast<bool>(model.insert_area(SemanticArea{
                .id = AreaId{3U},
                .semantic_type = "building",
                .geometry = {},
                .parent = std::nullopt,
                .tags = {},
            })),
            "schema-v2 area enters the fixture");
    require(static_cast<bool>(model.insert_instance_field(InstanceField{
                .id = InstanceFieldId{5U},
                .semantic_type = "trees",
                .area = AreaId{3U},
                .bounds = {{0.0, 0.0, 0.0}, {8.0, 4.0, 8.0}},
                .recipe_identity = "public/tree-cluster/v2",
                .recipe_digest = digest("tree-cluster-v2"),
                .seed = 7U,
                .maximum_instances = 64U,
                .suppressed_instances = {2U, 19U},
                .geometry_evidence = digest("tree-cluster-evidence"),
                .physical_material_intent = "consumer.physical.intent.weathered-wood",
                .construction_profile = "public/construction/timber-frame/v2",
                .surface_domain_identity = "condition.surface.exterior-shell",
            })),
            "schema-v2 instance field carries geometry evidence and physical intent");
    require(static_cast<bool>(model.validate()), "schema-v2 fixture validates");

    const std::string encoded = model.serialize();
    require(encoded.rfind("CARTOGRAPHER_WORLD_MODEL 3\n", 0U) == 0U &&
                encoded.find("\"consumer.physical.intent.weathered-wood\"") != std::string::npos,
            "current schema serialization carries the materialization intent");
    const auto reopened = WorldModel::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
            "current world model round trips deterministically");

    std::string v2 = encoded;
    v2.replace(0U, std::string{"CARTOGRAPHER_WORLD_MODEL 3"}.size(),
        "CARTOGRAPHER_WORLD_MODEL 2");
    const auto region_table = v2.find("REGIONS 0\n");
    require(region_table != std::string::npos,
            "schema-v2 material-intent fixture has no region table");
    v2.erase(region_table, std::string{"REGIONS 0\n"}.size());
    const auto migrated_v2 = WorldModel::deserialize(v2);
    require(migrated_v2 && migrated_v2.value().serialize() == encoded,
            "schema-v2 material intent migrates to current schema deterministically");
    const auto& field = migrated_v2.value().instance_fields().at(InstanceFieldId{5U});
    require(field.geometry_evidence.has_value() &&
                field.geometry_evidence.value() == digest("tree-cluster-evidence") &&
                field.physical_material_intent == "consumer.physical.intent.weathered-wood" &&
                field.construction_profile == "public/construction/timber-frame/v2" &&
                field.surface_domain_identity == "condition.surface.exterior-shell",
            "schema-v2 intent references survive migration exactly");

    const auto first_digest = model.canonical_digest();
    const auto second_digest = migrated_v2.value().canonical_digest();
    require(first_digest && second_digest && first_digest.value() == second_digest.value(),
            "schema-v2 digest survives migration");

    auto invalid_evidence = InstanceField{
        .id = InstanceFieldId{6U},
        .semantic_type = "trees",
        .area = std::nullopt,
        .bounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}},
        .recipe_identity = "trees/v1",
        .recipe_digest = digest("trees"),
        .seed = 1U,
        .maximum_instances = 4U,
        .suppressed_instances = {},
        .geometry_evidence = carto::assets::Sha256Digest{},
        .physical_material_intent = "",
        .construction_profile = "",
        .surface_domain_identity = "",
    };
    require(!invalid_evidence.validate(),
            "present-but-zero geometry evidence is rejected");
    auto unsafe_intent = InstanceField{
        .id = InstanceFieldId{7U},
        .semantic_type = "trees",
        .area = std::nullopt,
        .bounds = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}},
        .recipe_identity = "trees/v1",
        .recipe_digest = digest("trees"),
        .seed = 1U,
        .maximum_instances = 4U,
        .suppressed_instances = {},
        .geometry_evidence = std::nullopt,
        .physical_material_intent = "bad\nintent",
        .construction_profile = "",
        .surface_domain_identity = "",
    };
    require(!unsafe_intent.validate(),
            "control text cannot enter a materialization intent reference");

    std::string future = encoded;
    future.replace(0U, std::string{"CARTOGRAPHER_WORLD_MODEL 3"}.size(),
        "CARTOGRAPHER_WORLD_MODEL 99");
    require(!WorldModel::deserialize(future),
            "unknown future world-model versions fail precisely");
    require(!WorldModel::deserialize(encoded + "unexpected"),
            "trailing world-model data is rejected");
    const std::string bad_evidence =
        "CARTOGRAPHER_WORLD_MODEL 2\n"
        "AREAS 0\n"
        "PATHS 0\n"
        "STRUCTURE_GRAPHS 0\n"
        "INSTANCE_FIELDS 1\n"
        "INSTANCE_FIELD 5 0 0 0 0 1 1 1 1 4 \"trees\" \"trees/v1\" " +
        digest("trees").hex() + " 0 nothex \"\" \"\" \"\"\n"
        "VARIANT_LAYERS 0\n"
        "END\n";
    require(!WorldModel::deserialize(bad_evidence),
            "malformed schema-v2 geometry evidence is rejected");
}

} // namespace

int main() {
    five_primitives_round_trip_deterministically();
    large_world_regions_preserve_identity_and_explicit_cross_region_paths();
    large_world_region_edits_are_bounded_and_rebase_by_stable_identity();
    large_world_region_project_reopen_and_transaction_rebase_preserve_both_sides();
    cross_reference_and_population_failures_are_blocked();
    malformed_world_bytes_fail_closed();
    project_transaction_journals_world_authoring_and_preserves_bindings();
    project_schema_migration_is_explicit_and_fail_closed();
    schema_v1_world_documents_still_load_with_defaulted_material_intent();
    schema_v2_material_intent_round_trips_and_fails_closed();
    std::cout << "world model tests passed\n";
    return EXIT_SUCCESS;
}
