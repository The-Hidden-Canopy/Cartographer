#include <carto/assets/blob_store.hpp>
#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>
#include <carto/semantic/inspection.hpp>
#include <carto/semantic/lowering.hpp>
#include <carto/semantic/query.hpp>
#include <carto/semantic/schema.hpp>
#include <carto/semantic/sidecar.hpp>
#include <carto/world/model.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
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

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-semantic-query-tests-" + std::to_string(stamp));
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

carto::geometry::EditableMesh workshop_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    require(static_cast<bool>(mesh.insert_bulk(
                {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
                 {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
                 {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
                 {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
                {{geometry::FaceId{1U},
                  {geometry::VertexId{1U}, geometry::VertexId{2U},
                   geometry::VertexId{3U}}},
                 {geometry::FaceId{2U},
                  {geometry::VertexId{1U}, geometry::VertexId{3U},
                   geometry::VertexId{4U}}}})),
        "workshop mesh topology is valid");
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, 6U},
        {attributes::AttributeDomain::face, 2U},
    });
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"uv0.corners", attributes::AttributeDomain::corner,
                 attributes::AttributeType::vec2},
                {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0},
                 core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 0.0},
                 core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}})),
        "workshop mesh owns UV0 corners");
    require(static_cast<bool>(attributes.add_layer(attributes::AttributeLayer{
                {"condition.material_region", attributes::AttributeDomain::face,
                 attributes::AttributeType::uint32},
                {std::uint32_t{3U}, std::uint32_t{9U}}})),
        "workshop mesh owns two stable material regions");
    attributes::UvSetTable uv_sets;
    require(static_cast<bool>(uv_sets.add(attributes::UvSetDescriptor{
                1U, "UV0", "uv0.corners", true, true, false, 1U})),
        "workshop mesh declares UV0");
    require(static_cast<bool>(mesh.set_attribute_payload(
                std::move(attributes), std::move(uv_sets))),
        "workshop mesh accepts its material-region payload");
    return mesh;
}

struct Fixture {
    carto::project::ProjectDocument document;
    carto::semantic::SemanticSidecar sidecar;
    std::uint64_t root_object = 0U;
    std::uint64_t child_object = 0U;
    std::uint64_t mesh_asset = 0U;
};

Fixture make_fixture() {
    using namespace carto;
    TempDirectory temp;
    Fixture fixture;
    journal::Journal journal(temp.path() / "semantic-query-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        fixture.document, journal, "semantic-query-test");
    require(static_cast<bool>(transaction), "query fixture transaction begins");
    const auto mesh = transaction.value().add_mesh(workshop_mesh());
    require(static_cast<bool>(mesh), "workshop mesh enters project");
    fixture.mesh_asset = mesh.value();
    const auto root = transaction.value().create_object("Workshop root");
    require(static_cast<bool>(root), "root object enters project");
    fixture.root_object = root.value().value;
    const auto child = transaction.value().create_object(
        "Workshop interior", core::Transform{{2.0, 0.0, 0.0}});
    require(static_cast<bool>(child), "child object enters project");
    fixture.child_object = child.value().value;
    require(static_cast<bool>(transaction.value().attach_mesh(
                child.value(), mesh.value())),
        "child object references the reusable asset");
    require(static_cast<bool>(transaction.value().commit("semantic.query.fixture")),
        "query fixture commits through one journal event");

    world::WorldModel model;
    require(static_cast<bool>(model.insert_area(world::SemanticArea{
                .id = world::AreaId{1U},
                .semantic_type = "building",
                .geometry = world::AuthoringBinding{
                    world::AuthoringBindingKind::scene_object, fixture.root_object},
                .parent = std::nullopt,
                .tags = {"public"},
            })),
        "root semantic area enters fixture world model");
    require(static_cast<bool>(model.insert_area(world::SemanticArea{
                .id = world::AreaId{2U},
                .semantic_type = "room",
                .geometry = world::AuthoringBinding{
                    world::AuthoringBindingKind::scene_object, fixture.child_object},
                .parent = world::AreaId{1U},
                .tags = {"interior", "room"},
            })),
        "child semantic area enters fixture world model");
    require(static_cast<bool>(model.insert_path(world::SemanticPath{
                .id = world::PathId{1U},
                .semantic_type = "corridor",
                .control_points = {{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {4.0, 0.0, 1.0}},
                .start_area = world::AreaId{2U},
                .end_area = world::AreaId{1U},
                .width = 1.8,
                .closed = false,
            })),
        "semantic path enters fixture world model");
    world::StructureGraph graph;
    graph.id = world::StructureGraphId{1U};
    graph.semantic_type = "building-access";
    graph.nodes.emplace(
        1U, world::StructureNode{1U, "building", world::AreaId{1U}});
    graph.nodes.emplace(
        2U, world::StructureNode{2U, "room", world::AreaId{2U}});
    graph.edges.emplace(
        1U,
        world::StructureEdge{
            1U, 1U, 2U, "contains-access", world::PathId{1U}, true});
    require(static_cast<bool>(model.insert_structure_graph(std::move(graph))),
        "one world-semantic relationship enters fixture world model");
    require(static_cast<bool>(model.validate()),
        "fixture world model validates");

    semantic::SemanticSidecar sidecar;
    semantic::SemanticExtensionRecord record;
    record.schema = semantic::SchemaIdentity{"condition.example", "shelter"};
    record.schema_version = 1U;
    record.payload = "{\"state\":\"intact\",\"wear\":0.25}";
    record.payload_digest = digest(record.payload);
    record.bound_identities = {
        world::AuthoringBinding{
            world::AuthoringBindingKind::scene_object, fixture.child_object},
        world::AuthoringBinding{
            world::AuthoringBindingKind::mesh_asset, fixture.mesh_asset},
    };
    require(static_cast<bool>(sidecar.insert_record(std::move(record))),
        "one extension binding enters the fixture sidecar");
    require(static_cast<bool>(sidecar.refresh_digest()),
        "fixture sidecar digest refreshes");
    fixture.sidecar = std::move(sidecar);

    auto world_transaction = project::ProjectTransaction::begin(
        fixture.document, journal, "semantic-query-test");
    require(static_cast<bool>(world_transaction),
        "world-model transaction begins");
    require(static_cast<bool>(world_transaction.value().set_world_model(model)),
        "fixture world model enters the document");
    require(static_cast<bool>(world_transaction.value().commit(
                "semantic.query.world")),
        "fixture world model commits");
    return fixture;
}

void queries_resolve_fixture_semantics() {
    using namespace carto;
    const auto fixture = make_fixture();
    const auto& model = fixture.document.world_model();

    const auto rooms = semantic::areas_with_type(model, "room");
    require(rooms.size() == 1U && rooms.front() == world::AreaId{2U},
        "areas_with_type resolves the room area");
    const auto sites = semantic::areas_with_type(model, "building");
    require(sites.size() == 1U && sites.front() == world::AreaId{1U},
        "areas_with_type resolves the building area");
    require(semantic::areas_with_type(model, "dock").empty(),
        "areas_with_type stays empty for unknown types");

    const auto interior = semantic::areas_with_tag(model, "interior");
    require(interior.size() == 1U && interior.front() == world::AreaId{2U},
        "areas_with_tag resolves the interior tag");
    const auto public_areas = semantic::areas_with_tag(model, "public");
    require(public_areas.size() == 1U && public_areas.front() == world::AreaId{1U},
        "areas_with_tag resolves the public tag");
    require(semantic::areas_with_tag(model, "absent").empty(),
        "areas_with_tag stays empty for unknown tags");

    const auto outbound =
        semantic::paths_between(model, world::AreaId{2U}, world::AreaId{1U});
    require(outbound.size() == 1U && outbound.front() == world::PathId{1U},
        "paths_between resolves the authored corridor direction");
    require(semantic::paths_between(model, world::AreaId{1U}, world::AreaId{2U})
                .empty(),
        "paths_between does not invert the authored direction");
    require(semantic::paths_between(model, world::AreaId{1U}, world::AreaId{1U})
                .empty(),
        "paths_between stays empty without a terminal relationship");

    const auto object_bindings =
        semantic::bindings_for_object(fixture.sidecar, fixture.child_object);
    require(object_bindings.size() == 1U &&
                object_bindings.front()->schema.name == "shelter",
        "bindings_for_object resolves the extension binding");
    const auto mesh_bindings = semantic::bindings_for(
        fixture.sidecar,
        world::AuthoringBinding{
            world::AuthoringBindingKind::mesh_asset, fixture.mesh_asset});
    require(mesh_bindings.size() == 1U &&
                mesh_bindings.front() == object_bindings.front(),
        "bindings_for resolves the same record through its mesh binding");
    require(semantic::bindings_for_object(fixture.sidecar, fixture.root_object)
                .empty(),
        "bindings_for_object stays empty for unbound objects");
    require(semantic::bindings_for_object(fixture.sidecar, 999'999U).empty(),
        "bindings_for_object stays empty for unknown identities");

    const auto repeated = semantic::areas_with_tag(model, "interior");
    require(repeated == interior, "queries are deterministic across calls");
}

carto::semantic::SchemaRegistry fixture_registry() {
    using namespace carto::semantic;
    SchemaRegistry registry;
    SchemaRevision revision;
    revision.schema = SchemaIdentity{"condition.example", "shelter"};
    revision.version = SchemaVersion{1U};
    revision.fields = {
        {"state", SchemaValueKind::string, FieldOwnership::game_owned},
        {"wear", SchemaValueKind::real, FieldOwnership::game_owned},
        {"roof", SchemaValueKind::tag, FieldOwnership::native_lowerable},
    };
    std::sort(revision.fields.begin(), revision.fields.end(),
        [](const SchemaField& left, const SchemaField& right) {
            return left.name < right.name;
        });
    require(static_cast<bool>(revision.refresh_digest()),
        "fixture schema revision refreshes its digest");
    require(static_cast<bool>(registry.register_schema(revision)),
        "fixture schema revision registers");
    return registry;
}

void inspection_reports_resolution_tags_and_mismatches() {
    using namespace carto;
    const auto fixture = make_fixture();
    const auto registry = fixture_registry();

    semantic::TagLoweringMap lowering(fixture.sidecar.digest());
    require(static_cast<bool>(lowering.insert_binding(semantic::LoweredTagBinding{
                "interior", "consumer.zone.interior",
                "direct tag identity; no precision narrowing"})),
        "interior tag lowering enters the map");

    const auto known_identity = [&](const world::AuthoringBinding& binding) {
        return binding.identity == fixture.child_object ||
            binding.identity == fixture.mesh_asset;
    };
    const auto report = semantic::inspect_semantic_bindings(
        fixture.document.world_model(), fixture.sidecar, registry, &lowering,
        semantic::VerificationTier::native, known_identity);
    require(report.tier == semantic::VerificationTier::native,
        "report retains the requested verification tier");
    require(report.binding_resolutions.size() == 2U &&
                std::all_of(report.binding_resolutions.begin(),
                    report.binding_resolutions.end(),
                    [](const semantic::BindingResolution& resolution) {
                        return resolution.resolved;
                    }),
        "both bound authored identities resolve against document context");
    require(report.schema_mismatches.empty(),
        "registered schema reconciles without mismatches");
    require(report.lowered_tags == std::vector<std::string>{"interior"},
        "bound authored tags report as lowered");
    require(report.dropped_tags == std::vector<std::string>({"public", "room"}),
        "unbound authored tags report as dropped, sorted and unique");

    const auto unresolved = semantic::inspect_semantic_bindings(
        fixture.document.world_model(), fixture.sidecar, registry, &lowering,
        semantic::VerificationTier::structural,
        [](const world::AuthoringBinding&) { return false; });
    require(unresolved.binding_resolutions.size() == 2U &&
                std::none_of(unresolved.binding_resolutions.begin(),
                    unresolved.binding_resolutions.end(),
                    [](const semantic::BindingResolution& resolution) {
                        return resolution.resolved;
                    }),
        "unknown document context reports every binding as unresolved");

    semantic::SemanticSidecar drifting = fixture.sidecar;
    semantic::SemanticExtensionRecord foreign;
    foreign.schema = semantic::SchemaIdentity{"unknown.example", "den"};
    foreign.schema_version = 1U;
    foreign.payload = "{\"occupied\":true}";
    foreign.payload_digest = digest(foreign.payload);
    require(static_cast<bool>(drifting.insert_record(std::move(foreign))),
        "unregistered record enters drifting sidecar");
    require(static_cast<bool>(drifting.refresh_digest()),
        "drifting sidecar refreshes its digest");
    const auto mismatch_report = semantic::inspect_semantic_bindings(
        fixture.document.world_model(), drifting, registry, &lowering,
        semantic::VerificationTier::reference, known_identity);
    require(mismatch_report.schema_mismatches.size() == 1U &&
                mismatch_report.schema_mismatches.front().schema.name == "den",
        "unregistered schema identity reports one precise mismatch");
    require(mismatch_report.lowered_tags == report.lowered_tags &&
                mismatch_report.dropped_tags == report.dropped_tags,
        "schema drift does not alter tag classification");

    const std::string encoded = report.serialize();
    const auto reopened = semantic::SemanticInspectionReport::deserialize(encoded);
    require(reopened && reopened.value().serialize() == encoded,
        "inspection report survives deterministic round trip");
    const auto first_digest = report.canonical_digest();
    const auto second_digest = reopened.value().canonical_digest();
    require(first_digest && second_digest &&
                first_digest.value() == second_digest.value(),
        "inspection report identity survives reopen");
    require(semantic::VerificationTier::structural <
                semantic::VerificationTier::reference &&
            semantic::VerificationTier::reference <
                semantic::VerificationTier::native &&
            semantic::VerificationTier::native <
                semantic::VerificationTier::full_game,
        "verification tiers order without silent promotion");
}

} // namespace

int main() {
    queries_resolve_fixture_semantics();
    inspection_reports_resolution_tags_and_mismatches();
    std::cout << "semantic query tests passed\n";
    return EXIT_SUCCESS;
}
