#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/uv_island_pack.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
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
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-uv-island-pack-tests-" + std::to_string(stamp) + "-" +
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

carto::geometry::EditableMesh attach_uvs(
    carto::geometry::EditableMesh mesh,
    std::vector<carto::core::Vec2d> values) {
    using namespace carto;
    const auto topology = mesh.topology();
    REQUIRE(topology);
    REQUIRE(values.size() == topology.value().corners.size());
    std::vector<attributes::AttributeValue> attribute_values;
    attribute_values.reserve(values.size());
    for (const core::Vec2d value : values) attribute_values.emplace_back(value);
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, topology.value().corners.size()}});
    REQUIRE(attributes.add_layer(attributes::AttributeLayer{
        {"uv.source.corners", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec2},
        std::move(attribute_values)}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
        7U, "Source UV", "uv.source.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_overlapping_triangles() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {4.0, 0.0, 0.0}},
         {geometry::VertexId{5U}, {5.0, 0.0, 0.0}},
         {geometry::VertexId{6U}, {4.0, 1.0, 0.0}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    return attach_uvs(std::move(mesh), {
        {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0},
        {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}});
}

carto::geometry::EditableMesh make_density_mismatch_triangles(
    bool degenerate_first_uv = false) {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {4.0, 0.0, 0.0}},
         {geometry::VertexId{5U}, {5.0, 0.0, 0.0}},
         {geometry::VertexId{6U}, {4.0, 1.0, 0.0}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    return attach_uvs(std::move(mesh), degenerate_first_uv
        ? std::vector<core::Vec2d>{{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0},
            {0.0, 0.0}, {0.5, 0.0}, {0.0, 0.5}}
        : std::vector<core::Vec2d>{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0},
            {0.0, 0.0}, {0.5, 0.0}, {0.0, 0.5}});
}

carto::geometry::EditableMesh make_rotatable_islands() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {5.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 3.0, 0.0}},
         {geometry::VertexId{4U}, {10.0, 0.0, 0.0}},
         {geometry::VertexId{5U}, {13.0, 0.0, 0.0}},
         {geometry::VertexId{6U}, {10.0, 5.0, 0.0}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    return attach_uvs(std::move(mesh), {
        {0.0, 0.0}, {5.0, 0.0}, {0.0, 3.0},
        {-9.0, 4.0}, {-6.0, 4.0}, {-9.0, 9.0}});
}

carto::geometry::EditableMesh make_square(bool seam) {
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
    if (!seam) {
        return attach_uvs(std::move(mesh), {
            {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0},
            {0.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}});
    }
    return attach_uvs(std::move(mesh), {
        {0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0},
        {2.0, 0.0}, {3.0, 1.0}, {2.0, 1.0}});
}

carto::project::ProjectDocument make_project(
    carto::journal::Journal& journal,
    carto::geometry::EditableMesh mesh,
    std::uint64_t& mesh_asset) {
    using namespace carto;
    auto document = project::ProjectDocument::create("UV Island Pack Test");
    REQUIRE(document);
    const std::string initial_snapshot = document.value().serialize();
    const auto* snapshot_bytes = reinterpret_cast<const std::uint8_t*>(initial_snapshot.data());
    REQUIRE(journal.append(journal::JournalAppend{
        core::Revision{}, document.value().revision(), "project.create",
        std::span<const std::uint8_t>(snapshot_bytes, initial_snapshot.size()), std::nullopt}));
    auto transaction = project::ProjectTransaction::begin(
        document.value(), journal, "uv-island-pack-tests");
    REQUIRE(transaction);
    const auto added = transaction.value().add_mesh(std::move(mesh));
    REQUIRE(added);
    mesh_asset = added.value();
    REQUIRE(transaction.value().commit("uv-island.fixture"));
    return std::move(document).value();
}

carto::production::UvIslandPackRecipe make_recipe(
    const carto::project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    std::uint32_t resolution = 128U,
    std::uint32_t padding = 2U,
    std::uint32_t maximum_udim_tiles = 1U) {
    const auto recipe = carto::production::make_uv_island_pack_recipe(
        document, mesh_asset, 7U, 8U, "Packed UV", "uv.packed.corners",
        resolution, padding, maximum_udim_tiles);
    REQUIRE(recipe);
    return recipe.value();
}

std::map<carto::geometry::FaceId, std::array<double, 4U>> face_bounds(
    const carto::production::UvIslandPackArtifact& artifact) {
    std::map<carto::geometry::FaceId, std::array<double, 4U>> bounds_by_face;
    for (const auto& corner : artifact.corners) {
        auto [bounds, inserted] = bounds_by_face.try_emplace(corner.face,
            std::array<double, 4U>{corner.uv.x, corner.uv.y, corner.uv.x, corner.uv.y});
        if (!inserted) {
            bounds->second[0U] = std::min(bounds->second[0U], corner.uv.x);
            bounds->second[1U] = std::min(bounds->second[1U], corner.uv.y);
            bounds->second[2U] = std::max(bounds->second[2U], corner.uv.x);
            bounds->second[3U] = std::max(bounds->second[3U], corner.uv.y);
        }
    }
    return bounds_by_face;
}

double area_equivalent_texels_per_unit(
    const carto::geometry::EditableMesh& mesh,
    const carto::production::UvIslandPackArtifact& artifact,
    carto::geometry::FaceId face_id) {
    using namespace carto;
    const geometry::Face* face = mesh.find_face(face_id);
    REQUIRE(face != nullptr);
    const auto topology = mesh.topology();
    REQUIRE(topology);
    std::map<std::pair<geometry::FaceId, geometry::VertexId>, geometry::CornerId>
        corner_by_face_vertex;
    for (const geometry::CornerRecord& corner : topology.value().corners) {
        corner_by_face_vertex.emplace(std::make_pair(corner.face, corner.vertex), corner.id);
    }
    std::map<geometry::CornerId, core::Vec2d> output_uv_by_corner;
    for (const production::UvIslandPackCorner& corner : artifact.corners) {
        if (corner.face == face_id) output_uv_by_corner.emplace(corner.corner, corner.uv);
    }
    REQUIRE(face->vertices.size() == 3U);
    std::array<core::Vec3d, 3U> positions{};
    std::array<core::Vec2d, 3U> uvs{};
    for (std::size_t index = 0U; index < 3U; ++index) {
        const geometry::Vertex* vertex = mesh.find_vertex(face->vertices[index]);
        REQUIRE(vertex != nullptr);
        positions[index] = vertex->position;
        const geometry::CornerId corner = corner_by_face_vertex.at(
            {face_id, face->vertices[index]});
        uvs[index] = output_uv_by_corner.at(corner);
    }
    const double world_area = core::cross(
        positions[1U] - positions[0U], positions[2U] - positions[0U]).length() * 0.5;
    const double uv_twice_area =
        (uvs[1U].x - uvs[0U].x) * (uvs[2U].y - uvs[0U].y) -
        (uvs[1U].y - uvs[0U].y) * (uvs[2U].x - uvs[0U].x);
    REQUIRE(world_area > 0.0);
    return static_cast<double>(artifact.recipe.resolution) *
        std::sqrt((std::abs(uv_twice_area) * 0.5) / world_area);
}

void disconnected_overlapping_islands_pack_and_publish_transactionally() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "uv-islands-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_overlapping_triangles(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const auto recipe = make_recipe(document, mesh_asset);
    REQUIRE(!recipe.serialize().empty());
    REQUIRE(recipe.settings_digest());

    const auto first = production::compile_uv_island_pack(recipe, source);
    const auto repeated = production::compile_uv_island_pack(recipe, source);
    REQUIRE(first && repeated);
    REQUIRE(first.value().corners == repeated.value().corners);
    REQUIRE(first.value().tiles == repeated.value().tiles);
    REQUIRE(first.value().settings_digest == repeated.value().settings_digest);
    REQUIRE(first.value().output_digest == repeated.value().output_digest);
    REQUIRE(first.value().artifact_digest == repeated.value().artifact_digest);
    REQUIRE(first.value().byte_size == 6U * 36U + 52U);
    REQUIRE(first.value().tiles.size() == 1U);
    REQUIRE(first.value().tiles.front().udim_tile == 1001U);
    REQUIRE(first.value().tiles.front().face_count == 2U);
    REQUIRE(first.value().validate(source));

    const auto bounds = face_bounds(first.value());
    REQUIRE(bounds.size() == 2U);
    const auto first_face = bounds.at(geometry::FaceId{10U});
    const auto second_face = bounds.at(geometry::FaceId{20U});
    const double gap_x = std::max({0.0,
        second_face[0U] - first_face[2U], first_face[0U] - second_face[2U]});
    const double gap_y = std::max({0.0,
        second_face[1U] - first_face[3U], first_face[1U] - second_face[3U]});
    const double required_gap =
        2.0 * static_cast<double>(recipe.padding_texels) / recipe.resolution;
    REQUIRE(gap_x >= required_gap || gap_y >= required_gap);

    auto tampered = first.value();
    tampered.corners.front().uv.x += 0.01;
    REQUIRE(tampered.refresh_digests());
    REQUIRE(!tampered.validate(source));

    journal::Journal other_journal(directory.path() / "other-project-journal.log");
    std::uint64_t other_mesh_asset = 0U;
    project::ProjectDocument other_document = make_project(
        other_journal, make_overlapping_triangles(), other_mesh_asset);
    REQUIRE(other_mesh_asset == mesh_asset);
    const core::Revision other_revision = other_document.revision();
    auto other_transaction = project::ProjectTransaction::begin(
        other_document, other_journal, "uv-island-pack-tests");
    REQUIRE(other_transaction);
    const auto cross_project_apply = production::apply_uv_island_pack(
        other_transaction.value(), other_mesh_asset, first.value());
    REQUIRE(!cross_project_apply);
    REQUIRE(cross_project_apply.error().code == core::ErrorCode::stale_data);
    REQUIRE(other_transaction.value().rollback());
    REQUIRE(other_document.revision() == other_revision);

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "uv-island-pack-tests");
    REQUIRE(transaction);
    REQUIRE(production::apply_uv_island_pack(
        transaction.value(), mesh_asset, first.value()));
    const auto& staged = transaction.value().staged_document().meshes().at(mesh_asset);
    const auto* packed_set = staged.uv_sets().find(8U);
    REQUIRE(packed_set != nullptr);
    REQUIRE(!packed_set->lightmap_candidate);
    REQUIRE(packed_set->tile_policy_version == 2U);
    REQUIRE(staged.uv_sets().active_for_render()->id == 7U);
    const std::string operation =
        production::uv_island_pack_transaction_operation(first.value());
    REQUIRE(operation.size() <= 128U);
    REQUIRE(transaction.value().commit(operation));
    const auto journal_entries = journal.read_all();
    REQUIRE(journal_entries && journal_entries.value().back().event_type == operation);

    REQUIRE(document.save_atomic(directory.path() / "packed.carto"));
    const auto reopened = project::ProjectDocument::load(directory.path() / "packed.carto");
    REQUIRE(reopened);
    const auto& reopened_mesh = reopened.value().meshes().at(mesh_asset);
    REQUIRE(reopened_mesh.uv_sets().find(8U) != nullptr);
    const auto* reopened_layer = reopened_mesh.attributes().find("uv.packed.corners");
    REQUIRE(reopened_layer != nullptr);
    const auto reopened_topology = reopened_mesh.topology();
    REQUIRE(reopened_topology);
    std::map<geometry::CornerId, core::Vec2d> expected_uvs;
    for (const auto& corner : first.value().corners) expected_uvs.emplace(corner.corner, corner.uv);
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
        document, journal, "uv-island-pack-tests");
    REQUIRE(stale_transaction);
    const auto stale_apply = production::apply_uv_island_pack(
        stale_transaction.value(), mesh_asset, first.value());
    REQUIRE(!stale_apply);
    REQUIRE(stale_apply.error().code == core::ErrorCode::stale_data);
    REQUIRE(stale_transaction.value().rollback());
    REQUIRE(document.revision() == before_stale_apply);
}

void world_area_density_policy_normalizes_uv_islands_deterministically() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "uv-density-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_density_mismatch_triangles(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const production::UvIslandPackRecipe preserve_recipe =
        make_recipe(document, mesh_asset);
    REQUIRE(preserve_recipe.density_policy ==
        production::UvIslandDensityPolicy::preserve_source_uv);
    const auto preserve = production::compile_uv_island_pack(preserve_recipe, source);
    REQUIRE(preserve);

    production::UvIslandPackRecipe uniform_recipe = preserve_recipe;
    uniform_recipe.density_policy = production::UvIslandDensityPolicy::uniform_world_area;
    REQUIRE(uniform_recipe.validate());
    const auto factory_uniform = production::make_uv_island_pack_recipe(
        document, mesh_asset, 7U, 8U, "Packed UV", "uv.packed.corners",
        128U, 2U, 1U, production::UvIslandDensityPolicy::uniform_world_area);
    REQUIRE(factory_uniform);
    REQUIRE(factory_uniform.value().serialize() == uniform_recipe.serialize());
    REQUIRE(uniform_recipe.serialize().find(
        "density.policy=uniform_world_area") != std::string::npos);
    const auto preserve_settings = preserve_recipe.settings_digest();
    const auto uniform_settings = uniform_recipe.settings_digest();
    REQUIRE(preserve_settings && uniform_settings);
    REQUIRE(preserve_settings.value() != uniform_settings.value());

    const auto uniform = production::compile_uv_island_pack(uniform_recipe, source);
    const auto repeated = production::compile_uv_island_pack(uniform_recipe, source);
    REQUIRE(uniform && repeated);
    REQUIRE(uniform.value().algorithm == "carto.uv.island-shelf-pack.v2");
    REQUIRE(uniform.value().validate(source));
    REQUIRE(uniform.value().output_digest == repeated.value().output_digest);
    REQUIRE(uniform.value().artifact_digest == repeated.value().artifact_digest);

    const double preserved_first = area_equivalent_texels_per_unit(
        source, preserve.value(), geometry::FaceId{10U});
    const double preserved_second = area_equivalent_texels_per_unit(
        source, preserve.value(), geometry::FaceId{20U});
    REQUIRE(std::abs(preserved_first / preserved_second - 2.0) < 1e-12);
    const double uniform_first = area_equivalent_texels_per_unit(
        source, uniform.value(), geometry::FaceId{10U});
    const double uniform_second = area_equivalent_texels_per_unit(
        source, uniform.value(), geometry::FaceId{20U});
    REQUIRE(std::abs(uniform_first - uniform_second) < uniform_first * 1e-12);
    REQUIRE(std::abs(uniform_first - uniform.value().pixels_per_uv_unit) <
        uniform_first * 1e-12);
    REQUIRE(uniform.value().texels_per_world_unit ==
        uniform.value().pixels_per_uv_unit);
    REQUIRE(preserve.value().texels_per_world_unit == 0.0);

    const double requested_density = uniform.value().texels_per_world_unit * 0.5;
    const auto factory_target = production::make_uv_island_pack_recipe(
        document, mesh_asset, 7U, 8U, "Packed UV", "uv.packed.corners",
        128U, 2U, 1U, production::UvIslandDensityPolicy::uniform_world_area,
        requested_density);
    REQUIRE(factory_target);
    production::UvIslandPackRecipe target_recipe = factory_target.value();
    REQUIRE(target_recipe.target_texels_per_world_unit == requested_density);
    REQUIRE(target_recipe.settings_digest().value() != uniform_settings.value());
    const auto targeted = production::compile_uv_island_pack(target_recipe, source);
    REQUIRE(targeted);
    REQUIRE(targeted.value().texels_per_world_unit == requested_density);
    REQUIRE(std::abs(area_equivalent_texels_per_unit(
        source, targeted.value(), geometry::FaceId{10U}) - requested_density) <
        requested_density * 1e-12);
    REQUIRE(std::abs(area_equivalent_texels_per_unit(
        source, targeted.value(), geometry::FaceId{20U}) - requested_density) <
        requested_density * 1e-12);

    auto unfit_density_recipe = target_recipe;
    unfit_density_recipe.target_texels_per_world_unit =
        uniform.value().texels_per_world_unit * 2.0;
    const auto unfit_density = production::compile_uv_island_pack(
        unfit_density_recipe, source);
    REQUIRE(!unfit_density);
    REQUIRE(unfit_density.error().code == core::ErrorCode::unsupported);

    auto invalid_policy = uniform_recipe;
    invalid_policy.density_policy =
        static_cast<production::UvIslandDensityPolicy>(0xffffffffU);
    REQUIRE(!invalid_policy.validate());
    auto invalid_preserve_target = preserve_recipe;
    invalid_preserve_target.target_texels_per_world_unit = 1.0;
    REQUIRE(!invalid_preserve_target.validate());
    auto non_finite_target = uniform_recipe;
    non_finite_target.target_texels_per_world_unit =
        std::numeric_limits<double>::infinity();
    REQUIRE(!non_finite_target.validate());

    journal::Journal degenerate_journal(
        directory.path() / "uv-density-degenerate-journal.log");
    std::uint64_t degenerate_asset = 0U;
    project::ProjectDocument degenerate_document = make_project(
        degenerate_journal, make_density_mismatch_triangles(true), degenerate_asset);
    auto degenerate_recipe = make_recipe(degenerate_document, degenerate_asset);
    degenerate_recipe.density_policy =
        production::UvIslandDensityPolicy::uniform_world_area;
    const auto degenerate = production::compile_uv_island_pack(
        degenerate_recipe, degenerate_document.meshes().at(degenerate_asset));
    REQUIRE(!degenerate);
    REQUIRE(degenerate.error().code == core::ErrorCode::validation_failed);

    auto transaction = project::ProjectTransaction::begin(
        document, journal, "uv-island-density-tests");
    REQUIRE(transaction);
    REQUIRE(production::apply_uv_island_pack(
        transaction.value(), mesh_asset, targeted.value()));
    const std::string operation =
        production::uv_island_pack_transaction_operation(targeted.value());
    REQUIRE(operation.rfind("uvp2.", 0U) == 0U);
    REQUIRE(transaction.value().commit(operation));
    REQUIRE(document.save_atomic(directory.path() / "target-density.carto"));
    const auto reopened = project::ProjectDocument::load(
        directory.path() / "target-density.carto");
    REQUIRE(reopened);
    const geometry::EditableMesh& reopened_mesh =
        reopened.value().meshes().at(mesh_asset);
    REQUIRE(reopened_mesh.uv_sets().find(target_recipe.output_uv_set) != nullptr);
    const auto* output_layer = reopened_mesh.attributes().find(
        target_recipe.output_layer_id);
    REQUIRE(output_layer != nullptr);
    const auto reopened_topology = reopened_mesh.topology();
    REQUIRE(reopened_topology);
    std::map<geometry::CornerId, core::Vec2d> expected;
    for (const production::UvIslandPackCorner& corner : targeted.value().corners) {
        expected.emplace(corner.corner, corner.uv);
    }
    REQUIRE(output_layer->values.size() == reopened_topology.value().corners.size());
    for (std::size_t index = 0U; index < output_layer->values.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&output_layer->values[index]);
        REQUIRE(uv != nullptr);
        const core::Vec2d wanted = expected.at(
            reopened_topology.value().corners[index].id);
        REQUIRE(uv->x == wanted.x && uv->y == wanted.y);
    }
}

void quarter_turn_rotation_improves_density_deterministically() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "uv-rotation-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_rotatable_islands(), mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const production::UvIslandPackRecipe unrotated_recipe = make_recipe(
        document, mesh_asset, 64U, 2U, 1U);
    REQUIRE(unrotated_recipe.serialize().find("rotation=disabled") !=
        std::string::npos);
    const auto unrotated = production::compile_uv_island_pack(unrotated_recipe, source);
    REQUIRE(unrotated);

    production::UvIslandPackRecipe rotated_recipe = unrotated_recipe;
    rotated_recipe.allow_quarter_turn_rotation = true;
    REQUIRE(rotated_recipe.validate());
    REQUIRE(rotated_recipe.serialize().find("rotation=quarter_turn_90") !=
        std::string::npos);
    REQUIRE(rotated_recipe.settings_digest().value() !=
        unrotated_recipe.settings_digest().value());
    const auto rotated = production::compile_uv_island_pack(rotated_recipe, source);
    REQUIRE(rotated);
    REQUIRE(rotated.value().validate(source));
    REQUIRE(rotated.value().pixels_per_uv_unit >
        unrotated.value().pixels_per_uv_unit * 1.2);

    const auto bounds = face_bounds(rotated.value());
    REQUIRE(bounds.size() == 2U);
    REQUIRE(rotated.value().tiles.size() == 1U);
    for (const auto& [face, bound] : bounds) {
        static_cast<void>(face);
        REQUIRE(bound[0U] >= 0.0 && bound[1U] >= 0.0);
        REQUIRE(bound[2U] <= 1.0 && bound[3U] <= 1.0);
    }
    const auto first = bounds.at(geometry::FaceId{10U});
    const auto second = bounds.at(geometry::FaceId{20U});
    const double gap_x = std::max({
        0.0, second[0U] - first[2U], first[0U] - second[2U]});
    const double gap_y = std::max({
        0.0, second[1U] - first[3U], first[1U] - second[3U]});
    REQUIRE(std::max(gap_x, gap_y) + 1e-12 >= 2.0 * 2U / 64.0);
    const auto repeated = production::compile_uv_island_pack(rotated_recipe, source);
    REQUIRE(repeated);
    REQUIRE(repeated.value().artifact_digest == rotated.value().artifact_digest);

    production::UvIslandPackRecipe uniform_rotated_recipe = rotated_recipe;
    uniform_rotated_recipe.density_policy =
        production::UvIslandDensityPolicy::uniform_world_area;
    const auto uniform_rotated = production::compile_uv_island_pack(
        uniform_rotated_recipe, source);
    REQUIRE(uniform_rotated);
    const double first_density = area_equivalent_texels_per_unit(
        source, uniform_rotated.value(), geometry::FaceId{10U});
    const double second_density = area_equivalent_texels_per_unit(
        source, uniform_rotated.value(), geometry::FaceId{20U});
    REQUIRE(std::abs(first_density - second_density) < first_density * 1e-12);
    REQUIRE(uniform_rotated.value().texels_per_world_unit ==
        uniform_rotated.value().pixels_per_uv_unit);

    production::UvIslandPackRecipe overpacked_recipe = rotated_recipe;
    overpacked_recipe.density_policy =
        production::UvIslandDensityPolicy::uniform_world_area;
    overpacked_recipe.target_texels_per_world_unit =
        rotated.value().pixels_per_uv_unit * 1.05;
    const auto overpacked = production::compile_uv_island_pack(overpacked_recipe, source);
    REQUIRE(!overpacked);
    REQUIRE(overpacked.error().code == core::ErrorCode::unsupported);
}

void shared_edge_identity_controls_island_connectivity() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal continuous_journal(directory.path() / "uv-continuous-journal.log");
    journal::Journal seam_journal(directory.path() / "uv-seam-journal.log");

    std::uint64_t continuous_asset = 0U;
    project::ProjectDocument continuous_document = make_project(
        continuous_journal, make_square(false), continuous_asset);
    const auto continuous = production::compile_uv_island_pack(
        make_recipe(continuous_document, continuous_asset),
        continuous_document.meshes().at(continuous_asset));
    REQUIRE(continuous);
    std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d> continuous_uvs;
    const auto continuous_topology =
        continuous_document.meshes().at(continuous_asset).topology();
    REQUIRE(continuous_topology);
    for (const auto& corner : continuous.value().corners) {
        const auto source_corner = std::find_if(
            continuous_topology.value().corners.begin(),
            continuous_topology.value().corners.end(),
            [&corner](const geometry::CornerRecord& value) { return value.id == corner.corner; });
        REQUIRE(source_corner != continuous_topology.value().corners.end());
        continuous_uvs.emplace(std::make_pair(source_corner->face, source_corner->vertex), corner.uv);
    }
    REQUIRE(continuous_uvs.at({geometry::FaceId{10U}, geometry::VertexId{1U}}).x ==
        continuous_uvs.at({geometry::FaceId{20U}, geometry::VertexId{1U}}).x);
    REQUIRE(continuous_uvs.at({geometry::FaceId{10U}, geometry::VertexId{3U}}).y ==
        continuous_uvs.at({geometry::FaceId{20U}, geometry::VertexId{3U}}).y);

    std::uint64_t seam_asset = 0U;
    project::ProjectDocument seam_document = make_project(
        seam_journal, make_square(true), seam_asset);
    const auto seam = production::compile_uv_island_pack(
        make_recipe(seam_document, seam_asset), seam_document.meshes().at(seam_asset));
    REQUIRE(seam);
    REQUIRE(seam.value().tiles.front().face_count == 2U);
    const auto seam_bounds = face_bounds(seam.value());
    const auto a = seam_bounds.at(geometry::FaceId{10U});
    const auto b = seam_bounds.at(geometry::FaceId{20U});
    const double gap_x = std::max({0.0, b[0U] - a[2U], a[0U] - b[2U]});
    const double gap_y = std::max({0.0, b[1U] - a[3U], a[1U] - b[3U]});
    REQUIRE(gap_x > 0.0 || gap_y > 0.0);
}

void stale_and_malformed_sources_fail_closed() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "uv-hostile-journal.log");
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(
        journal, make_overlapping_triangles(), mesh_asset);
    const auto recipe = make_recipe(document, mesh_asset);
    const geometry::EditableMesh& source = document.meshes().at(mesh_asset);
    const auto artifact = production::compile_uv_island_pack(recipe, source);
    REQUIRE(artifact);

    geometry::EditableMesh changed = source;
    REQUIRE(changed.set_vertex_position(geometry::VertexId{1U}, {-0.1, 0.0, 0.0}));
    const auto stale_validation = artifact.value().validate(changed);
    REQUIRE(!stale_validation);
    REQUIRE(stale_validation.error().code == core::ErrorCode::stale_data);

    auto missing_uv = recipe;
    missing_uv.source_uv_set = 99U;
    const auto missing = production::compile_uv_island_pack(missing_uv, source);
    REQUIRE(!missing);

    auto same_uv_identity = recipe;
    same_uv_identity.output_uv_set = same_uv_identity.source_uv_set;
    REQUIRE(!same_uv_identity.validate());

    auto invalid_budget = recipe;
    invalid_budget.maximum_udim_tiles = production::kMaxUvIslandPackTiles + 1U;
    REQUIRE(!invalid_budget.validate());

    geometry::EditableMesh degenerate_uv;
    REQUIRE(degenerate_uv.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}}}));
    degenerate_uv = attach_uvs(std::move(degenerate_uv), {
        {0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0}});
    const auto degenerate_binding =
        production::bind_bake_target("degenerate/mesh/1", degenerate_uv, 7U);
    REQUIRE(degenerate_binding);
    const production::UvIslandPackRecipe degenerate_recipe{
        "degenerate", 1U, degenerate_binding.value(), 7U, 8U, "Packed", "uv.packed",
        64U, 2U, 1U};
    const auto degenerate = production::compile_uv_island_pack(
        degenerate_recipe, degenerate_uv);
    REQUIRE(!degenerate);
    REQUIRE(degenerate.error().code == core::ErrorCode::validation_failed);
}

void udim_tiles_are_scheduled_contiguously_and_budgeted() {
    using namespace carto;
    TempDirectory directory;
    journal::Journal journal(directory.path() / "uv-udim-journal.log");
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {100.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 100.0, 0.0}},
         {geometry::VertexId{4U}, {200.0, 0.0, 0.0}},
         {geometry::VertexId{5U}, {201.0, 0.0, 0.0}},
         {geometry::VertexId{6U}, {200.0, 1.0, 0.0}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    mesh = attach_uvs(std::move(mesh), {
        {0.0, 0.0}, {100.0, 0.0}, {0.0, 100.0},
        {0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}});
    std::uint64_t mesh_asset = 0U;
    project::ProjectDocument document = make_project(journal, std::move(mesh), mesh_asset);
    const auto& source = document.meshes().at(mesh_asset);
    const auto two_tile_recipe = make_recipe(document, mesh_asset, 16U, 2U, 2U);
    const auto packed = production::compile_uv_island_pack(two_tile_recipe, source);
    REQUIRE(packed);
    REQUIRE(packed.value().tiles.size() == 2U);
    REQUIRE(packed.value().tiles[0U].udim_tile == 1001U);
    REQUIRE(packed.value().tiles[1U].udim_tile == 1002U);
    std::map<geometry::FaceId, std::uint32_t> face_tiles;
    for (const auto& corner : packed.value().corners) {
        const auto [tile, inserted] = face_tiles.emplace(corner.face, corner.udim_tile);
        REQUIRE(inserted || tile->second == corner.udim_tile);
        const std::uint32_t tile_index = corner.udim_tile - 1001U;
        REQUIRE(corner.uv.x >= static_cast<double>(tile_index % 10U));
        REQUIRE(corner.uv.x < static_cast<double>(tile_index % 10U) + 1.0);
        REQUIRE(corner.uv.y >= static_cast<double>(tile_index / 10U));
        REQUIRE(corner.uv.y < static_cast<double>(tile_index / 10U) + 1.0);
    }
    REQUIRE(face_tiles.at(geometry::FaceId{10U}) == 1001U);
    REQUIRE(face_tiles.at(geometry::FaceId{20U}) == 1002U);

    geometry::EditableMesh crowded;
    std::vector<geometry::Vertex> crowded_vertices;
    std::vector<geometry::Face> crowded_faces;
    std::vector<core::Vec2d> crowded_uvs;
    for (std::uint64_t chart = 0U; chart < 10U; ++chart) {
        const std::uint64_t first_vertex = chart * 3U + 1U;
        const double x = static_cast<double>(chart) * 3.0;
        crowded_vertices.push_back({geometry::VertexId{first_vertex}, {x, 0.0, 0.0}});
        crowded_vertices.push_back({geometry::VertexId{first_vertex + 1U}, {x + 1.0, 0.0, 0.0}});
        crowded_vertices.push_back({geometry::VertexId{first_vertex + 2U}, {x, 1.0, 0.0}});
        crowded_faces.push_back({geometry::FaceId{chart + 1U}, {
            geometry::VertexId{first_vertex}, geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
        crowded_uvs.insert(crowded_uvs.end(), {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}});
    }
    REQUIRE(crowded.insert_bulk(std::move(crowded_vertices), std::move(crowded_faces)));
    crowded = attach_uvs(std::move(crowded), std::move(crowded_uvs));
    const auto crowded_binding = production::bind_bake_target("crowded/mesh/1", crowded, 7U);
    REQUIRE(crowded_binding);
    const production::UvIslandPackRecipe one_tile_recipe{
        "crowded", 1U, crowded_binding.value(), 7U, 8U, "Packed", "uv.packed",
        16U, 2U, 1U};
    const auto unfit = production::compile_uv_island_pack(one_tile_recipe, crowded);
    REQUIRE(!unfit);
    REQUIRE(unfit.error().code == core::ErrorCode::unsupported);
}

void adversarial_island_count_fails_within_the_declared_bound() {
    using namespace carto;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    constexpr std::uint64_t chart_count = 5'001U;
    vertices.reserve(chart_count * 3U);
    faces.reserve(chart_count);
    std::vector<core::Vec2d> uvs;
    uvs.reserve(chart_count * 3U);
    for (std::uint64_t chart = 0U; chart < chart_count; ++chart) {
        const std::uint64_t first_vertex = chart * 3U + 1U;
        const double x = static_cast<double>(chart) * 2.0;
        vertices.push_back({geometry::VertexId{first_vertex}, {x, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 1U}, {x + 1.0, 0.0, 0.0}});
        vertices.push_back({geometry::VertexId{first_vertex + 2U}, {x, 1.0, 0.0}});
        faces.push_back({geometry::FaceId{chart + 1U}, {
            geometry::VertexId{first_vertex}, geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
        uvs.insert(uvs.end(), {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}});
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    mesh = attach_uvs(std::move(mesh), std::move(uvs));
    const auto source = production::bind_bake_target("bounded/mesh/1", mesh, 7U);
    REQUIRE(source);
    production::UvIslandPackRecipe recipe{
        "bounded", 1U, source.value(), 7U, 8U, "Packed", "uv.packed",
        64U, 2U, 100U};
    recipe.allow_quarter_turn_rotation = true;
    const auto compile = production::compile_uv_island_pack(recipe, mesh);
    REQUIRE(!compile);
    REQUIRE(compile.error().code == core::ErrorCode::unsupported);
    REQUIRE(compile.error().message.find("orientation candidate budget") !=
        std::string::npos);
}

} // namespace

int main() {
    try {
        disconnected_overlapping_islands_pack_and_publish_transactionally();
        std::cout << "PASS deterministic island packing, provenance, and transactional persistence\n";
        world_area_density_policy_normalizes_uv_islands_deterministically();
        std::cout << "PASS recipe-bound world-area texel density and save/reopen\n";
        quarter_turn_rotation_improves_density_deterministically();
        std::cout << "PASS opt-in quarter-turn UV packing is deterministic and density improving\n";
        shared_edge_identity_controls_island_connectivity();
        std::cout << "PASS shared-edge UV continuity and seam-aware island boundaries\n";
        stale_and_malformed_sources_fail_closed();
        std::cout << "PASS stale UV sources and malformed recipes fail closed\n";
        udim_tiles_are_scheduled_contiguously_and_budgeted();
        std::cout << "PASS deterministic UDIM scheduling and tile-budget rejection\n";
        adversarial_island_count_fails_within_the_declared_bound();
        std::cout << "PASS adversarial island workload fails within its bound\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL UV island pack tests: " << error.what() << '\n';
        return 1;
    }
}
