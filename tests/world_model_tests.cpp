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

} // namespace

int main() {
    five_primitives_round_trip_deterministically();
    cross_reference_and_population_failures_are_blocked();
    malformed_world_bytes_fail_closed();
    project_transaction_journals_world_authoring_and_preserves_bindings();
    project_schema_migration_is_explicit_and_fail_closed();
    std::cout << "world model tests passed\n";
    return EXIT_SUCCESS;
}
