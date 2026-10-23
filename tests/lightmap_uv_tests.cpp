#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/lightmap_uv.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-lightmap-uv-tests-" + std::to_string(stamp) + "-" +
                std::to_string(sequence.fetch_add(1U)));
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

carto::geometry::EditableMesh make_square_mesh() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{1U}, geometry::VertexId{3U},
             geometry::VertexId{4U}}}}));
    attributes::AttributeSet attributes({{attributes::AttributeDomain::corner, 6U}});
    REQUIRE(attributes.add_layer(attributes::AttributeLayer{
        {"uv0.corners", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        {core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 0.0}, core::Vec2d{1.0, 1.0},
         core::Vec2d{0.0, 0.0}, core::Vec2d{1.0, 1.0}, core::Vec2d{0.0, 1.0}}}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
        7U, "Render UV", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::project::ProjectDocument make_project(
    carto::journal::Journal& journal,
    carto::geometry::EditableMesh mesh,
    std::uint64_t& mesh_asset) {
    using namespace carto;
    auto document = project::ProjectDocument::create("Lightmap UV Test");
    REQUIRE(document);
    const std::string initial_snapshot = document.value().serialize();
    const auto* snapshot_bytes = reinterpret_cast<const std::uint8_t*>(initial_snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(snapshot_bytes, initial_snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "lightmap-uv-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("lightmap.fixture"));
    return std::move(document).value();
}

carto::production::UniqueLightmapUvRecipe make_recipe(
    const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    std::uint32_t resolution = 256U,
    std::uint32_t padding = 4U,
    std::uint32_t maximum_udim_tiles = 1U) {
    const auto recipe = carto::production::make_unique_lightmap_uv_recipe(
        document, mesh_asset, 8U, "Lightmap UV", "uv.lightmap.generated",
        resolution, padding, maximum_udim_tiles);
    REQUIRE(recipe);
    return recipe.value();
}

void generated_uv_is_deterministic_unique_and_transactional() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "lightmap-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_square_mesh(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const auto recipe = make_recipe(document, mesh_asset);
    REQUIRE(!recipe.serialize().empty());
    REQUIRE(recipe.settings_digest());
    auto existing_set_recipe = recipe;
    existing_set_recipe.output_uv_set = 7U;
    const auto existing_set = production::compile_unique_lightmap_uv(
        existing_set_recipe, source);
    REQUIRE(!existing_set);
    REQUIRE(existing_set.error().code == core::ErrorCode::validation_failed);

    const auto first = production::compile_unique_lightmap_uv(recipe, source);
    const auto second = production::compile_unique_lightmap_uv(recipe, source);
    REQUIRE(first && second);
    REQUIRE(first.value().corners.size() == 6U);
    REQUIRE(first.value().settings_digest == second.value().settings_digest);
    REQUIRE(first.value().output_digest == second.value().output_digest);
    REQUIRE(first.value().artifact_digest == second.value().artifact_digest);
    REQUIRE(first.value().corners == second.value().corners);
    REQUIRE(first.value().byte_size == 6U * 36U + 52U);
    REQUIRE(first.value().tiles.size() == 1U);
    REQUIRE(first.value().tiles.front().udim_tile == 1001U);
    REQUIRE(first.value().tiles.front().face_count == 2U);
    REQUIRE(first.value().tiles.front().corner_count == 6U);
    REQUIRE(first.value().evidence_class == "carto.cpu-reference");
    REQUIRE(first.value().validate(source));
    REQUIRE(first.value().texels_per_unit > 0.0);

    const auto source_revision = source.revision();
    const auto source_uv_set_count = source.uv_sets().descriptors().size();
    const auto candidate_mesh = production::with_unique_lightmap_uv(source, first.value());
    REQUIRE(candidate_mesh);
    REQUIRE(source.revision() == source_revision);
    REQUIRE(source.uv_sets().descriptors().size() == source_uv_set_count);
    REQUIRE(source.uv_sets().find(8U) == nullptr);
    REQUIRE(candidate_mesh.value().uv_sets().find(8U) != nullptr);
    REQUIRE(candidate_mesh.value().uv_sets().find(7U)->active_for_render);
    REQUIRE(candidate_mesh.value().attributes().find("uv.lightmap.generated") != nullptr);

    auto colliding_layer_artifact = first.value();
    colliding_layer_artifact.recipe.output_layer_id = "uv0.corners";
    REQUIRE(colliding_layer_artifact.refresh_digests());
    REQUIRE(colliding_layer_artifact.validate(source));
    const auto rejected_layer_collision = production::with_unique_lightmap_uv(
        source, colliding_layer_artifact);
    REQUIRE(!rejected_layer_collision);
    REQUIRE(rejected_layer_collision.error().code == core::ErrorCode::validation_failed);
    REQUIRE(source.revision() == source_revision);

    const auto tampered = [&] {
        auto value = first.value();
        value.corners.front().uv.x = std::min(1.0, value.corners.front().uv.x + 0.01);
        return value;
    }();
    REQUIRE(!tampered.validate(source));

    const auto topology = source.topology();
    REQUIRE(topology);
    std::map<geometry::CornerId, core::Vec2d> coordinates;
    for (const auto& corner : first.value().corners) coordinates.emplace(corner.corner, corner.uv);
    std::map<geometry::FaceId, std::array<double, 4U>> face_bounds;
    for (const auto& corner : topology.value().corners) {
        const core::Vec2d uv = coordinates.at(corner.id);
        auto [bounds, inserted] = face_bounds.try_emplace(corner.face,
            std::array<double, 4U>{uv.x, uv.y, uv.x, uv.y});
        if (!inserted) {
            bounds->second[0U] = std::min(bounds->second[0U], uv.x);
            bounds->second[1U] = std::min(bounds->second[1U], uv.y);
            bounds->second[2U] = std::max(bounds->second[2U], uv.x);
            bounds->second[3U] = std::max(bounds->second[3U], uv.y);
        }
    }
    REQUIRE(face_bounds.size() == 2U);
    const auto a = face_bounds.at(geometry::FaceId{10U});
    const auto b = face_bounds.at(geometry::FaceId{20U});
    const double gap_x = std::max({0.0, b[0U] - a[2U], a[0U] - b[2U]});
    const double gap_y = std::max({0.0, b[1U] - a[3U], a[1U] - b[3U]});
    REQUIRE(gap_x >= static_cast<double>(recipe.padding_texels) / recipe.resolution ||
        gap_y >= static_cast<double>(recipe.padding_texels) / recipe.resolution);

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "lightmap-uv-tests");
    REQUIRE(transaction);
    REQUIRE(production::apply_unique_lightmap_uv(
        transaction.value(), mesh_asset, first.value()));
    const auto& staged_mesh = transaction.value().staged_document().meshes().at(mesh_asset);
    const auto* generated_set = staged_mesh.uv_sets().find(8U);
    REQUIRE(generated_set != nullptr);
    REQUIRE(generated_set->lightmap_candidate);
    REQUIRE(generated_set->tile_policy_version == 2U);
    REQUIRE(staged_mesh.uv_sets().active_for_render()->id == 7U);
    REQUIRE(staged_mesh.attributes().find("uv.lightmap.generated") != nullptr);
    const std::string operation = production::lightmap_uv_transaction_operation(first.value());
    REQUIRE(operation.size() <= 128U);
    const auto committed = transaction.value().commit(operation);
    REQUIRE(committed);
    REQUIRE(committed.value().revision_after == document.revision());
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries && !journal_entries.value().empty());
    REQUIRE(journal_entries.value().back().event_type == operation);

    const auto saved = document.save_atomic(directory.path() / "lightmap.carto");
    if (!saved) throw std::runtime_error("lightmap save failed: " + saved.error().message);
    const auto reopened = project::ProjectDocument::load(directory.path() / "lightmap.carto");
    REQUIRE(reopened);
    const auto reopened_mesh = reopened.value().meshes().find(mesh_asset);
    REQUIRE(reopened_mesh != reopened.value().meshes().end());
    REQUIRE(reopened_mesh->second.uv_sets().find(8U) != nullptr);
    REQUIRE(reopened_mesh->second.uv_sets().find(7U)->active_for_render);
    const auto* reopened_layer = reopened_mesh->second.attributes().find("uv.lightmap.generated");
    REQUIRE(reopened_layer != nullptr);
    const auto reopened_topology = reopened_mesh->second.topology();
    REQUIRE(reopened_topology);
    std::map<geometry::CornerId, core::Vec2d> expected_uvs;
    for (const production::LightmapUvCorner& corner : first.value().corners) {
        expected_uvs.emplace(corner.corner, corner.uv);
    }
    REQUIRE(reopened_layer->values.size() == reopened_topology.value().corners.size());
    for (std::size_t index = 0U; index < reopened_layer->values.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&reopened_layer->values[index]);
        REQUIRE(uv != nullptr);
        const core::Vec2d expected = expected_uvs.at(
            reopened_topology.value().corners[index].id);
        REQUIRE(uv->x == expected.x && uv->y == expected.y);
    }

    const core::Revision before_stale_apply = document.revision();
    auto stale_transaction = project::ProjectTransaction::begin(
        document, journal, "lightmap-uv-tests");
    REQUIRE(stale_transaction);
    const auto stale_apply = production::apply_unique_lightmap_uv(
        stale_transaction.value(), mesh_asset, first.value());
    REQUIRE(!stale_apply);
    REQUIRE(stale_apply.error().code == core::ErrorCode::stale_data);
    REQUIRE(stale_transaction.value().rollback());
    REQUIRE(document.revision() == before_stale_apply);
}

void udim_tiles_are_deterministic_bounded_and_persisted() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "udim-journal.log");
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    constexpr std::uint64_t chart_count = 5U;
    for (std::uint64_t chart = 0U; chart < chart_count; ++chart) {
        const std::uint64_t first_vertex = chart * 3U + 1U;
        const double x = static_cast<double>(chart) * 3.0;
        vertices.push_back({geometry::VertexId{first_vertex}, {x, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 1U}, {x + 1.0, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 2U}, {x + 1.0, 1.0, 0.0}});
        faces.push_back({geometry::FaceId{chart + 1U}, {
            geometry::VertexId{first_vertex}, geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, std::move(mesh), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const auto recipe = make_recipe(document, mesh_asset, 64U, 2U, 5U);
    auto invalid_budget = recipe;
    invalid_budget.maximum_udim_tiles = production::kMaxUniqueLightmapUdimTiles + 1U;
    REQUIRE(!invalid_budget.validate());

    const auto first = production::compile_unique_lightmap_uv(recipe, source);
    const auto repeated = production::compile_unique_lightmap_uv(recipe, source);
    REQUIRE(first && repeated);
    REQUIRE(first.value().tiles.size() == chart_count);
    REQUIRE(first.value().settings_digest == repeated.value().settings_digest);
    REQUIRE(first.value().output_digest == repeated.value().output_digest);
    REQUIRE(first.value().artifact_digest == repeated.value().artifact_digest);
    REQUIRE(first.value().corners == repeated.value().corners);
    REQUIRE(first.value().tiles == repeated.value().tiles);
    REQUIRE(first.value().validate(source));

    std::map<geometry::FaceId, std::uint32_t> tile_by_face;
    for (const production::LightmapUvCorner& corner : first.value().corners) {
        const auto [tile, inserted] = tile_by_face.emplace(corner.face, corner.udim_tile);
        REQUIRE(inserted || tile->second == corner.udim_tile);
        const std::uint32_t tile_index = corner.udim_tile - 1001U;
        const double tile_u = static_cast<double>(tile_index % 10U);
        const double tile_v = static_cast<double>(tile_index / 10U);
        REQUIRE(corner.uv.x >= tile_u && corner.uv.x < tile_u + 1.0);
        REQUIRE(corner.uv.y >= tile_v && corner.uv.y < tile_v + 1.0);
    }
    REQUIRE(tile_by_face.size() == chart_count);
    for (std::uint64_t chart = 0U; chart < chart_count; ++chart) {
        REQUIRE(tile_by_face.at(geometry::FaceId{chart + 1U}) == 1001U + chart);
        REQUIRE(first.value().tiles[chart].udim_tile == 1001U + chart);
        REQUIRE(first.value().tiles[chart].face_count == 1U);
        REQUIRE(first.value().tiles[chart].corner_count == 3U);
    }

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "lightmap-udim-tests");
    REQUIRE(transaction);
    REQUIRE(production::apply_unique_lightmap_uv(
        transaction.value(), mesh_asset, first.value()));
    const auto& staged_mesh = transaction.value().staged_document().meshes().at(mesh_asset);
    const auto* generated_set = staged_mesh.uv_sets().find(8U);
    REQUIRE(generated_set != nullptr);
    REQUIRE(generated_set->lightmap_candidate);
    REQUIRE(generated_set->tile_policy_version == 2U);
    const std::string operation = production::lightmap_uv_transaction_operation(first.value());
    REQUIRE(transaction.value().commit(operation));

    const auto saved = document.save_atomic(directory.path() / "udim.carto");
    if (!saved) throw std::runtime_error("UDIM save failed: " + saved.error().message);
    const auto reopened = project::ProjectDocument::load(directory.path() / "udim.carto");
    REQUIRE(reopened);
    const auto reopened_mesh = reopened.value().meshes().find(mesh_asset);
    REQUIRE(reopened_mesh != reopened.value().meshes().end());
    const auto* reopened_set = reopened_mesh->second.uv_sets().find(8U);
    REQUIRE(reopened_set != nullptr);
    REQUIRE(reopened_set->tile_policy_version == 2U);
    const auto* reopened_layer = reopened_mesh->second.attributes().find("uv.lightmap.generated");
    REQUIRE(reopened_layer != nullptr);
    const auto reopened_topology = reopened_mesh->second.topology();
    REQUIRE(reopened_topology);
    std::map<geometry::CornerId, production::LightmapUvCorner> expected;
    for (const production::LightmapUvCorner& corner : first.value().corners) {
        expected.emplace(corner.corner, corner);
    }
    REQUIRE(reopened_layer->values.size() == reopened_topology.value().corners.size());
    for (std::size_t index = 0U; index < reopened_layer->values.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&reopened_layer->values[index]);
        REQUIRE(uv != nullptr);
        const auto& corner = reopened_topology.value().corners[index];
        const production::LightmapUvCorner& expected_corner = expected.at(corner.id);
        REQUIRE(uv->x == expected_corner.uv.x && uv->y == expected_corner.uv.y);
        const std::uint32_t tile_index = expected_corner.udim_tile - 1001U;
        const auto inferred_tile = 1001U +
            static_cast<std::uint32_t>(std::floor(uv->x)) +
            10U * static_cast<std::uint32_t>(std::floor(uv->y));
        REQUIRE(inferred_tile == 1001U + tile_index);
    }
}

void unsupported_geometry_and_unfit_atlas_fail_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "unsupported-journal.log");
    geometry::EditableMesh non_planar;
    REQUIRE(non_planar.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.25}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}, geometry::VertexId{4U}}}}));
    REQUIRE(non_planar.validate());
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, non_planar, mesh_asset);
    const auto recipe = make_recipe(document, mesh_asset);
    const auto compile = production::compile_unique_lightmap_uv(
        recipe, document.meshes().at(mesh_asset));
    REQUIRE(!compile);
    REQUIRE(compile.error().code == core::ErrorCode::unsupported);

    TempDirectory crowded_directory;
    journal::Journal crowded_journal(crowded_directory.path() / "crowded-journal.log");
    geometry::EditableMesh crowded;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    constexpr std::uint64_t chart_count = 20U;
    for (std::uint64_t chart = 0U; chart < chart_count; ++chart) {
        const std::uint64_t first_vertex = chart * 3U + 1U;
        const double x = static_cast<double>(chart) * 2.0;
        vertices.push_back({geometry::VertexId{first_vertex}, {x, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 1U}, {x + 1.0, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 2U}, {x, 1.0, 0.0}});
        faces.push_back({geometry::FaceId{chart + 1U}, {
            geometry::VertexId{first_vertex}, geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
    }
    REQUIRE(crowded.insert_bulk(std::move(vertices), std::move(faces)));
    std::uint64_t crowded_asset = 0U;
    project::ProjectDocument crowded_document = make_project(
        crowded_journal, std::move(crowded), crowded_asset);
    const auto crowded_recipe = make_recipe(crowded_document, crowded_asset, 16U, 2U);
    const core::Revision crowded_revision = crowded_document.revision();
    const auto crowded_compile = production::compile_unique_lightmap_uv(
        crowded_recipe, crowded_document.meshes().at(crowded_asset));
    REQUIRE(!crowded_compile);
    REQUIRE(crowded_compile.error().code == core::ErrorCode::unsupported);
    REQUIRE(crowded_document.revision() == crowded_revision);
}

void adversarial_udim_chart_workload_fails_within_bound() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "workload-journal.log");
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    constexpr std::uint64_t chart_count = 10'001U;
    vertices.reserve(chart_count * 3U);
    faces.reserve(chart_count);
    for (std::uint64_t chart = 0U; chart < chart_count; ++chart) {
        const std::uint64_t first_vertex = chart * 3U + 1U;
        const double x = static_cast<double>(chart) * 3.0;
        vertices.push_back({geometry::VertexId{first_vertex}, {x, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 1U}, {x + 1.0, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 2U}, {x, 1.0, 0.0}});
        faces.push_back({geometry::FaceId{chart + 1U}, {
            geometry::VertexId{first_vertex}, geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, std::move(mesh), mesh_asset);
    const auto recipe = make_recipe(document, mesh_asset, 64U, 2U, 100U);
    const auto compile = production::compile_unique_lightmap_uv(
        recipe, document.meshes().at(mesh_asset));
    REQUIRE(!compile);
    REQUIRE(compile.error().code == core::ErrorCode::unsupported);
}

} // namespace

int main() {
    try {
        generated_uv_is_deterministic_unique_and_transactional();
        std::cout << "PASS deterministic unique lightmap UV generation and transactional application\n";
        udim_tiles_are_deterministic_bounded_and_persisted();
        std::cout << "PASS deterministic bounded UDIM lightmap UV scheduling and save/reopen\n";
        unsupported_geometry_and_unfit_atlas_fail_closed();
        std::cout << "PASS nonplanar and unfit lightmap UV inputs fail closed\n";
        adversarial_udim_chart_workload_fails_within_bound();
        std::cout << "PASS adversarial UDIM chart workload fails within its bound\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL unique lightmap UV tests: " << error.what() << '\n';
        return 1;
    }
}
