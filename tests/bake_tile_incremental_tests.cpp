#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/production/bake_graph.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

struct TempDirectory {
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-bake-padding-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

carto::geometry::EditableMesh make_source() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}}}));
    return mesh;
}

carto::geometry::EditableMesh make_source_pair() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {2.0, 0.0, 0.0}},
         {geometry::VertexId{5U}, {3.0, 0.0, 0.0}},
         {geometry::VertexId{6U}, {2.0, 1.0, 0.0}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    return mesh;
}

carto::geometry::EditableMesh make_thickness_source_pair() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.05}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.05}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.05}},
         {geometry::VertexId{4U}, {0.0, 0.0, -0.15}},
         {geometry::VertexId{5U}, {1.0, 0.0, -0.15}},
         {geometry::VertexId{6U}, {0.0, 1.0, -0.15}},
         {geometry::VertexId{7U}, {2.0, 0.0, 0.05}},
         {geometry::VertexId{8U}, {3.0, 0.0, 0.05}},
         {geometry::VertexId{9U}, {2.0, 1.0, 0.05}},
         {geometry::VertexId{10U}, {2.0, 0.0, -0.15}},
         {geometry::VertexId{11U}, {3.0, 0.0, -0.15}},
         {geometry::VertexId{12U}, {2.0, 1.0, -0.15}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{11U}, {geometry::VertexId{4U}, geometry::VertexId{6U},
             geometry::VertexId{5U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{7U}, geometry::VertexId{8U},
             geometry::VertexId{9U}}},
         {geometry::FaceId{21U}, {geometry::VertexId{10U}, geometry::VertexId{12U},
             geometry::VertexId{11U}}}}));
    return mesh;
}

carto::geometry::EditableMesh attach_uvs(
    carto::geometry::EditableMesh mesh,
    std::vector<carto::core::Vec2d> uvs);

carto::geometry::EditableMesh make_tiled_mesh(
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& tiles,
    double z) {
    using namespace carto;
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    std::vector<core::Vec2d> uvs;
    vertices.reserve(tiles.size() * 3U);
    faces.reserve(tiles.size());
    uvs.reserve(tiles.size() * 3U);
    for (std::size_t index = 0U; index < tiles.size(); ++index) {
        const std::uint64_t first_vertex =
            static_cast<std::uint64_t>(index) * 3U + 1U;
        const std::uint64_t face_id = static_cast<std::uint64_t>(index) + 1U;
        const double x = static_cast<double>(index) * 2.0;
        vertices.push_back({geometry::VertexId{first_vertex}, {x, 0.0, z}});
        vertices.push_back({geometry::VertexId{first_vertex + 1U}, {x + 1.0, 0.0, z}});
        vertices.push_back({geometry::VertexId{first_vertex + 2U}, {x, 1.0, z}});
        faces.push_back({geometry::FaceId{face_id}, {
            geometry::VertexId{first_vertex},
            geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
        const auto [tile_u, tile_v] = tiles[index];
        const double u = static_cast<double>(tile_u);
        const double v = static_cast<double>(tile_v);
        uvs.push_back({u + 0.05, v + 0.05});
        uvs.push_back({u + 0.9, v + 0.05});
        uvs.push_back({u + 0.05, v + 0.9});
    }
    REQUIRE(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    return attach_uvs(std::move(mesh), std::move(uvs));
}

carto::geometry::EditableMesh attach_uvs(
    carto::geometry::EditableMesh mesh,
    std::vector<carto::core::Vec2d> uvs) {
    using namespace carto;
    const auto topology = mesh.topology();
    REQUIRE(topology);
    REQUIRE(uvs.size() == topology.value().corners.size());
    std::vector<attributes::AttributeValue> values;
    values.reserve(uvs.size());
    for (const core::Vec2d uv : uvs) values.emplace_back(uv);
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, topology.value().corners.size()}});
    REQUIRE(attributes.add_layer(attributes::AttributeLayer{
        {"uv0.corners", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
        std::move(values)}));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add(attributes::UvSetDescriptor{
        7U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

carto::geometry::EditableMesh make_target(std::vector<carto::core::Vec2d> uvs) {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.1}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.1}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.1}},
         {geometry::VertexId{4U}, {2.0, 0.0, 0.1}},
         {geometry::VertexId{5U}, {3.0, 0.0, 0.1}},
         {geometry::VertexId{6U}, {2.0, 1.0, 0.1}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    return attach_uvs(std::move(mesh), std::move(uvs));
}

carto::geometry::EditableMesh make_close_target() {
    using namespace carto;
    geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.1}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.1}},
         {geometry::VertexId{3U}, {0.0, 1.0, 0.1}},
         {geometry::VertexId{4U}, {2.0, 0.0, 0.1}},
         {geometry::VertexId{5U}, {3.0, 0.0, 0.1}},
         {geometry::VertexId{6U}, {2.0, 1.0, 0.1}}},
        {{geometry::FaceId{10U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
             geometry::VertexId{3U}}},
         {geometry::FaceId{20U}, {geometry::VertexId{4U}, geometry::VertexId{5U},
             geometry::VertexId{6U}}}}));
    return attach_uvs(std::move(mesh), {
        {0.1, 0.1}, {0.45, 0.1}, {0.1, 0.45},
        {0.55, 0.1}, {0.9, 0.1}, {0.9, 0.45}});
}

std::vector<carto::core::Vec2d> initial_uvs() {
    return {{0.0, 0.0}, {0.9, 0.0}, {0.0, 0.9},
        {1.0, 0.0}, {1.9, 0.0}, {1.0, 0.9}};
}

carto::production::BakeRecipe make_recipe(
    const carto::geometry::EditableMesh& source,
    const carto::geometry::EditableMesh& target,
    std::uint32_t tile_u) {
    using namespace carto;
    const auto source_binding = production::bind_bake_source("scene/source", source);
    const auto target_binding = production::bind_bake_target("scene/target", target, 7U);
    REQUIRE(source_binding && target_binding);
    production::BakeRecipe recipe;
    recipe.identity = "bake/normal/incremental";
    recipe.source = source_binding.value();
    recipe.target = target_binding.value();
    recipe.target_uv_set = 7U;
    recipe.width = 64U;
    recipe.height = 64U;
    recipe.tile_u = tile_u;
    recipe.tile_v = 0U;
    recipe.normal_convention = production::BakeNormalConvention::opengl_y_plus;
    recipe.backface_policy = production::BakeBackfacePolicy::both_sides;
    recipe.projection_half_extent = 0.5;
    recipe.reject_projection_misses = true;
    REQUIRE(recipe.validate());
    return recipe;
}

void tile_uv_changes_invalidate_only_the_affected_uv_tile() {
    using namespace carto;
    const geometry::EditableMesh source = make_source();
    const geometry::EditableMesh original_target = make_target(initial_uvs());
    const production::BakeRecipe tile_zero_recipe =
        make_recipe(source, original_target, 0U);
    const production::BakeRecipe tile_one_recipe =
        make_recipe(source, original_target, 1U);
    const auto old_zero = production::make_bake_tile_dependency_key(
        tile_zero_recipe, source, original_target);
    const auto old_one = production::make_bake_tile_dependency_key(
        tile_one_recipe, source, original_target);
    REQUIRE(old_zero && old_one);
    REQUIRE(old_zero.value().target_tile_digest != old_one.value().target_tile_digest);
    auto forged_zero = old_zero.value();
    forged_zero.target_tile_digest = old_one.value().target_tile_digest;
    REQUIRE(!old_zero.value().same_dependencies(forged_zero));

    std::vector<core::Vec2d> changed_tile_one_uvs = initial_uvs();
    changed_tile_one_uvs[3U] = {1.1, 0.1};
    changed_tile_one_uvs[4U] = {1.8, 0.1};
    changed_tile_one_uvs[5U] = {1.1, 0.8};
    geometry::EditableMesh changed_target = attach_uvs(
        original_target, std::move(changed_tile_one_uvs));
    const auto new_zero_recipe = make_recipe(source, changed_target, 0U);
    const auto new_one_recipe = make_recipe(source, changed_target, 1U);
    const auto new_zero = production::make_bake_tile_dependency_key(
        new_zero_recipe, source, changed_target);
    const auto new_one = production::make_bake_tile_dependency_key(
        new_one_recipe, source, changed_target);
    REQUIRE(new_zero && new_one);
    REQUIRE(old_zero.value().target_revision != new_zero.value().target_revision);
    REQUIRE(old_zero.value().same_dependencies(new_zero.value()));
    REQUIRE(!old_one.value().same_dependencies(new_one.value()));
}

void geometry_and_settings_changes_invalidate_tile_reuse() {
    using namespace carto;
    geometry::EditableMesh source = make_source();
    const geometry::EditableMesh target = make_target(initial_uvs());
    const auto baseline_recipe = make_recipe(source, target, 0U);
    const auto baseline = production::make_bake_tile_dependency_key(
        baseline_recipe, source, target);
    REQUIRE(baseline);

    geometry::EditableMesh changed_source = source;
    REQUIRE(changed_source.set_vertex_position(
        geometry::VertexId{1U}, {-0.1, 0.0, 0.0}));
    const auto changed_source_recipe = make_recipe(changed_source, target, 0U);
    const auto source_changed = production::make_bake_tile_dependency_key(
        changed_source_recipe, changed_source, target);
    REQUIRE(source_changed);
    REQUIRE(!baseline.value().same_dependencies(source_changed.value()));

    geometry::EditableMesh changed_target_geometry = target;
    REQUIRE(changed_target_geometry.set_vertex_position(
        geometry::VertexId{4U}, {2.1, 0.0, 0.1}));
    const auto target_geometry_recipe =
        make_recipe(source, changed_target_geometry, 0U);
    const auto target_geometry_changed = production::make_bake_tile_dependency_key(
        target_geometry_recipe, source, changed_target_geometry);
    REQUIRE(target_geometry_changed);
    REQUIRE(!baseline.value().same_dependencies(target_geometry_changed.value()));

    auto changed_settings_recipe = baseline_recipe;
    changed_settings_recipe.width = 128U;
    changed_settings_recipe.height = 128U;
    const auto changed_settings = production::make_bake_tile_dependency_key(
        changed_settings_recipe, source, target);
    REQUIRE(changed_settings);
    REQUIRE(!baseline.value().same_dependencies(changed_settings.value()));

    const auto object_space = production::make_bake_tile_dependency_key(
        baseline_recipe, source, target, production::BakeNormalSpace::object);
    REQUIRE(object_space);
    REQUIRE(!baseline.value().same_dependencies(object_space.value()));
}

void cross_tile_faces_and_uncovered_tiles_are_not_cacheable() {
    using namespace carto;
    const geometry::EditableMesh source = make_source();
    std::vector<core::Vec2d> crossing_uvs{
        {0.8, 0.0}, {1.2, 0.0}, {0.8, 0.8},
        {1.0, 0.0}, {1.9, 0.0}, {1.0, 0.9}};
    const geometry::EditableMesh crossing_target = make_target(std::move(crossing_uvs));
    const auto crossing_recipe = make_recipe(source, crossing_target, 0U);
    const auto crossing = production::make_bake_tile_dependency_key(
        crossing_recipe, source, crossing_target);
    REQUIRE(!crossing);
    REQUIRE(crossing.error().code == core::ErrorCode::unsupported);

    const geometry::EditableMesh tile_zero_target = make_target(initial_uvs());
    const auto empty_recipe = make_recipe(source, tile_zero_target, 2U);
    const auto empty = production::make_bake_tile_dependency_key(
        empty_recipe, source, tile_zero_target);
    REQUIRE(!empty);
}

void cache_entry_reuses_only_verified_unchanged_tiles() {
    using namespace carto;
    const geometry::EditableMesh source = make_source();
    const geometry::EditableMesh original_target = make_target(initial_uvs());
    const production::BakeRecipe original_recipe = make_recipe(source, original_target, 0U);
    const auto original = production::compile_tangent_normal_map_cpu(
        original_recipe, source, original_target);
    REQUIRE(original.succeeded());
    REQUIRE(original.artifact.has_value());

    const auto entry = production::make_bake_tile_cache_entry(
        *original.artifact, source, original_target);
    REQUIRE(entry);
    REQUIRE(entry.value().validate());
    const auto serialized = entry.value().serialize();
    REQUIRE(serialized);
    const auto reopened = production::BakeTileArtifactCacheEntry::deserialize(
        serialized.value());
    REQUIRE(reopened);

    std::vector<core::Vec2d> changed_other_tile_uvs = initial_uvs();
    changed_other_tile_uvs[3U] = {1.1, 0.1};
    changed_other_tile_uvs[4U] = {1.8, 0.1};
    changed_other_tile_uvs[5U] = {1.1, 0.8};
    const geometry::EditableMesh changed_other_tile_target = attach_uvs(
        original_target, std::move(changed_other_tile_uvs));
    const production::BakeRecipe current_recipe = make_recipe(
        source, changed_other_tile_target, 0U);
    const auto reused = production::reuse_bake_tile_artifact(
        reopened.value(), current_recipe, source, changed_other_tile_target);
    REQUIRE(reused);
    REQUIRE(reused.value().validate());
    REQUIRE(reused.value().recipe == current_recipe);
    REQUIRE(reused.value().rgba8 == original.artifact->rgba8);
    REQUIRE(reused.value().output_digest == original.artifact->output_digest);
    REQUIRE(reused.value().artifact_digest != original.artifact->artifact_digest);

    const auto cold = production::compile_tangent_normal_map_cpu(
        current_recipe, source, changed_other_tile_target);
    REQUIRE(cold.succeeded());
    REQUIRE(cold.artifact.has_value());
    REQUIRE(cold.artifact->rgba8 == reused.value().rgba8);
    REQUIRE(cold.artifact->output_digest == reused.value().output_digest);

    auto corrupted_entry = reopened.value();
    corrupted_entry.dependencies.target_geometry_digest.bytes[0U] ^= 1U;
    REQUIRE(!production::reuse_bake_tile_artifact(
        corrupted_entry, current_recipe, source, changed_other_tile_target));
    auto corrupted_bytes = serialized.value();
    corrupted_bytes.back() ^= 1U;
    REQUIRE(!production::BakeTileArtifactCacheEntry::deserialize(corrupted_bytes));
}

void cache_entry_rejects_changed_dependencies_and_tampered_pixels() {
    using namespace carto;
    const geometry::EditableMesh source = make_source();
    const geometry::EditableMesh target = make_target(initial_uvs());
    const production::BakeRecipe recipe = make_recipe(source, target, 0U);
    const auto baked = production::compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(baked.succeeded() && baked.artifact.has_value());
    const auto entry = production::make_bake_tile_cache_entry(
        *baked.artifact, source, target);
    REQUIRE(entry);

    std::vector<core::Vec2d> changed_same_tile_uvs = initial_uvs();
    changed_same_tile_uvs[1U] = {0.8, 0.0};
    const geometry::EditableMesh changed_uv_target = attach_uvs(
        target, std::move(changed_same_tile_uvs));
    const auto changed_uv_recipe = make_recipe(source, changed_uv_target, 0U);
    const auto changed_uv_reuse = production::reuse_bake_tile_artifact(
        entry.value(), changed_uv_recipe, source, changed_uv_target);
    REQUIRE(!changed_uv_reuse);
    REQUIRE(changed_uv_reuse.error().code == core::ErrorCode::stale_data);

    geometry::EditableMesh changed_source = source;
    REQUIRE(changed_source.set_vertex_position(
        geometry::VertexId{1U}, {-0.1, 0.0, 0.0}));
    const auto changed_source_recipe = make_recipe(changed_source, target, 0U);
    const auto changed_source_reuse = production::reuse_bake_tile_artifact(
        entry.value(), changed_source_recipe, changed_source, target);
    REQUIRE(!changed_source_reuse);
    REQUIRE(changed_source_reuse.error().code == core::ErrorCode::stale_data);

    geometry::EditableMesh changed_target_geometry = target;
    REQUIRE(changed_target_geometry.set_vertex_position(
        geometry::VertexId{4U}, {2.1, 0.0, 0.1}));
    const auto changed_geometry_recipe = make_recipe(
        source, changed_target_geometry, 0U);
    const auto changed_geometry_reuse = production::reuse_bake_tile_artifact(
        entry.value(), changed_geometry_recipe, source, changed_target_geometry);
    REQUIRE(!changed_geometry_reuse);
    REQUIRE(changed_geometry_reuse.error().code == core::ErrorCode::stale_data);

    auto changed_settings_recipe = recipe;
    changed_settings_recipe.width = 128U;
    changed_settings_recipe.height = 128U;
    const auto changed_settings_reuse = production::reuse_bake_tile_artifact(
        entry.value(), changed_settings_recipe, source, target);
    REQUIRE(!changed_settings_reuse);
    REQUIRE(changed_settings_reuse.error().code == core::ErrorCode::stale_data);

    auto damaged_artifact = entry.value();
    damaged_artifact.artifact.rgba8[0U] ^= 1U;
    REQUIRE(!damaged_artifact.validate());
    REQUIRE(!production::reuse_bake_tile_artifact(
        damaged_artifact, recipe, source, target));
}

void tile_schedule_compiles_all_tiles_in_stable_order() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    const geometry::EditableMesh target = make_target(initial_uvs());
    const production::BakeRecipe recipe = make_recipe(source, target, 0U);
    const auto tiles = production::enumerate_bake_tiles(target, 7U);
    REQUIRE(tiles);
    REQUIRE((tiles.value() == std::vector<production::BakeTileCoordinate>{
        {0U, 0U}, {1U, 0U}}));

    const auto first = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::tangent, source, target);
    REQUIRE(first);
    REQUIRE(first.value().entries.size() == 2U);
    REQUIRE(first.value().compiled_tiles == 2U);
    REQUIRE(first.value().reused_tiles == 0U);
    for (std::size_t index = 0U; index < first.value().entries.size(); ++index) {
        REQUIRE(first.value().entries[index].tile == tiles.value()[index]);
        REQUIRE(first.value().entries[index].artifact.validate());
    }

    const auto repeated = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::tangent, source, target);
    REQUIRE(repeated);
    REQUIRE(repeated.value().entries.size() == first.value().entries.size());
    for (std::size_t index = 0U; index < first.value().entries.size(); ++index) {
        REQUIRE(repeated.value().entries[index].artifact.artifact_digest ==
            first.value().entries[index].artifact.artifact_digest);
    }

    const auto object_space = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::object, source, target);
    REQUIRE(object_space);
    REQUIRE(object_space.value().compiled_tiles == 2U);
    REQUIRE(object_space.value().reused_tiles == 0U);
    for (const production::BakeTileScheduleEntry& entry : object_space.value().entries) {
        REQUIRE(entry.artifact.normal_space == production::BakeNormalSpace::object);
        REQUIRE(entry.artifact.validate());
    }
}

void tile_enumeration_orders_both_axes_and_enforces_bounds() {
    using namespace carto;
    const geometry::EditableMesh unsorted_target = make_tiled_mesh(
        {{1U, 0U}, {0U, 1U}, {0U, 0U}}, 0.1);
    const auto ordered = production::enumerate_bake_tiles(unsorted_target, 7U);
    REQUIRE(ordered);
    REQUIRE((ordered.value() == std::vector<production::BakeTileCoordinate>{
        {0U, 0U}, {0U, 1U}, {1U, 0U}}));

    const geometry::EditableMesh outside_udim = make_tiled_mesh({{10U, 0U}}, 0.1);
    const auto outside = production::enumerate_bake_tiles(outside_udim, 7U);
    REQUIRE(!outside);
    REQUIRE(outside.error().code == core::ErrorCode::validation_failed);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> too_many_tiles;
    too_many_tiles.reserve(production::kMaxBakeTileScheduleTiles + 1U);
    for (std::uint32_t index = 0U;
         index <= production::kMaxBakeTileScheduleTiles;
         ++index) {
        too_many_tiles.emplace_back(index % 10U, index / 10U);
    }
    const geometry::EditableMesh oversized_target = make_tiled_mesh(
        too_many_tiles, 0.1);
    const auto oversized = production::enumerate_bake_tiles(oversized_target, 7U);
    REQUIRE(!oversized);
    REQUIRE(oversized.error().code == core::ErrorCode::unsupported);
}

void tile_schedule_reuses_only_matching_cached_tiles() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    const geometry::EditableMesh original_target = make_target(initial_uvs());
    const production::BakeRecipe original_recipe = make_recipe(
        source, original_target, 0U);
    const auto original = production::compile_tangent_normal_map_cpu(
        original_recipe, source, original_target);
    REQUIRE(original.succeeded() && original.artifact.has_value());
    const auto cache = production::make_bake_tile_cache_entry(
        *original.artifact, source, original_target);
    REQUIRE(cache);

    std::vector<core::Vec2d> changed_tile_one_uvs = initial_uvs();
    changed_tile_one_uvs[3U] = {1.1, 0.1};
    changed_tile_one_uvs[4U] = {1.8, 0.1};
    changed_tile_one_uvs[5U] = {1.1, 0.8};
    const geometry::EditableMesh changed_target = attach_uvs(
        original_target, std::move(changed_tile_one_uvs));
    const production::BakeRecipe current_recipe = make_recipe(
        source, changed_target, 0U);
    const std::array<production::BakeTileArtifactCacheEntry, 1U> reusable{
        cache.value()};
    const auto scheduled = production::schedule_normal_map_tiles_cpu(
        current_recipe, production::BakeNormalSpace::tangent, source,
        changed_target, reusable);
    REQUIRE(scheduled);
    REQUIRE(scheduled.value().entries.size() == 2U);
    REQUIRE(scheduled.value().reused_tiles == 1U);
    REQUIRE(scheduled.value().compiled_tiles == 1U);
    REQUIRE((scheduled.value().entries[0U].tile ==
        production::BakeTileCoordinate{0U, 0U}));
    REQUIRE(scheduled.value().entries[0U].reused_from_cache);
    REQUIRE(!scheduled.value().entries[1U].reused_from_cache);

    const auto cold = production::schedule_normal_map_tiles_cpu(
        current_recipe, production::BakeNormalSpace::tangent, source, changed_target);
    REQUIRE(cold);
    for (std::size_t index = 0U; index < cold.value().entries.size(); ++index) {
        REQUIRE(scheduled.value().entries[index].artifact.rgba8 ==
            cold.value().entries[index].artifact.rgba8);
        REQUIRE(scheduled.value().entries[index].artifact.output_digest ==
            cold.value().entries[index].artifact.output_digest);
        REQUIRE(scheduled.value().entries[index].artifact.artifact_digest ==
            cold.value().entries[index].artifact.artifact_digest);
    }
}

void tile_schedule_rejects_cross_tile_faces_and_partial_failure() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    std::vector<core::Vec2d> crossing_uvs{
        {0.8, 0.0}, {1.2, 0.0}, {0.8, 0.8},
        {1.0, 0.0}, {1.9, 0.0}, {1.0, 0.9}};
    const geometry::EditableMesh crossing_target = make_target(std::move(crossing_uvs));
    const auto crossing = production::enumerate_bake_tiles(crossing_target, 7U);
    REQUIRE(!crossing);
    REQUIRE(crossing.error().code == core::ErrorCode::unsupported);

    const geometry::EditableMesh incomplete_source = make_source();
    const geometry::EditableMesh target = make_target(initial_uvs());
    const production::BakeRecipe recipe = make_recipe(
        incomplete_source, target, 0U);
    const auto partial_failure = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::tangent, incomplete_source, target);
    REQUIRE(!partial_failure);
    REQUIRE(partial_failure.error().code == core::ErrorCode::validation_failed);
    REQUIRE(partial_failure.error().context.size() == 1U);
    REQUIRE(partial_failure.error().context.front() == "UDIM tile 1,0");
}

void tile_schedule_rejects_a_corrupt_matching_cache_entry() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    const geometry::EditableMesh target = make_target(initial_uvs());
    const production::BakeRecipe recipe = make_recipe(source, target, 0U);
    const auto tile_zero = production::compile_tangent_normal_map_cpu(
        recipe, source, target);
    REQUIRE(tile_zero.succeeded() && tile_zero.artifact.has_value());
    const auto cache = production::make_bake_tile_cache_entry(
        *tile_zero.artifact, source, target);
    REQUIRE(cache);
    const std::array<production::BakeTileArtifactCacheEntry, 2U> duplicate_cache{
        cache.value(), cache.value()};
    const auto duplicate = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::tangent, source, target, duplicate_cache);
    REQUIRE(!duplicate);
    REQUIRE(duplicate.error().code == core::ErrorCode::invalid_argument);

    auto corrupted = cache.value();
    corrupted.artifact.rgba8[0U] ^= 0x01U;
    const std::array<production::BakeTileArtifactCacheEntry, 1U> reusable{
        std::move(corrupted)};
    const auto scheduled = production::schedule_normal_map_tiles_cpu(
        recipe, production::BakeNormalSpace::tangent, source, target, reusable);
    REQUIRE(!scheduled);
    REQUIRE(scheduled.error().code == core::ErrorCode::validation_failed);
}

void seam_safe_padding_is_deterministic_serializable_and_publishable() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    const geometry::EditableMesh target = make_close_target();
    const production::BakeRecipe bake_recipe = make_recipe(source, target, 0U);
    const auto bake = production::compile_tangent_normal_map_cpu(
        bake_recipe, source, target);
    REQUIRE(bake.succeeded() && bake.artifact.has_value());

    production::NormalMapArtifact input = *bake.artifact;
    constexpr std::array<std::uint8_t, 3U> left_color{20U, 40U, 60U};
    constexpr std::array<std::uint8_t, 3U> right_color{170U, 180U, 190U};
    for (std::size_t pixel = 0U; pixel < input.rgba8.size(); pixel += 4U) {
        if (input.rgba8[pixel + 3U] != 255U) continue;
        const std::array<std::uint8_t, 3U>& color =
            (pixel / 4U) % input.recipe.width < 32U ? left_color : right_color;
        std::copy(color.begin(), color.end(), input.rgba8.begin() +
            static_cast<std::ptrdiff_t>(pixel));
    }
    REQUIRE(input.refresh_digests());
    const auto recipe = production::make_bake_padding_recipe(input, 3U);
    REQUIRE(recipe);
    const auto padded = production::compile_bake_padding_cpu(
        recipe.value(), input, source, target);
    REQUIRE(padded);
    REQUIRE(padded.value().validate());
    REQUIRE(padded.value().padded_texels > 0U);

    std::uint64_t left_padded = 0U;
    std::uint64_t right_padded = 0U;
    for (std::size_t pixel = 0U; pixel < input.rgba8.size(); pixel += 4U) {
        REQUIRE(padded.value().rgba8[pixel + 3U] == input.rgba8[pixel + 3U]);
        if (input.rgba8[pixel + 3U] == 255U) {
            REQUIRE(std::equal(input.rgba8.begin() + static_cast<std::ptrdiff_t>(pixel),
                input.rgba8.begin() + static_cast<std::ptrdiff_t>(pixel + 4U),
                padded.value().rgba8.begin() + static_cast<std::ptrdiff_t>(pixel)));
            continue;
        }
        const bool is_left = std::equal(left_color.begin(), left_color.end(),
            padded.value().rgba8.begin() + static_cast<std::ptrdiff_t>(pixel));
        const bool is_right = std::equal(right_color.begin(), right_color.end(),
            padded.value().rgba8.begin() + static_cast<std::ptrdiff_t>(pixel));
        const std::size_t x = (pixel / 4U) % input.recipe.width;
        if (is_left) {
            REQUIRE(x < 32U);
            ++left_padded;
        }
        if (is_right) {
            REQUIRE(x >= 32U);
            ++right_padded;
        }
    }
    REQUIRE(left_padded > 0U);
    REQUIRE(right_padded > 0U);

    const auto repeated = production::compile_bake_padding_cpu(
        recipe.value(), input, source, target);
    REQUIRE(repeated);
    REQUIRE(repeated.value().artifact_digest == padded.value().artifact_digest);
    const auto serialized = padded.value().serialize();
    REQUIRE(serialized);
    const auto reopened = production::BakePaddingArtifact::deserialize(serialized.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == padded.value().artifact_digest);
    REQUIRE(production::validate_bake_padding_sources(
        reopened.value(), input, source, target));

    auto stale_target = target;
    REQUIRE(stale_target.set_vertex_position(
        geometry::VertexId{1U}, {-0.05, 0.0, 0.1}));
    const auto stale_result = production::compile_bake_padding_cpu(
        recipe.value(), input, source, stale_target);
    REQUIRE(!stale_result);
    REQUIRE(stale_result.error().code == core::ErrorCode::stale_data);

    production::NormalMapArtifact changed_input = input;
    std::size_t changed_pixel = changed_input.rgba8.size();
    for (std::size_t pixel = 0U; pixel < changed_input.rgba8.size(); pixel += 4U) {
        if (changed_input.rgba8[pixel + 3U] == 255U) {
            changed_pixel = pixel;
            break;
        }
    }
    REQUIRE(changed_pixel < changed_input.rgba8.size());
    changed_input.rgba8[changed_pixel] ^= 1U;
    REQUIRE(changed_input.refresh_digests());
    const auto mismatched_input = production::compile_bake_padding_cpu(
        recipe.value(), changed_input, source, target);
    REQUIRE(!mismatched_input);
    REQUIRE(mismatched_input.error().code == core::ErrorCode::stale_data);

    auto tampered = padded.value();
    tampered.rgba8[0U] ^= 1U;
    REQUIRE(!tampered.validate());
    auto corrupt_bytes = serialized.value();
    corrupt_bytes.back() ^= 1U;
    REQUIRE(!production::BakePaddingArtifact::deserialize(corrupt_bytes));

    const auto invalid_zero = production::make_bake_padding_recipe(input, 0U);
    const auto invalid_large = production::make_bake_padding_recipe(
        input, production::kMaxBakePaddingTexels + 1U);
    REQUIRE(!invalid_zero);
    REQUIRE(!invalid_large);

    const auto cook_key = production::bake_padding_cook_key(recipe.value());
    REQUIRE(cook_key);
    production::ProductionCookGraph graph;
    const auto registered = production::register_bake_padding_graph_node(
        graph, recipe.value());
    REQUIRE(registered && registered.value() == cook_key.value());
    REQUIRE(graph.validate());
    const auto input_key = production::bake_production_cook_key(
        bake_recipe, production::BakeNormalSpace::tangent);
    REQUIRE(input_key);
    REQUIRE(graph.nodes().size() == 1U);
    REQUIRE(std::find(graph.nodes().front().dependencies.begin(),
        graph.nodes().front().dependencies.end(), input_key.value()) !=
        graph.nodes().front().dependencies.end());

    const auto publish_recipe = production::make_bake_padding_recipe(*bake.artifact, 3U);
    REQUIRE(publish_recipe);
    const auto publish_artifact = production::compile_bake_padding_cpu(
        publish_recipe.value(), *bake.artifact, source, target);
    REQUIRE(publish_artifact);
    const auto publish_key = production::bake_padding_cook_key(publish_recipe.value());
    REQUIRE(publish_key);
    TempDirectory directory;
    assets::BlobStore blobs(directory.path() / "blobs");
    production::ProductionArtifactStore store(blobs);
    const auto payload = publish_artifact.value().serialize();
    REQUIRE(payload);
    production::ProductionArtifactReceipt receipt;
    receipt.key = publish_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    production::AdmissionPolicy policy;
    REQUIRE(production::publish_bake_padding(
        store, receipt, publish_artifact.value(), *bake.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    auto forged_output = publish_artifact.value();
    forged_output.rgba8[0U] ^= 1U;
    REQUIRE(forged_output.refresh_digests());
    REQUIRE(!production::publish_bake_padding(
        store, receipt, forged_output, *bake.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    const auto stored = production::read_bake_padding(
        store, publish_recipe.value(), *bake.artifact, source, target, policy);
    REQUIRE(stored);
    REQUIRE(stored.value().artifact_digest == publish_artifact.value().artifact_digest);
    const auto stale_publish = production::publish_bake_padding(
        store, receipt, publish_artifact.value(), *bake.artifact, source, stale_target, policy);
    REQUIRE(!stale_publish);
    REQUIRE(store.ledger().size() == 1U);
}

template <typename MapArtifact>
carto::production::BakePaddingArtifact require_typed_bake_padding(
    const MapArtifact& input,
    carto::production::BakePaddingInputKind expected_kind,
    const carto::geometry::EditableMesh& source,
    const carto::geometry::EditableMesh& target) {
    using namespace carto;
    const production::BakePaddingInputRef input_ref{std::cref(input)};
    const auto recipe = production::make_bake_padding_recipe(input_ref, 2U);
    REQUIRE(recipe);
    REQUIRE(recipe.value().input_kind == expected_kind);
    const auto padded = production::compile_bake_padding_cpu(
        recipe.value(), input_ref, source, target);
    REQUIRE(padded);
    REQUIRE(padded.value().validate());
    REQUIRE(padded.value().padded_texels > 0U);
    for (std::size_t pixel = 0U; pixel < input.rgba8.size(); pixel += 4U) {
        REQUIRE(padded.value().rgba8[pixel + 3U] == input.rgba8[pixel + 3U]);
        if (input.rgba8[pixel + 3U] == 255U) {
            REQUIRE(std::equal(input.rgba8.begin() + static_cast<std::ptrdiff_t>(pixel),
                input.rgba8.begin() + static_cast<std::ptrdiff_t>(pixel + 4U),
                padded.value().rgba8.begin() + static_cast<std::ptrdiff_t>(pixel)));
        }
    }
    const auto repeated = production::compile_bake_padding_cpu(
        recipe.value(), input_ref, source, target);
    REQUIRE(repeated);
    REQUIRE(repeated.value().artifact_digest == padded.value().artifact_digest);
    const auto bytes = padded.value().serialize();
    REQUIRE(bytes);
    const auto reopened = production::BakePaddingArtifact::deserialize(bytes.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().recipe.input_kind == expected_kind);
    REQUIRE(reopened.value().artifact_digest == padded.value().artifact_digest);
    auto corrupted = bytes.value();
    corrupted.back() ^= 1U;
    REQUIRE(!production::BakePaddingArtifact::deserialize(corrupted));
    return padded.value();
}

void typed_rgba8_bake_padding_preserves_provenance_and_publication() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    const geometry::EditableMesh target = make_close_target();
    const production::BakeRecipe binding = make_recipe(source, target, 0U);

    production::AmbientOcclusionRecipe ao_recipe;
    ao_recipe.binding = binding;
    ao_recipe.sample_count = 4U;
    ao_recipe.max_distance = 0.5;
    const auto ao_report = production::compile_ambient_occlusion_cpu(
        ao_recipe, source, target);
    REQUIRE(ao_report.succeeded() && ao_report.artifact.has_value());
    const production::AmbientOcclusionMapArtifact ao = *ao_report.artifact;
    const auto ao_padding = require_typed_bake_padding(
        ao, production::BakePaddingInputKind::ambient_occlusion, source, target);

    const auto normal_report = production::compile_tangent_normal_map_cpu(
        binding, source, target);
    REQUIRE(normal_report.succeeded() && normal_report.artifact.has_value());
    const auto ao_padding_recipe = production::make_bake_padding_recipe(
        production::BakePaddingInputRef{std::cref(ao)}, 2U);
    REQUIRE(ao_padding_recipe);
    auto invalid_kind_recipe = ao_padding_recipe.value();
    invalid_kind_recipe.input_kind =
        static_cast<production::BakePaddingInputKind>(99U);
    REQUIRE(!invalid_kind_recipe.validate());
    auto invalid_normal_space_recipe = ao_padding_recipe.value();
    invalid_normal_space_recipe.input_normal_space = production::BakeNormalSpace::object;
    REQUIRE(!invalid_normal_space_recipe.validate());
    const auto wrong_kind = production::compile_bake_padding_cpu(
        ao_padding_recipe.value(),
        production::BakePaddingInputRef{std::cref(*normal_report.artifact)},
        source, target);
    REQUIRE(!wrong_kind);
    REQUIRE(wrong_kind.error().code == core::ErrorCode::stale_data);

    production::AmbientOcclusionMapArtifact changed_ao = ao;
    std::size_t alpha_index = 3U;
    while (alpha_index < changed_ao.rgba8.size() &&
        changed_ao.rgba8[alpha_index] != 255U) alpha_index += 4U;
    REQUIRE(alpha_index < changed_ao.rgba8.size());
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        changed_ao.rgba8[alpha_index - 3U + channel] ^= 1U;
    }
    REQUIRE(changed_ao.refresh_digests());
    const auto stale_input = production::compile_bake_padding_cpu(
        ao_padding_recipe.value(),
        production::BakePaddingInputRef{std::cref(changed_ao)}, source, target);
    REQUIRE(!stale_input);
    REQUIRE(stale_input.error().code == core::ErrorCode::stale_data);

    production::AmbientOcclusionRecipe bent_recipe = ao_recipe;
    const auto bent_report = production::compile_bent_normal_cpu(
        bent_recipe, source, target);
    REQUIRE(bent_report.succeeded() && bent_report.artifact.has_value());
    (void)require_typed_bake_padding(*bent_report.artifact,
        production::BakePaddingInputKind::bent_normal, source, target);

    production::CurvatureRecipe curvature_recipe;
    curvature_recipe.binding = binding;
    const auto curvature_report = production::compile_curvature_cpu(
        curvature_recipe, source, target);
    REQUIRE(curvature_report.succeeded() && curvature_report.artifact.has_value());
    (void)require_typed_bake_padding(*curvature_report.artifact,
        production::BakePaddingInputKind::curvature, source, target);

    const geometry::EditableMesh thickness_source = make_thickness_source_pair();
    production::ThicknessRecipe thickness_recipe;
    thickness_recipe.binding = make_recipe(thickness_source, target, 0U);
    thickness_recipe.maximum_distance = 0.5;
    const auto thickness_report = production::compile_thickness_cpu(
        thickness_recipe, thickness_source, target);
    REQUIRE(thickness_report.succeeded() && thickness_report.artifact.has_value());
    (void)require_typed_bake_padding(*thickness_report.artifact,
        production::BakePaddingInputKind::thickness, thickness_source, target);

    production::HeightDisplacementRecipe height_recipe;
    height_recipe.binding = binding;
    height_recipe.normalization_distance = 0.25;
    const auto height_report = production::compile_height_displacement_cpu(
        height_recipe, source, target);
    REQUIRE(height_report.succeeded() && height_report.artifact.has_value());
    (void)require_typed_bake_padding(*height_report.artifact,
        production::BakePaddingInputKind::height_displacement, source, target);

    const auto ao_key = production::bake_ambient_occlusion_cook_key(ao_recipe);
    REQUIRE(ao_key);
    production::ProductionCookGraph missing_dependency_graph;
    REQUIRE(!production::register_bake_padding_graph_node(
        missing_dependency_graph, ao_padding_recipe.value()));
    auto wrong_scope_key = ao_key.value();
    wrong_scope_key.spatial_scope += ";other-target";
    production::ProductionCookGraph wrong_dependency_graph;
    REQUIRE(!production::register_bake_padding_graph_node(
        wrong_dependency_graph, ao_padding_recipe.value(), {wrong_scope_key}));
    production::ProductionCookGraph graph;
    const auto padding_key = production::register_bake_padding_graph_node(
        graph, ao_padding_recipe.value(), {ao_key.value()});
    if (!padding_key) std::cerr << padding_key.error().message << '\n';
    REQUIRE(padding_key);
    REQUIRE(graph.validate());
    REQUIRE(graph.nodes().size() == 1U);
    REQUIRE(std::find(graph.nodes().front().dependencies.begin(),
        graph.nodes().front().dependencies.end(), ao_key.value()) !=
        graph.nodes().front().dependencies.end());

    TempDirectory directory;
    assets::BlobStore blobs(directory.path() / "typed-padding-blobs");
    production::ProductionArtifactStore store(blobs);
    const auto payload = ao_padding.serialize();
    REQUIRE(payload);
    production::ProductionArtifactReceipt receipt;
    receipt.key = padding_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    production::AdmissionPolicy policy;
    REQUIRE(!production::publish_bake_padding(store, receipt, ao_padding,
        production::BakePaddingInputRef{std::cref(changed_ao)}, source, target, policy));
    REQUIRE(store.ledger().size() == 0U);
    REQUIRE(production::publish_bake_padding(store, receipt, ao_padding,
        production::BakePaddingInputRef{std::cref(ao)}, source, target, policy));
    const auto reopened = production::read_bake_padding(store,
        ao_padding_recipe.value(), production::BakePaddingInputRef{std::cref(ao)},
        source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().recipe.input_kind ==
        production::BakePaddingInputKind::ambient_occlusion);
    REQUIRE(reopened.value().artifact_digest == ao_padding.artifact_digest);
}

void seam_safe_padding_rejects_overlapping_and_cross_tile_uvs() {
    using namespace carto;
    const geometry::EditableMesh source = make_source_pair();
    std::vector<core::Vec2d> overlapping_uvs{
        {0.1, 0.1}, {0.8, 0.1}, {0.1, 0.8},
        {0.1, 0.1}, {0.8, 0.1}, {0.1, 0.8}};
    const geometry::EditableMesh overlapping_target = make_target(
        std::move(overlapping_uvs));
    const production::BakeRecipe overlapping_recipe = make_recipe(
        source, overlapping_target, 0U);
    production::NormalMapArtifact forged_input;
    forged_input.recipe = overlapping_recipe;
    forged_input.rgba8.assign(64U * 64U * 4U, 0U);
    for (std::size_t pixel = 0U; pixel < forged_input.rgba8.size(); pixel += 4U) {
        forged_input.rgba8[pixel] = 128U;
        forged_input.rgba8[pixel + 1U] = 128U;
        forged_input.rgba8[pixel + 2U] = 255U;
    }
    forged_input.rgba8[3U] = 255U;
    forged_input.covered_texels = 1U;
    forged_input.projected_texels = 1U;
    REQUIRE(forged_input.refresh_digests());
    const auto overlapping_padding_recipe = production::make_bake_padding_recipe(
        forged_input, 2U);
    REQUIRE(overlapping_padding_recipe);
    const auto overlap = production::compile_bake_padding_cpu(
        overlapping_padding_recipe.value(), forged_input, source, overlapping_target);
    REQUIRE(!overlap);
    REQUIRE(overlap.error().code == core::ErrorCode::validation_failed);

    std::vector<core::Vec2d> crossing_uvs{
        {0.8, 0.1}, {1.2, 0.1}, {0.8, 0.8},
        {1.1, 0.1}, {1.9, 0.1}, {1.1, 0.8}};
    const geometry::EditableMesh crossing_target = make_target(std::move(crossing_uvs));
    const production::BakeRecipe crossing_recipe = make_recipe(
        source, crossing_target, 0U);
    forged_input.recipe = crossing_recipe;
    forged_input.artifact_digest = {};
    REQUIRE(forged_input.refresh_digests());
    const auto crossing_padding_recipe = production::make_bake_padding_recipe(
        forged_input, 2U);
    REQUIRE(crossing_padding_recipe);
    const auto crossing = production::compile_bake_padding_cpu(
        crossing_padding_recipe.value(), forged_input, source, crossing_target);
    REQUIRE(!crossing);
    REQUIRE(crossing.error().code == core::ErrorCode::unsupported);
}

} // namespace

int main() {
    try {
        tile_uv_changes_invalidate_only_the_affected_uv_tile();
        std::cout << "PASS per-tile dependency reuse survives unrelated UV tile edits\n";
        geometry_and_settings_changes_invalidate_tile_reuse();
        std::cout << "PASS source, target geometry, and settings changes invalidate tile reuse\n";
        cross_tile_faces_and_uncovered_tiles_are_not_cacheable();
        std::cout << "PASS cross-tile and uncovered bake inputs fail closed\n";
        cache_entry_reuses_only_verified_unchanged_tiles();
        std::cout << "PASS verified unchanged bake tiles survive serialization and revision rebinding\n";
        cache_entry_rejects_changed_dependencies_and_tampered_pixels();
        std::cout << "PASS changed bake dependencies and tampered cache bytes fail closed\n";
        tile_schedule_compiles_all_tiles_in_stable_order();
        std::cout << "PASS multi-tile bake scheduling is ordered and deterministic\n";
        tile_enumeration_orders_both_axes_and_enforces_bounds();
        std::cout << "PASS UDIM tile ordering and schedule bounds fail closed\n";
        tile_schedule_reuses_only_matching_cached_tiles();
        std::cout << "PASS scheduled bake tiles reuse only verified matching cache entries\n";
        tile_schedule_rejects_cross_tile_faces_and_partial_failure();
        std::cout << "PASS cross-tile and partially failed schedules admit no result\n";
        tile_schedule_rejects_a_corrupt_matching_cache_entry();
        std::cout << "PASS corrupt matching bake tile cache fails closed\n";
        seam_safe_padding_is_deterministic_serializable_and_publishable();
        std::cout << "PASS seam-safe bake padding is deterministic, source-bound, and publishable\n";
        typed_rgba8_bake_padding_preserves_provenance_and_publication();
        std::cout << "PASS typed bake padding preserves map identity and publication provenance\n";
        seam_safe_padding_rejects_overlapping_and_cross_tile_uvs();
        std::cout << "PASS overlapping and cross-tile bake padding inputs fail closed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL bake tile incremental tests: " << error.what() << '\n';
        return 1;
    }
}
