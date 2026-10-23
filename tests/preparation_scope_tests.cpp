#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/preparation_scope.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-preparation-scope-tests-" + std::to_string(stamp));
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

carto::geometry::EditableMesh triangle(double offset) {
    using namespace carto;
    geometry::EditableMesh mesh;
    require(static_cast<bool>(mesh.insert_bulk(
                {{geometry::VertexId{1U}, {offset, 0.0, 0.0}},
                 {geometry::VertexId{2U}, {offset + 1.0, 0.0, 0.0}},
                 {geometry::VertexId{3U}, {offset, 1.0, 0.0}}},
                {{geometry::FaceId{1U},
                  {geometry::VertexId{1U}, geometry::VertexId{2U},
                   geometry::VertexId{3U}}}})),
        "scope fixture mesh is valid");
    return mesh;
}

carto::project::ProjectDocument scope_document() {
    using namespace carto;
    TempDirectory temp;
    project::ProjectDocument document;
    journal::Journal journal(temp.path() / "scope-journal.log");
    auto transaction = project::ProjectTransaction::begin(document, journal, "scope-test");
    require(static_cast<bool>(transaction), "scope transaction begins");
    require(transaction.value().insert_mesh(1U, triangle(0.0)) &&
            transaction.value().insert_mesh(2U, triangle(2.0)) &&
            transaction.value().insert_mesh(3U, triangle(4.0)),
        "three stable mesh assets enter the fixture");
    require(transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{1U}, "Root", {}, std::nullopt, std::nullopt, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{2U}, "Assembly", {}, scene::ObjectId{1U}, 1U, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{3U}, "Leaf", {}, scene::ObjectId{2U}, 2U, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{4U}, "Sibling", {}, scene::ObjectId{1U}, 1U, true, false}) &&
            transaction.value().insert_object(scene::SceneObject{
                scene::ObjectId{5U}, "Detached", {}, std::nullopt, 3U, true, false}),
        "scope fixture hierarchy enters the transaction");
    require(static_cast<bool>(transaction.value().commit("preparation.scope.fixture")),
        "scope fixture commits through the journal boundary");
    return document;
}

void stable_scope_resolution_is_explicit() {
    using namespace carto;
    const auto document = scope_document();

    const auto asset = production::resolve_preparation_scope(document, {
        production::PreparationScopeKind::asset, document.revision(), {2U}, {}});
    require(asset && asset.value().asset_ids == std::vector<std::uint64_t>{2U} &&
            asset.value().node_ids.empty(),
        "asset scope resolves one explicit stable mesh identity");

    const auto selection = production::resolve_preparation_scope(document, {
        production::PreparationScopeKind::selection, document.revision(), {}, {3U}});
    require(selection &&
            selection.value().node_ids == std::vector<std::uint64_t>({1U, 2U, 3U}) &&
            selection.value().asset_ids == std::vector<std::uint64_t>({1U, 2U}),
        "selection scope closes over ancestors and referenced reusable assets");

    const auto assembly = production::resolve_preparation_scope(document, {
        production::PreparationScopeKind::assembly, document.revision(), {}, {2U}});
    require(assembly &&
            assembly.value().node_ids == std::vector<std::uint64_t>({1U, 2U, 3U}) &&
            assembly.value().asset_ids == std::vector<std::uint64_t>({1U, 2U}),
        "assembly scope includes descendants, ancestors, and referenced assets");

    const auto scene = production::resolve_preparation_scope(document, {
        production::PreparationScopeKind::scene, document.revision(), {}, {}});
    require(scene && scene.value().node_ids.size() == 5U &&
            scene.value().asset_ids == std::vector<std::uint64_t>({1U, 2U, 3U}),
        "scene scope resolves every authored node and mesh deterministically");
}

void stale_and_hostile_scope_requests_fail_closed() {
    using namespace carto;
    const auto document = scope_document();
    require(!production::resolve_preparation_scope(document, {
                production::PreparationScopeKind::scene,
                core::Revision{document.revision().value() - 1U}, {}, {}}),
        "scope resolution rejects a stale captured project revision");
    require(!production::resolve_preparation_scope(document, {
                production::PreparationScopeKind::selection,
                document.revision(), {2U, 1U}, {}}),
        "non-canonical selection IDs are rejected");
    require(!production::resolve_preparation_scope(document, {
                production::PreparationScopeKind::asset,
                document.revision(), {999U}, {}}),
        "missing explicit assets fail precisely");
    require(!production::resolve_preparation_scope(document, {
                production::PreparationScopeKind::assembly,
                document.revision(), {}, {999U}}),
        "missing assembly roots fail precisely");

    production::ResolvedPreparationScope forged{
        production::PreparationScopeKind::selection,
        document.revision(), {2U}, {3U}};
    require(!forged.validate(document),
        "resolved scope cannot omit required ancestors and asset closure");
}

void single_mesh_export_never_chooses_the_first_implicitly() {
    using namespace carto;
    const auto document = scope_document();
    const auto explicit_asset = production::resolve_single_mesh_export(document, 2U);
    require(explicit_asset && explicit_asset.value() == 2U,
        "single-mesh formats accept an exact stable asset ID");
    require(!production::resolve_single_mesh_export(document, std::nullopt),
        "multi-mesh projects reject an omitted ambiguous export scope");
    require(!production::resolve_single_mesh_export(document, 999U),
        "single-mesh export rejects a missing explicit asset");

    TempDirectory temp;
    project::ProjectDocument single;
    journal::Journal journal(temp.path() / "single-journal.log");
    auto transaction = project::ProjectTransaction::begin(single, journal, "scope-test");
    require(transaction && transaction.value().insert_mesh(41U, triangle(0.0)) &&
            transaction.value().commit("preparation.scope.single"),
        "single-mesh compatibility fixture commits");
    const auto implicit_single = production::resolve_single_mesh_export(single, std::nullopt);
    require(implicit_single && implicit_single.value() == 41U,
        "legacy omission remains unambiguous for a one-mesh project");
}

} // namespace

int main() {
    stable_scope_resolution_is_explicit();
    stale_and_hostile_scope_requests_fail_closed();
    single_mesh_export_never_chooses_the_first_implicitly();
    std::cout << "preparation scope tests passed\n";
    return EXIT_SUCCESS;
}
