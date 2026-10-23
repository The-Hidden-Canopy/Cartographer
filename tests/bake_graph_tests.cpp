#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/journal/journal.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/material_catalog.hpp>
#include <carto/project/transaction.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
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
            ("cartographer-bake-graph-tests-" + std::to_string(stamp) + "-" +
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

using carto::attributes::AttributeDomain;
using carto::attributes::AttributeLayer;
using carto::attributes::AttributeSet;
using carto::attributes::AttributeType;
using carto::attributes::UvSetDescriptor;
using carto::attributes::UvSetTable;
using carto::core::Vec2d;
using carto::core::Vec3d;
using carto::core::Vec4d;
using carto::geometry::EditableMesh;
using carto::geometry::Face;
using carto::geometry::FaceId;
using carto::geometry::Vertex;
using carto::geometry::VertexId;
using carto::production::BakeBackfacePolicy;
using carto::production::BakeNormalConvention;
using carto::production::BakeNormalSpace;
using carto::production::BakeRecipe;
using carto::production::ArtifactKind;
using carto::production::ProductionCookGraph;
using carto::production::bind_bake_source;
using carto::production::bind_bake_target;
using carto::production::compile_tangent_normal_map_cpu;
using carto::production::register_bake_graph_node;
using carto::production::bind_world_position_recipe;
using carto::production::compile_world_position_cpu;
using carto::production::register_world_position_graph_node;

EditableMesh make_triangle(
    std::array<Vec3d, 3U> positions,
    bool with_uv,
    bool reverse_winding = false,
    std::array<Vec2d, 3U> uvs = {{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}}}) {
    EditableMesh mesh;
    std::vector<Vertex> vertices{
        {VertexId{1U}, positions[0U]},
        {VertexId{2U}, positions[1U]},
        {VertexId{3U}, positions[2U]},
    };
    Face face{FaceId{1U}, reverse_winding
        ? std::vector<VertexId>{VertexId{1U}, VertexId{3U}, VertexId{2U}}
        : std::vector<VertexId>{VertexId{1U}, VertexId{2U}, VertexId{3U}}};
    REQUIRE(mesh.insert_bulk(std::move(vertices), {std::move(face)}));
    if (with_uv) {
        const std::array<std::size_t, 3U> uv_order = reverse_winding
            ? std::array<std::size_t, 3U>{0U, 2U, 1U}
            : std::array<std::size_t, 3U>{0U, 1U, 2U};
        AttributeSet attributes({{AttributeDomain::corner, 3U}});
        REQUIRE(attributes.add_layer(AttributeLayer{
            {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
            {uvs[uv_order[0U]], uvs[uv_order[1U]], uvs[uv_order[2U]]}}));
        UvSetTable sets;
        REQUIRE(sets.add(UvSetDescriptor{7U, "UV0", "uv0.corners", true, true, false, 1U}));
        REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(sets)));
    }
    REQUIRE(mesh.validate());
    return mesh;
}

EditableMesh make_overlapping_target() {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}}, {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {0.0, 1.0, 0.0}}, {VertexId{4U}, {0.0, 0.0, 0.0}},
         {VertexId{5U}, {1.0, 0.0, 0.0}}, {VertexId{6U}, {0.0, 1.0, 0.0}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{4U}, VertexId{5U}, VertexId{6U}}}}));
    AttributeSet attributes({{AttributeDomain::corner, 6U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
        {Vec2d{0.0, 0.0}, Vec2d{1.0, 0.0}, Vec2d{0.0, 1.0},
         Vec2d{0.0, 0.0}, Vec2d{1.0, 0.0}, Vec2d{0.0, 1.0}}}));
    UvSetTable sets;
    REQUIRE(sets.add(UvSetDescriptor{7U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

BakeRecipe make_recipe(const EditableMesh& source, const EditableMesh& target) {
    const auto source_binding = bind_bake_source("project/asset/high", source);
    const auto target_binding = bind_bake_target("project/asset/target", target, 7U);
    REQUIRE(source_binding);
    REQUIRE(target_binding);
    BakeRecipe recipe;
    recipe.identity = "bake/target/normal";
    recipe.source = source_binding.value();
    recipe.target = target_binding.value();
    recipe.target_uv_set = 7U;
    recipe.width = 16U;
    recipe.height = 16U;
    recipe.projection_half_extent = 0.5;
    REQUIRE(recipe.validate());
    return recipe;
}

EditableMesh make_target() {
    return make_triangle({{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, true);
}

EditableMesh make_non_affine_tangent_target() {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}}, {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {1.0, 1.0, 0.0}}, {VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{1U}, VertexId{3U}, VertexId{4U}}}}));
    AttributeSet attributes({{AttributeDomain::corner, 6U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
        {Vec2d{0.0, 0.0}, Vec2d{1.0, 0.0}, Vec2d{0.8, 1.0},
         Vec2d{0.0, 0.0}, Vec2d{0.8, 1.0}, Vec2d{0.1, 0.9}}}));
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv1.corners", AttributeDomain::corner, AttributeType::vec2},
        {Vec2d{0.0, 0.0}, Vec2d{1.0, 0.0}, Vec2d{1.0, 1.0},
         Vec2d{0.1, 0.0}, Vec2d{0.9, 1.0}, Vec2d{0.0, 1.0}}}));
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"color.corners", AttributeDomain::corner, AttributeType::color4},
        {carto::core::Vec4d{1.0, 0.0, 0.0, 1.0},
         carto::core::Vec4d{1.0, 0.0, 0.0, 1.0},
         carto::core::Vec4d{1.0, 0.0, 0.0, 1.0},
         carto::core::Vec4d{0.0, 0.0, 1.0, 1.0},
         carto::core::Vec4d{0.0, 0.0, 1.0, 1.0},
         carto::core::Vec4d{0.0, 0.0, 1.0, 1.0}}}));
    UvSetTable sets;
    REQUIRE(sets.add(UvSetDescriptor{7U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(sets.add(UvSetDescriptor{8U, "UV1", "uv1.corners", false, false, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

EditableMesh make_tangent_discontinuity_target() {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}}, {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {1.0, 1.0, 0.0}}, {VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{1U}, VertexId{3U}, VertexId{4U}}}}));
    AttributeSet attributes({{AttributeDomain::corner, 6U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
        {Vec2d{0.0, 0.0}, Vec2d{1.0, 0.0}, Vec2d{0.8, 1.0},
         Vec2d{1.1, 0.0}, Vec2d{1.7, 0.9}, Vec2d{1.1, 0.8}}}));
    UvSetTable sets;
    REQUIRE(sets.add(UvSetDescriptor{7U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

EditableMesh make_tilted_tangent_source() {
    return make_triangle(
        {{{-2.0, -2.0, 0.4}, {3.0, -2.0, -0.1}, {0.5, 3.0, 0.15}}}, false);
}

struct WorldPositionProject {
    TempDirectory directory;
    carto::project::ProjectDocument document;
    std::uint64_t mesh_asset = 0U;
    carto::scene::ObjectId node;

    explicit WorldPositionProject(
        EditableMesh mesh,
        carto::core::Transform transform = carto::core::Transform::identity()) {
        carto::journal::Journal journal(directory.path() / "world-position-journal.log");
        auto transaction = carto::project::ProjectTransaction::begin(
            document, journal, "bake-world-position-test");
        REQUIRE(transaction);
        const auto added_mesh = transaction.value().add_mesh(std::move(mesh));
        REQUIRE(added_mesh);
        mesh_asset = added_mesh.value();
        const auto added_node = transaction.value().create_object("World position target", transform);
        REQUIRE(added_node);
        node = added_node.value();
        REQUIRE(transaction.value().attach_mesh(node, mesh_asset));
        REQUIRE(transaction.value().commit("bake.world-position.fixture"));
    }
};

EditableMesh make_material_surface_target(
    bool overlapping_uvs = false,
    bool cross_tile_uv = false,
    bool include_uvs = true) {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}}, {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {1.0, 1.0, 0.0}}, {VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{1U}, VertexId{3U}, VertexId{4U}}}}));
    AttributeSet attributes({
        {AttributeDomain::face, 2U},
        {AttributeDomain::corner, 6U},
    });
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"condition.material_region", AttributeDomain::face, AttributeType::uint32},
        {std::uint32_t{3U}, std::uint32_t{9U}}}));
    UvSetTable uv_sets;
    if (include_uvs) {
        const double shared_u = cross_tile_uv ? 1.2 : 1.0;
        std::vector<carto::core::Vec2d> uvs{
            {0.0, 0.0}, {shared_u, 0.0}, {shared_u, 1.0},
            {0.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};
        if (overlapping_uvs) {
            uvs[3U] = {0.0, 0.0};
            uvs[4U] = {1.0, 0.0};
            uvs[5U] = {1.0, 1.0};
        }
        REQUIRE(attributes.set_domain_count(AttributeDomain::corner, uvs.size()));
        REQUIRE(attributes.add_layer(AttributeLayer{
            {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
            std::vector<carto::attributes::AttributeValue>(uvs.begin(), uvs.end())}));
        REQUIRE(uv_sets.add(UvSetDescriptor{
            1U, "UV0", "uv0.corners", true, true, false, 1U}));
    }
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

struct MaterialSurfaceIdProject {
    TempDirectory directory;
    carto::project::ProjectDocument document;
    std::uint64_t mesh_asset = 0U;

    explicit MaterialSurfaceIdProject(
        EditableMesh mesh,
        bool omit_first_assignment = false) {
        using namespace carto;
        journal::Journal journal(directory.path() / "material-surface-id-journal.log");
        auto transaction = project::ProjectTransaction::begin(
            document, journal, "bake-material-surface-id-test");
        REQUIRE(transaction);
        const auto added_mesh = transaction.value().add_mesh(std::move(mesh));
        REQUIRE(added_mesh);
        mesh_asset = added_mesh.value();

        project::MaterialCatalog catalog;
        render::MaterialAsset paint;
        paint.id = 71U;
        paint.name = "Paint";
        paint.revision = core::Revision{1U};
        render::MaterialAsset rubber;
        rubber.id = 72U;
        rubber.name = "Rubber";
        rubber.revision = core::Revision{1U};
        REQUIRE(catalog.insert_material(std::move(paint)));
        REQUIRE(catalog.insert_material(std::move(rubber)));
        project::MaterialVariant worn;
        worn.id = 901U;
        worn.name = "Worn paint";
        worn.base_material = 71U;
        worn.overrides.roughness = 0.84;
        worn.permitted_mesh_assets = {mesh_asset};
        worn.revision = core::Revision{1U};
        REQUIRE(catalog.insert_variant(std::move(worn)));
        if (!omit_first_assignment) {
            REQUIRE(catalog.insert_assignment({
                {mesh_asset, 3U}, 71U, 901U}));
        }
        REQUIRE(catalog.insert_assignment({{mesh_asset, 9U}, 72U, std::nullopt}));
        REQUIRE(transaction.value().set_material_catalog(std::move(catalog)));
        REQUIRE(transaction.value().commit("bake.material-surface-id.fixture"));
    }

    [[nodiscard]] std::vector<carto::surface::SurfaceDomain> domains() const {
        const auto compiled = document.meshes().at(mesh_asset).compile();
        REQUIRE(compiled);
        carto::surface::SurfaceDomain first{
            .id = carto::surface::SurfaceDomainId{101U},
            .source_faces = {FaceId{1U}},
            .source_revision = compiled.value().source_revision,
            .semantic_class = "painted-shell",
            .material_class = "paint",
            .source_content_digest = {},
            .digest = {},
        };
        carto::surface::SurfaceDomain second{
            .id = carto::surface::SurfaceDomainId{102U},
            .source_faces = {FaceId{2U}},
            .source_revision = compiled.value().source_revision,
            .semantic_class = "grip",
            .material_class = "rubber",
            .source_content_digest = {},
            .digest = {},
        };
        REQUIRE(first.refresh_digest(compiled.value()));
        REQUIRE(second.refresh_digest(compiled.value()));
        return {second, first};
    }
};

std::uint64_t read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

EditableMesh make_high_surface() {
    return make_triangle({{{0.0, 0.0, 0.05}, {1.0, 0.0, 0.05}, {0.0, 1.0, 0.30}}}, false);
}

EditableMesh make_ao_occluder_scene() {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.0}}, {VertexId{2U}, {1.0, 0.0, 0.0}},
         {VertexId{3U}, {0.0, 1.0, 0.0}}, {VertexId{4U}, {0.35, 0.25, 0.0}},
         {VertexId{5U}, {0.35, 0.75, 0.0}}, {VertexId{6U}, {0.35, 0.50, 0.60}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{4U}, VertexId{5U}, VertexId{6U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

EditableMesh make_curvature_tetrahedron() {
    EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.05}}, {VertexId{2U}, {1.0, 0.0, 0.05}},
         {VertexId{3U}, {0.0, 1.0, 0.05}}, {VertexId{4U}, {0.25, 0.25, 0.65}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{3U}, VertexId{2U}}},
         {FaceId{2U}, {VertexId{1U}, VertexId{2U}, VertexId{4U}}},
         {FaceId{3U}, {VertexId{2U}, VertexId{3U}, VertexId{4U}}},
         {FaceId{4U}, {VertexId{3U}, VertexId{1U}, VertexId{4U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

EditableMesh make_thickness_shell(bool partial_back_surface = false) {
    EditableMesh mesh;
    const std::array<carto::geometry::Vertex, 6U> vertices{{
        {VertexId{1U}, {0.0, 0.0, 0.05}},
        {VertexId{2U}, {1.0, 0.0, 0.05}},
        {VertexId{3U}, {0.0, 1.0, 0.05}},
        {VertexId{4U}, {0.0, 0.0, -0.15}},
        {VertexId{5U}, {partial_back_surface ? 0.5 : 1.0, 0.0, -0.15}},
        {VertexId{6U}, {0.0, partial_back_surface ? 0.5 : 1.0, -0.15}},
    }};
    REQUIRE(mesh.insert_bulk(
        std::vector<Vertex>(vertices.begin(), vertices.end()),
        {{FaceId{1U}, {VertexId{1U}, VertexId{2U}, VertexId{3U}}},
         {FaceId{2U}, {VertexId{4U}, VertexId{6U}, VertexId{5U}}}}));
    REQUIRE(mesh.validate());
    return mesh;
}

bool has_diagnostic(
    const carto::production::BakeExecutionReport& report,
    std::string_view code) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
        [code](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

bool has_thickness_diagnostic(
    const carto::production::ThicknessExecutionReport& report,
    std::string_view code) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
        [code](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

bool has_height_displacement_diagnostic(
    const carto::production::HeightDisplacementExecutionReport& report,
    std::string_view code) {
    return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
        [code](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == code;
        });
}

void recipe_serialization_is_canonical_and_rejects_hostile_fields() {
    auto source = make_high_surface();
    auto target = make_target();
    const BakeRecipe recipe = make_recipe(source, target);
    const std::string serialized = recipe.serialize();
    REQUIRE(!serialized.empty());
    const auto reopened = BakeRecipe::deserialize(serialized);
    REQUIRE(reopened);
    REQUIRE(reopened.value().serialize() == serialized);
    REQUIRE(reopened.value().source == recipe.source);
    REQUIRE(reopened.value().target == recipe.target);

    REQUIRE(!BakeRecipe::deserialize(serialized + "identity=00\n"));
    REQUIRE(!BakeRecipe::deserialize(serialized.substr(0U, serialized.size() - 1U)));
    std::string unknown = serialized;
    const std::size_t field = unknown.find("policy.reject_projection_misses=");
    REQUIRE(field != std::string::npos);
    unknown.replace(field, std::string("policy.reject_projection_misses").size(), "unknown");
    REQUIRE(!BakeRecipe::deserialize(unknown));

    BakeRecipe invalid = recipe;
    invalid.width = 0U;
    REQUIRE(!invalid.validate());
    invalid = recipe;
    invalid.projection_half_extent = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!invalid.validate());
    REQUIRE(!bind_bake_target("project/asset/target", target, 999U));
}

void cpu_normal_bake_is_deterministic_and_round_trips() {
    auto source = make_high_surface();
    auto target = make_target();
    const BakeRecipe recipe = make_recipe(source, target);
    const auto first = compile_tangent_normal_map_cpu(recipe, source, target);
    const auto second = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(first.succeeded());
    REQUIRE(second.succeeded());
    REQUIRE(first.covered_texels > 0U);
    REQUIRE(first.projected_texels == first.covered_texels);
    REQUIRE(first.projection_misses == 0U);
    REQUIRE(first.artifact->rgba8 == second.artifact->rgba8);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->validate());

    const std::size_t sample = (2U * recipe.width + 2U) * 4U;
    REQUIRE(first.artifact->rgba8[sample + 3U] == 255U);
    REQUIRE(first.artifact->rgba8[sample + 1U] < 128U);
    REQUIRE(first.artifact->rgba8[sample + 2U] > 240U);

    auto malformed_coverage = *first.artifact;
    malformed_coverage.rgba8[sample + 3U] = 127U;
    REQUIRE(malformed_coverage.refresh_digests());
    REQUIRE(!malformed_coverage.validate());

    auto directx_recipe = recipe;
    directx_recipe.normal_convention = BakeNormalConvention::directx_y_minus;
    const auto directx = compile_tangent_normal_map_cpu(directx_recipe, source, target);
    REQUIRE(directx.succeeded());
    REQUIRE(directx.artifact->rgba8[sample + 1U] > 128U);

    const auto serialized = first.artifact->serialize();
    REQUIRE(serialized);
    const auto reopened = carto::production::TangentNormalMapArtifact::deserialize(
        serialized.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == first.artifact->artifact_digest);
    REQUIRE(reopened.value().rgba8 == first.artifact->rgba8);

    auto tampered = serialized.value();
    tampered.back() ^= 1U;
    REQUIRE(!carto::production::TangentNormalMapArtifact::deserialize(tampered));
    auto truncated = serialized.value();
    truncated.pop_back();
    REQUIRE(!carto::production::TangentNormalMapArtifact::deserialize(truncated));
    auto trailing = serialized.value();
    trailing.push_back(0U);
    REQUIRE(!carto::production::TangentNormalMapArtifact::deserialize(trailing));
}

void tangent_normal_bake_uses_the_shared_uv0_tangent_domain() {
    auto source = make_tilted_tangent_source();
    auto target = make_non_affine_tangent_target();
    BakeRecipe recipe = make_recipe(source, target);
    recipe.width = 32U;
    recipe.height = 32U;
    recipe.projection_half_extent = 0.5;
    REQUIRE(recipe.validate());

    const std::array<std::uint8_t, 1U> source_bytes{0x5aU};
    const carto::production::AuthoritativeSource render_source{
        "project/tangent-frame-fixture",
        carto::core::Revision{1U},
        carto::assets::sha256(source_bytes),
    };
    const auto render_mesh = carto::production::compile_render_mesh_artifact(
        render_source, target, carto::production::RenderMeshCompileOptions{
            .uv_set = 7U,
            .uv1_set = 8U,
            .generate_tangents = true,
            .material_slot_layer = std::nullopt,
            .vertex_color_layer = "color.corners",
            .normal_layer = std::nullopt,
        });
    if (!render_mesh) throw std::runtime_error(render_mesh.error().message);

    std::map<VertexId, Vec4d> tangent_by_vertex;
    std::size_t vertex_one_uv0_duplicates = 0U;
    std::size_t vertex_three_uv0_duplicates = 0U;
    for (const auto& vertex : render_mesh.value().vertices) {
        REQUIRE(vertex.tangent.has_value());
        auto [found, inserted] = tangent_by_vertex.emplace(
            vertex.source_vertex, vertex.tangent.value());
        if (!inserted) {
            REQUIRE(std::abs(found->second.x - vertex.tangent->x) < 1e-15);
            REQUIRE(std::abs(found->second.y - vertex.tangent->y) < 1e-15);
            REQUIRE(std::abs(found->second.z - vertex.tangent->z) < 1e-15);
            REQUIRE(std::abs(found->second.w - vertex.tangent->w) < 1e-15);
        }
        if (vertex.source_vertex == VertexId{1U}) ++vertex_one_uv0_duplicates;
        if (vertex.source_vertex == VertexId{3U}) ++vertex_three_uv0_duplicates;
    }
    REQUIRE(vertex_one_uv0_duplicates == 2U);
    REQUIRE(vertex_three_uv0_duplicates == 2U);

    const auto baked = compile_tangent_normal_map_cpu(recipe, source, target);
    const auto repeated_bake = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(baked.succeeded());
    REQUIRE(repeated_bake.succeeded());
    REQUIRE(baked.artifact->rgba8 == repeated_bake.artifact->rgba8);
    REQUIRE(baked.artifact->artifact_digest == repeated_bake.artifact->artifact_digest);
    REQUIRE(baked.artifact->algorithm == carto::production::kBakeNormalAlgorithm);

    const std::array<std::array<VertexId, 3U>, 2U> triangle_vertices{{
        {{VertexId{1U}, VertexId{2U}, VertexId{3U}}},
        {{VertexId{1U}, VertexId{3U}, VertexId{4U}}},
    }};
    const std::array<std::array<Vec2d, 3U>, 2U> triangle_uvs{{
        {{{0.0, 0.0}, {1.0, 0.0}, {0.8, 1.0}}},
        {{{0.0, 0.0}, {0.8, 1.0}, {0.1, 0.9}}},
    }};
    const auto edge = [](Vec2d first, Vec2d second, Vec2d point) {
        return (second.x - first.x) * (point.y - first.y) -
            (second.y - first.y) * (point.x - first.x);
    };
    const auto source_compiled = source.compile();
    REQUIRE(source_compiled);
    const Vec3d expected_world_normal = source_compiled.value().normals[0U].normalized();
    const Vec3d target_normal{0.0, 0.0, 1.0};
    std::array<bool, 2U> decoded_each_triangle{};

    for (std::uint32_t y = 0U; y < recipe.height; ++y) {
        for (std::uint32_t x = 0U; x < recipe.width; ++x) {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * recipe.width + x) * 4U;
            if (baked.artifact->rgba8[pixel + 3U] != 255U) continue;
            const Vec2d sample{
                (static_cast<double>(x) + 0.5) / recipe.width,
                (static_cast<double>(y) + 0.5) / recipe.height};
            bool found_triangle = false;
            for (std::size_t triangle = 0U; triangle < triangle_vertices.size(); ++triangle) {
                const auto& uv = triangle_uvs[triangle];
                const double area = edge(uv[0U], uv[1U], uv[2U]);
                const std::array<double, 3U> barycentric{
                    edge(uv[1U], uv[2U], sample) / area,
                    edge(uv[2U], uv[0U], sample) / area,
                    edge(uv[0U], uv[1U], sample) / area,
                };
                if (std::any_of(barycentric.begin(), barycentric.end(), [](double value) {
                        return value < -1e-10;
                    })) {
                    continue;
                }

                Vec3d tangent{};
                double handedness = 0.0;
                for (std::size_t corner = 0U; corner < 3U; ++corner) {
                    const Vec4d vertex_tangent = tangent_by_vertex.at(
                        triangle_vertices[triangle][corner]);
                    tangent = tangent + Vec3d{
                        vertex_tangent.x, vertex_tangent.y, vertex_tangent.z} *
                        barycentric[corner];
                    handedness += vertex_tangent.w * barycentric[corner];
                }
                tangent = (tangent - target_normal *
                    carto::core::dot(target_normal, tangent)).normalized();
                const Vec3d bitangent = carto::core::cross(target_normal, tangent) *
                    (handedness < 0.0 ? -1.0 : 1.0);
                const auto decode = [](std::uint8_t value) {
                    return static_cast<double>(value) / 255.0 * 2.0 - 1.0;
                };
                const Vec3d tangent_normal{
                    decode(baked.artifact->rgba8[pixel]),
                    decode(baked.artifact->rgba8[pixel + 1U]),
                    decode(baked.artifact->rgba8[pixel + 2U]),
                };
                const Vec3d decoded_world_normal = (
                    tangent * tangent_normal.x + bitangent * tangent_normal.y +
                    target_normal * tangent_normal.z).normalized();
                REQUIRE(carto::core::dot(
                    decoded_world_normal, expected_world_normal) > 0.999);
                decoded_each_triangle[triangle] = true;
                found_triangle = true;
                break;
            }
            REQUIRE(found_triangle);
        }
    }
    REQUIRE(decoded_each_triangle[0U]);
    REQUIRE(decoded_each_triangle[1U]);
}

void tangent_normal_bake_reports_uv_seam_frame_discontinuity() {
    auto source = make_tilted_tangent_source();
    auto target = make_tangent_discontinuity_target();
    BakeRecipe recipe = make_recipe(source, target);
    recipe.width = 16U;
    recipe.height = 16U;
    REQUIRE(recipe.validate());

    const auto report = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(report.succeeded());
    const auto diagnostic = std::find_if(
        report.diagnostics.begin(), report.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& item) {
            return item.code == "target_tangent_discontinuity";
        });
    REQUIRE(diagnostic != report.diagnostics.end());
    REQUIRE(!diagnostic->is_error);
    REQUIRE(diagnostic->target_face.has_value());
    REQUIRE(diagnostic->message.find("UV seam") != std::string::npos);
}

void tangent_normal_bake_reports_projection_cage_crossing_without_rejecting() {
    auto target = make_target();
    auto outward_source = make_triangle(
        {{{0.0, 0.0, 0.05}, {1.0, 0.0, 0.05}, {0.0, 1.0, 0.05}}}, false);
    const auto clear = compile_tangent_normal_map_cpu(
        make_recipe(outward_source, target), outward_source, target);
    REQUIRE(clear.succeeded());
    REQUIRE(std::none_of(clear.diagnostics.begin(), clear.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& item) {
            return item.code == "projection_cage_intersection";
        }));

    auto inward_source = make_triangle(
        {{{0.0, 0.0, -0.05}, {1.0, 0.0, -0.05}, {0.0, 1.0, -0.05}}}, false);
    const auto crossed = compile_tangent_normal_map_cpu(
        make_recipe(inward_source, target), inward_source, target);
    REQUIRE(crossed.succeeded());
    const auto diagnostic = std::find_if(
        crossed.diagnostics.begin(), crossed.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& item) {
            return item.code == "projection_cage_intersection";
        });
    REQUIRE(diagnostic != crossed.diagnostics.end());
    REQUIRE(!diagnostic->is_error);
    REQUIRE(diagnostic->target_face == FaceId{1U});
    REQUIRE(diagnostic->source_face == FaceId{1U});
}

void object_space_normal_bake_has_distinct_encoding_and_identity() {
    auto source = make_triangle(
        {{{-0.05, 0.0, 0.0}, {-0.05, 1.0, 0.0}, {-0.30, 0.0, 1.0}}}, false);
    auto target = make_triangle(
        {{{0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}}, true);
    BakeRecipe recipe = make_recipe(source, target);
    recipe.projection_half_extent = 0.5;

    const auto tangent = compile_tangent_normal_map_cpu(recipe, source, target);
    const auto object = carto::production::compile_object_space_normal_map_cpu(
        recipe, source, target);
    REQUIRE(tangent.succeeded());
    REQUIRE(object.succeeded());
    REQUIRE(tangent.artifact->normal_space == BakeNormalSpace::tangent);
    REQUIRE(object.artifact->normal_space == BakeNormalSpace::object);
    REQUIRE(object.artifact->algorithm == carto::production::kBakeObjectNormalAlgorithm);

    const std::size_t sample = (2U * recipe.width + 2U) * 4U;
    REQUIRE(tangent.artifact->rgba8[sample] == 128U);
    REQUIRE(tangent.artifact->rgba8[sample + 2U] > 240U);
    REQUIRE(object.artifact->rgba8[sample] > 240U);
    REQUIRE(object.artifact->rgba8[sample + 1U] == 128U);
    REQUIRE(object.artifact->rgba8[sample + 2U] > 150U);
    REQUIRE(object.artifact->settings_digest != tangent.artifact->settings_digest);

    const auto tangent_key = carto::production::bake_production_cook_key(recipe);
    const auto object_key = carto::production::bake_production_cook_key(
        recipe, BakeNormalSpace::object);
    REQUIRE(tangent_key && object_key && tangent_key.value() != object_key.value());

    const auto serialized = object.artifact->serialize();
    REQUIRE(serialized);
    const auto reopened = carto::production::NormalMapArtifact::deserialize(
        serialized.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().normal_space == BakeNormalSpace::object);
    REQUIRE(reopened.value().artifact_digest == object.artifact->artifact_digest);

    auto invalid_space = *object.artifact;
    invalid_space.normal_space = static_cast<BakeNormalSpace>(255U);
    REQUIRE(!invalid_space.refresh_digests());
}

void ambient_occlusion_cpu_is_deterministic_and_round_trips() {
    auto source = make_ao_occluder_scene();
    auto target = make_target();
    carto::production::AmbientOcclusionRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/ao";
    recipe.sample_count = 64U;
    recipe.max_distance = 0.8;
    recipe.origin_bias = 1e-4;
    recipe.seed = 0x5a17U;
    REQUIRE(recipe.validate());

    const std::string serialized_recipe = recipe.serialize();
    const auto reopened_recipe =
        carto::production::AmbientOcclusionRecipe::deserialize(serialized_recipe);
    REQUIRE(reopened_recipe);
    REQUIRE(reopened_recipe.value().serialize() == serialized_recipe);
    REQUIRE(!carto::production::AmbientOcclusionRecipe::deserialize(
        serialized_recipe + "unknown=1\n"));
    auto invalid_recipe = recipe;
    invalid_recipe.sample_count = 0U;
    REQUIRE(!invalid_recipe.validate());
    invalid_recipe = recipe;
    invalid_recipe.origin_bias = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!invalid_recipe.validate());

    const auto first = carto::production::compile_ambient_occlusion_cpu(
        recipe, source, target);
    const auto second = carto::production::compile_ambient_occlusion_cpu(
        recipe, source, target);
    REQUIRE(first.succeeded());
    REQUIRE(second.succeeded());
    REQUIRE(first.covered_texels > 0U);
    REQUIRE(first.rays_cast == first.covered_texels * recipe.sample_count);
    REQUIRE(first.occluded_rays > 0U);
    REQUIRE(first.rays_cast <= carto::production::kMaxBakeAmbientOcclusionRays);
    REQUIRE(first.artifact->rgba8 == second.artifact->rgba8);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->sampling_standard_error_upper_bound == 0.0625);
    REQUIRE(first.artifact->validate());
    auto unsupported_metric = *first.artifact;
    unsupported_metric.sampling_standard_error_upper_bound = 0.0;
    REQUIRE(!unsupported_metric.validate());

    const auto key = carto::production::bake_ambient_occlusion_cook_key(recipe);
    REQUIRE(key);
    REQUIRE(key.value().algorithm ==
        carto::production::kBakeAmbientOcclusionAlgorithm);
    carto::production::ProductionCookGraph graph;
    const auto registered = carto::production::register_ambient_occlusion_graph_node(
        graph, recipe);
    REQUIRE(registered && registered.value() == key.value());
    REQUIRE(graph.validate());
    auto other_sampling = recipe;
    ++other_sampling.seed;
    const auto other_key = carto::production::bake_ambient_occlusion_cook_key(
        other_sampling);
    REQUIRE(other_key && other_key.value() != key.value());

    bool found_occluded = false;
    bool found_unoccluded = false;
    for (std::size_t pixel = 0U; pixel < first.artifact->rgba8.size(); pixel += 4U) {
        const auto alpha = first.artifact->rgba8[pixel + 3U];
        REQUIRE(alpha == 0U || alpha == 255U);
        if (alpha == 255U && first.artifact->rgba8[pixel] < 255U) found_occluded = true;
        if (alpha == 255U && first.artifact->rgba8[pixel] == 255U) found_unoccluded = true;
    }
    REQUIRE(found_occluded);
    REQUIRE(found_unoccluded);

    const auto bytes = first.artifact->serialize();
    REQUIRE(bytes);
    const auto reopened = carto::production::AmbientOcclusionMapArtifact::deserialize(
        bytes.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().recipe.serialize() == serialized_recipe);
    REQUIRE(reopened.value().rgba8 == first.artifact->rgba8);
    auto tampered = bytes.value();
    tampered.back() ^= 1U;
    REQUIRE(!carto::production::AmbientOcclusionMapArtifact::deserialize(tampered));
    auto truncated = bytes.value();
    truncated.pop_back();
    REQUIRE(!carto::production::AmbientOcclusionMapArtifact::deserialize(truncated));
    auto trailing = bytes.value();
    trailing.push_back(0U);
    REQUIRE(!carto::production::AmbientOcclusionMapArtifact::deserialize(trailing));

    auto malformed_coverage = *first.artifact;
    std::size_t covered_pixel = malformed_coverage.rgba8.size();
    for (std::size_t pixel = 3U; pixel < malformed_coverage.rgba8.size(); pixel += 4U) {
        if (malformed_coverage.rgba8[pixel] == 255U) {
            covered_pixel = pixel;
            break;
        }
    }
    REQUIRE(covered_pixel < malformed_coverage.rgba8.size());
    REQUIRE(malformed_coverage.rgba8[covered_pixel] == 255U);
    malformed_coverage.rgba8[covered_pixel] = 127U;
    REQUIRE(malformed_coverage.refresh_digests());
    REQUIRE(!malformed_coverage.validate());
}

void stale_sources_and_projection_misses_fail_closed_without_mutation() {
    auto source = make_high_surface();
    auto target = make_target();
    const BakeRecipe recipe = make_recipe(source, target);
    const auto source_revision = source.revision();
    const auto target_revision = target.revision();
    REQUIRE(source.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.06}));
    const auto stale_report = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(!stale_report.succeeded());
    REQUIRE(!stale_report.artifact.has_value());
    REQUIRE(has_diagnostic(stale_report, "bake_input_invalid"));
    REQUIRE(source.revision() != source_revision);
    REQUIRE(target.revision() == target_revision);

    auto far_source = make_triangle(
        {{{0.0, 0.0, 5.0}, {1.0, 0.0, 5.0}, {0.0, 1.0, 5.0}}}, false);
    auto miss_recipe = make_recipe(far_source, target);
    const auto rejected = compile_tangent_normal_map_cpu(miss_recipe, far_source, target);
    REQUIRE(!rejected.succeeded());
    REQUIRE(!rejected.artifact.has_value());
    REQUIRE(rejected.projection_misses > 0U);
    REQUIRE(has_diagnostic(rejected, "projection_miss"));

    miss_recipe.reject_projection_misses = false;
    const auto partial = compile_tangent_normal_map_cpu(miss_recipe, far_source, target);
    REQUIRE(partial.succeeded());
    REQUIRE(partial.artifact->projection_misses == partial.covered_texels);
    REQUIRE(!partial.artifact->projection_miss_samples.empty());
    REQUIRE(partial.artifact->projection_miss_samples_truncated);
    REQUIRE(partial.artifact->projection_miss_samples.front().target_face == FaceId{1U});
    REQUIRE(partial.artifact->validate());
    const auto partial_serialized = partial.artifact->serialize();
    REQUIRE(partial_serialized);
    const auto partial_reopened =
        carto::production::TangentNormalMapArtifact::deserialize(partial_serialized.value());
    REQUIRE(partial_reopened);
    REQUIRE(partial_reopened.value().projection_miss_samples ==
        partial.artifact->projection_miss_samples);
    REQUIRE(partial_reopened.value().projection_miss_samples_truncated);
}

void same_revision_content_drift_invalidates_source_and_target_bindings() {
    auto source = make_high_surface();
    auto target = make_target();
    const BakeRecipe recipe = make_recipe(source, target);

    auto changed_source = make_triangle(
        {{{0.0, 0.0, 0.05}, {1.0, 0.0, 0.05}, {0.0, 1.0, 0.24}}}, false);
    REQUIRE(changed_source.revision() == source.revision());
    const auto source_drift = compile_tangent_normal_map_cpu(recipe, changed_source, target);
    REQUIRE(!source_drift.succeeded());
    REQUIRE(!source_drift.artifact.has_value());
    REQUIRE(has_diagnostic(source_drift, "bake_input_invalid"));

    auto changed_target = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}},
        true, false, {{{0.0, 0.0}, {0.75, 0.0}, {0.0, 1.0}}});
    REQUIRE(changed_target.revision() == target.revision());
    const auto target_drift = compile_tangent_normal_map_cpu(recipe, source, changed_target);
    REQUIRE(!target_drift.succeeded());
    REQUIRE(!target_drift.artifact.has_value());
    REQUIRE(has_diagnostic(target_drift, "bake_input_invalid"));
}

void backface_and_uv_overlap_policies_are_explicit() {
    auto target = make_target();
    auto reversed_source = make_triangle(
        {{{0.0, 0.0, 0.05}, {1.0, 0.0, 0.05}, {0.0, 1.0, 0.05}}},
        false, true);
    auto front_only_recipe = make_recipe(reversed_source, target);
    front_only_recipe.backface_policy = BakeBackfacePolicy::front_only;
    const auto rejected = compile_tangent_normal_map_cpu(
        front_only_recipe, reversed_source, target);
    REQUIRE(!rejected.succeeded());
    REQUIRE(rejected.projection_misses > 0U);
    REQUIRE(rejected.projection_backface_rejections == rejected.projection_misses);
    REQUIRE(has_diagnostic(rejected, "projection_backface_rejected"));
    const auto rejected_backface = std::find_if(
        rejected.diagnostics.begin(), rejected.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "projection_backface_rejected";
        });
    REQUIRE(rejected_backface != rejected.diagnostics.end());
    REQUIRE(!rejected_backface->is_error);
    REQUIRE(rejected_backface->target_face == FaceId{1U});
    REQUIRE(rejected_backface->source_face == FaceId{1U});

    front_only_recipe.backface_policy = BakeBackfacePolicy::both_sides;
    const auto accepted = compile_tangent_normal_map_cpu(
        front_only_recipe, reversed_source, target);
    REQUIRE(accepted.succeeded());
    REQUIRE(accepted.projection_backface_hits == accepted.projected_texels);
    REQUIRE(accepted.projection_backface_rejections == 0U);
    REQUIRE(has_diagnostic(accepted, "projection_backface_hit"));
    const auto selected_backface = std::find_if(
        accepted.diagnostics.begin(), accepted.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "projection_backface_hit";
        });
    REQUIRE(selected_backface != accepted.diagnostics.end());
    REQUIRE(!selected_backface->is_error);
    REQUIRE(selected_backface->source_face == FaceId{1U});

    EditableMesh layered_source;
    REQUIRE(layered_source.insert_bulk(
        {{VertexId{1U}, {0.0, 0.0, 0.05}}, {VertexId{2U}, {1.0, 0.0, 0.05}},
         {VertexId{3U}, {0.0, 1.0, 0.05}}, {VertexId{4U}, {0.0, 0.0, -0.05}},
         {VertexId{5U}, {1.0, 0.0, -0.05}}, {VertexId{6U}, {0.0, 1.0, -0.05}}},
        {{FaceId{1U}, {VertexId{1U}, VertexId{3U}, VertexId{2U}}},
         {FaceId{2U}, {VertexId{4U}, VertexId{5U}, VertexId{6U}}}}));
    REQUIRE(layered_source.validate());
    auto layered_recipe = make_recipe(layered_source, target);
    layered_recipe.backface_policy = BakeBackfacePolicy::front_only;
    const auto filtered = compile_tangent_normal_map_cpu(
        layered_recipe, layered_source, target);
    REQUIRE(filtered.succeeded());
    REQUIRE(filtered.projection_misses == 0U);
    REQUIRE(filtered.projection_backface_rejections == 0U);
    const std::size_t front_layer_sample = (2U * layered_recipe.width + 2U) * 4U;
    REQUIRE(filtered.artifact->rgba8[front_layer_sample + 2U] > 240U);

    auto overlapping = make_overlapping_target();
    auto source = make_high_surface();
    const auto overlap_recipe = make_recipe(source, overlapping);
    const auto overlap_report = compile_tangent_normal_map_cpu(
        overlap_recipe, source, overlapping);
    REQUIRE(!overlap_report.succeeded());
    REQUIRE(!overlap_report.artifact.has_value());
    REQUIRE(has_diagnostic(overlap_report, "overlapping_uv_islands"));
}

void cross_tile_uv_triangles_fail_closed() {
    auto source = make_high_surface();
    auto target = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}},
        true, false, {{{0.0, 0.0}, {1.25, 0.0}, {0.0, 1.0}}});
    const auto recipe = make_recipe(source, target);
    const auto report = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(!report.succeeded());
    REQUIRE(!report.artifact.has_value());
    REQUIRE(has_diagnostic(report, "cross_tile_uv_triangle"));
}

void bake_nodes_use_production_cook_identity_and_invalidation() {
    auto source = make_high_surface();
    auto target = make_target();
    const BakeRecipe recipe = make_recipe(source, target);
    ProductionCookGraph graph;
    const auto registered = register_bake_graph_node(graph, recipe);
    REQUIRE(registered);
    REQUIRE(registered.value().kind == ArtifactKind::baked_texture);
    REQUIRE(registered.value().source.identity == recipe.source.identity);
    REQUIRE(registered.value().settings_digest == recipe.canonical_digest().value());

    auto downstream = registered.value();
    downstream.kind = ArtifactKind::material_package;
    downstream.algorithm = "carto.test.bake-consumer.v1";
    downstream.representation_kind = "test-consumer";
    downstream.spatial_scope = "test-consumer-scope";
    REQUIRE(graph.add_node(downstream, {registered.value()}));
    REQUIRE(graph.validate());
    const auto order = graph.topological_order();
    REQUIRE(order);
    REQUIRE(order.value().size() == 2U);
    const auto invalidated = graph.invalidation_order(registered.value());
    REQUIRE(invalidated);
    REQUIRE(std::find(invalidated.value().begin(), invalidated.value().end(), downstream) !=
        invalidated.value().end());

    const auto baked = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(baked.succeeded());
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = registered.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = baked.artifact->output_digest;
    receipt.output_bytes = static_cast<std::uint64_t>(baked.artifact->rgba8.size());
    REQUIRE(receipt.refresh_digest());
    carto::production::ProductionReceiptLedger ledger;
    REQUIRE(ledger.append(receipt));
    const auto ledger_text = ledger.serialize();
    const auto reopened_ledger =
        carto::production::ProductionReceiptLedger::deserialize(ledger_text);
    REQUIRE(reopened_ledger);
    const auto reopened_receipt = reopened_ledger.value().query(registered.value());
    REQUIRE(reopened_receipt);
    REQUIRE(reopened_receipt.value().has_value());
    REQUIRE(reopened_receipt.value()->key.kind == ArtifactKind::baked_texture);

    auto other_target = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}},
        true, false, {{{0.0, 0.0}, {0.75, 0.0}, {0.0, 1.0}}});
    BakeRecipe other_recipe = make_recipe(source, other_target);
    const auto other_key = carto::production::bake_production_cook_key(other_recipe);
    REQUIRE(other_key);
    REQUIRE(other_key.value() != registered.value());
}

void normal_map_publication_revalidates_both_meshes_and_receipt_identity() {
    auto source = make_high_surface();
    auto target = make_target();
    const BakeRecipe recipe = make_recipe(source, target);
    const auto bake = compile_tangent_normal_map_cpu(recipe, source, target);
    REQUIRE(bake.succeeded());
    const auto key = carto::production::bake_production_cook_key(recipe);
    REQUIRE(key);
    const auto payload = bake.artifact->serialize();
    REQUIRE(payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;

    REQUIRE(carto::production::publish_normal_map(
        store, receipt, *bake.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    const auto reopened = carto::production::read_normal_map(
        store, recipe, BakeNormalSpace::tangent, source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == bake.artifact->artifact_digest);

    const auto object_bake = carto::production::compile_object_space_normal_map_cpu(
        recipe, source, target);
    REQUIRE(object_bake.succeeded());
    const auto object_key = carto::production::bake_production_cook_key(
        recipe, BakeNormalSpace::object);
    const auto object_payload = object_bake.artifact->serialize();
    REQUIRE(object_key && object_payload);
    auto object_receipt = receipt;
    object_receipt.key = object_key.value();
    object_receipt.output_digest = carto::assets::sha256(object_payload.value());
    object_receipt.output_bytes = static_cast<std::uint64_t>(object_payload.value().size());
    REQUIRE(object_receipt.refresh_digest());
    REQUIRE(carto::production::publish_normal_map(
        store, object_receipt, *object_bake.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 2U);
    const auto reopened_object = carto::production::read_normal_map(
        store, recipe, BakeNormalSpace::object, source, target, policy);
    REQUIRE(reopened_object);
    REQUIRE(reopened_object.value().normal_space == BakeNormalSpace::object);
    REQUIRE(reopened_object.value().artifact_digest == object_bake.artifact->artifact_digest);

    auto changed_source = source;
    REQUIRE(changed_source.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.06}));
    const auto stale_source_publish = carto::production::publish_normal_map(
        store, receipt, *bake.artifact, changed_source, target, policy);
    REQUIRE(!stale_source_publish);
    REQUIRE(store.ledger().size() == 2U);
    REQUIRE(!carto::production::read_normal_map(
        store, recipe, BakeNormalSpace::tangent, changed_source, target, policy));

    auto changed_target = target;
    REQUIRE(changed_target.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    const auto stale_target_publish = carto::production::publish_normal_map(
        store, receipt, *bake.artifact, source, changed_target, policy);
    REQUIRE(!stale_target_publish);
    REQUIRE(store.ledger().size() == 2U);
    REQUIRE(!carto::production::read_normal_map(
        store, recipe, BakeNormalSpace::tangent, source, changed_target, policy));

    auto wrong_key_receipt = receipt;
    wrong_key_receipt.key.platform_profile = "carto.cpu-different.v1";
    REQUIRE(wrong_key_receipt.refresh_digest());
    REQUIRE(!carto::production::publish_normal_map(
        store, wrong_key_receipt, *bake.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 2U);
}

void curvature_cpu_is_deterministic_signed_and_round_trips() {
    auto source = make_curvature_tetrahedron();
    auto target = make_target();
    carto::production::CurvatureRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/curvature";
    recipe.normalization_scale = 0.25;
    REQUIRE(recipe.validate());

    const auto serialized_recipe = recipe.serialize();
    const auto reopened_recipe = carto::production::CurvatureRecipe::deserialize(
        serialized_recipe);
    REQUIRE(reopened_recipe);
    REQUIRE(reopened_recipe.value().serialize() == serialized_recipe);
    REQUIRE(!carto::production::CurvatureRecipe::deserialize(
        serialized_recipe + "unknown=1\n"));
    auto invalid_recipe = recipe;
    invalid_recipe.normalization_scale = 0.0;
    REQUIRE(!invalid_recipe.validate());
    invalid_recipe.normalization_scale = std::numeric_limits<double>::infinity();
    REQUIRE(!invalid_recipe.validate());

    const auto first = carto::production::compile_curvature_cpu(recipe, source, target);
    const auto second = carto::production::compile_curvature_cpu(recipe, source, target);
    REQUIRE(first.succeeded());
    REQUIRE(second.succeeded());
    REQUIRE(first.projected_texels == first.covered_texels);
    REQUIRE(first.artifact->rgba8 == second.artifact->rgba8);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->minimum_signed_curvature > 0.0);
    REQUIRE(first.artifact->maximum_signed_curvature >=
        first.artifact->minimum_signed_curvature);
    REQUIRE(first.clamped_texels > 0U);
    REQUIRE(first.artifact->clamped_texels == first.clamped_texels);
    REQUIRE(first.artifact->maximum_quantization_error == recipe.normalization_scale / 255.0);
    REQUIRE(first.artifact->validate());

    bool saw_convex = false;
    bool saw_saturated = false;
    for (std::size_t pixel = 0U; pixel < first.artifact->rgba8.size(); pixel += 4U) {
        const auto alpha = first.artifact->rgba8[pixel + 3U];
        REQUIRE(alpha == 0U || alpha == 255U);
        if (alpha == 255U && first.artifact->rgba8[pixel] > 128U) saw_convex = true;
        if (alpha == 255U && first.artifact->rgba8[pixel] == 255U) saw_saturated = true;
    }
    REQUIRE(saw_convex);
    REQUIRE(saw_saturated);

    auto flat_source = make_triangle(
        {{{0.0, 0.0, 0.05}, {1.0, 0.0, 0.05}, {0.0, 1.0, 0.05}}}, false);
    auto flat_recipe = recipe;
    flat_recipe.binding = make_recipe(flat_source, target);
    flat_recipe.binding.identity = "bake/target/flat-curvature";
    const auto flat = carto::production::compile_curvature_cpu(
        flat_recipe, flat_source, target);
    REQUIRE(flat.succeeded());
    for (std::size_t pixel = 0U; pixel < flat.artifact->rgba8.size(); pixel += 4U) {
        if (flat.artifact->rgba8[pixel + 3U] == 255U) {
            REQUIRE(flat.artifact->rgba8[pixel] == 128U);
        }
    }

    const auto key = carto::production::bake_curvature_cook_key(recipe);
    REQUIRE(key);
    REQUIRE(key.value().algorithm == carto::production::kBakeCurvatureAlgorithm);
    auto other_scale = recipe;
    other_scale.normalization_scale = 0.5;
    const auto other_key = carto::production::bake_curvature_cook_key(other_scale);
    REQUIRE(other_key && other_key.value() != key.value());
    carto::production::ProductionCookGraph graph;
    const auto registered = carto::production::register_curvature_graph_node(graph, recipe);
    REQUIRE(registered && registered.value() == key.value());
    REQUIRE(graph.validate());

    const auto bytes = first.artifact->serialize();
    REQUIRE(bytes);
    const auto reopened = carto::production::CurvatureMapArtifact::deserialize(bytes.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == first.artifact->artifact_digest);
    REQUIRE(reopened.value().rgba8 == first.artifact->rgba8);
    auto tampered = bytes.value();
    tampered.back() ^= 1U;
    REQUIRE(!carto::production::CurvatureMapArtifact::deserialize(tampered));
    auto truncated = bytes.value();
    truncated.pop_back();
    REQUIRE(!carto::production::CurvatureMapArtifact::deserialize(truncated));
    auto trailing = bytes.value();
    trailing.push_back(0U);
    REQUIRE(!carto::production::CurvatureMapArtifact::deserialize(trailing));

    auto invalid_metric = *first.artifact;
    invalid_metric.maximum_quantization_error = 0.0;
    REQUIRE(!invalid_metric.validate());
    auto malformed_pixels = *first.artifact;
    std::size_t first_covered = malformed_pixels.rgba8.size();
    for (std::size_t pixel = 3U; pixel < malformed_pixels.rgba8.size(); pixel += 4U) {
        if (malformed_pixels.rgba8[pixel] == 255U) {
            first_covered = pixel - 3U;
            break;
        }
    }
    REQUIRE(first_covered < malformed_pixels.rgba8.size());
    malformed_pixels.rgba8[first_covered + 1U] ^= 1U;
    REQUIRE(malformed_pixels.refresh_digests());
    REQUIRE(!malformed_pixels.validate());
}

void curvature_projection_and_uv_failures_are_closed() {
    auto target = make_target();
    auto far_source = make_triangle(
        {{{0.0, 0.0, 2.0}, {1.0, 0.0, 2.0}, {0.0, 1.0, 2.0}}}, false);
    carto::production::CurvatureRecipe miss_recipe;
    miss_recipe.binding = make_recipe(far_source, target);
    miss_recipe.binding.identity = "bake/target/curvature-miss";
    const auto miss = carto::production::compile_curvature_cpu(
        miss_recipe, far_source, target);
    REQUIRE(!miss.succeeded());
    REQUIRE(!miss.artifact);
    REQUIRE(miss.projection_misses > 0U);

    auto source = make_curvature_tetrahedron();
    auto cross_tile_target = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, true, false,
        {{{0.0, 0.0}, {1.2, 0.0}, {0.0, 1.0}}});
    carto::production::CurvatureRecipe cross_tile_recipe;
    cross_tile_recipe.binding = make_recipe(source, cross_tile_target);
    cross_tile_recipe.binding.identity = "bake/target/curvature-cross-tile";
    const auto cross_tile = carto::production::compile_curvature_cpu(
        cross_tile_recipe, source, cross_tile_target);
    REQUIRE(!cross_tile.succeeded());
    REQUIRE(!cross_tile.artifact);

    auto overlapping_target = make_overlapping_target();
    carto::production::CurvatureRecipe overlap_recipe;
    overlap_recipe.binding = make_recipe(source, overlapping_target);
    overlap_recipe.binding.identity = "bake/target/curvature-overlap";
    const auto overlap = carto::production::compile_curvature_cpu(
        overlap_recipe, source, overlapping_target);
    REQUIRE(!overlap.succeeded());
    REQUIRE(!overlap.artifact);
}

void curvature_publication_revalidates_sources_and_receipt() {
    auto source = make_curvature_tetrahedron();
    auto target = make_target();
    carto::production::CurvatureRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/curvature-publish";
    recipe.normalization_scale = 0.5;
    const auto baked = carto::production::compile_curvature_cpu(recipe, source, target);
    REQUIRE(baked.succeeded());
    const auto key = carto::production::bake_curvature_cook_key(recipe);
    const auto payload = baked.artifact->serialize();
    REQUIRE(key && payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;

    REQUIRE(carto::production::publish_curvature_map(
        store, receipt, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    const auto reopened = carto::production::read_curvature_map(
        store, recipe, source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == baked.artifact->artifact_digest);

    auto changed_source = source;
    REQUIRE(changed_source.set_vertex_position(VertexId{4U}, {0.25, 0.25, 0.70}));
    REQUIRE(!carto::production::read_curvature_map(
        store, recipe, changed_source, target, policy));
    REQUIRE(!carto::production::publish_curvature_map(
        store, receipt, *baked.artifact, changed_source, target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto changed_target = target;
    REQUIRE(changed_target.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    REQUIRE(!carto::production::read_curvature_map(
        store, recipe, source, changed_target, policy));
    REQUIRE(!carto::production::publish_curvature_map(
        store, receipt, *baked.artifact, source, changed_target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto wrong_key = receipt;
    wrong_key.key.algorithm = "carto.bake.cpu.curvature.unverified";
    REQUIRE(wrong_key.refresh_digest());
    REQUIRE(!carto::production::publish_curvature_map(
        store, wrong_key, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
}

void thickness_cpu_is_deterministic_signed_by_distance_and_round_trips() {
    auto source = make_thickness_shell();
    auto target = make_target();
    carto::production::ThicknessRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/thickness";
    recipe.maximum_distance = 0.5;
    recipe.surface_bias = 1e-5;
    REQUIRE(recipe.validate());

    const std::string recipe_bytes = recipe.serialize();
    const auto reopened_recipe = carto::production::ThicknessRecipe::deserialize(recipe_bytes);
    REQUIRE(reopened_recipe);
    REQUIRE(reopened_recipe.value().serialize() == recipe_bytes);
    REQUIRE(!carto::production::ThicknessRecipe::deserialize(recipe_bytes + "unknown=1\n"));
    auto invalid_recipe = recipe;
    invalid_recipe.maximum_distance = 0.0;
    REQUIRE(!invalid_recipe.validate());
    invalid_recipe = recipe;
    invalid_recipe.surface_bias = recipe.maximum_distance;
    REQUIRE(!invalid_recipe.validate());

    const auto first = carto::production::compile_thickness_cpu(recipe, source, target);
    const auto second = carto::production::compile_thickness_cpu(recipe, source, target);
    REQUIRE(first.succeeded());
    REQUIRE(second.succeeded());
    REQUIRE(first.covered_texels == first.projected_texels);
    REQUIRE(first.projected_texels == first.thickness_hits);
    REQUIRE(first.thickness_misses == 0U);
    REQUIRE(first.artifact->rgba8 == second.artifact->rgba8);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(std::abs(first.artifact->minimum_thickness - 0.2) < 1e-9);
    REQUIRE(std::abs(first.artifact->maximum_thickness - 0.2) < 1e-9);
    REQUIRE(std::abs(first.artifact->maximum_quantization_error - 0.5 / 510.0) < 1e-15);
    REQUIRE(first.artifact->recipe.binding.source == recipe.binding.source);
    REQUIRE(first.artifact->recipe.binding.target == recipe.binding.target);
    REQUIRE(first.artifact->validate());

    bool saw_covered = false;
    for (std::size_t pixel = 0U; pixel < first.artifact->rgba8.size(); pixel += 4U) {
        const auto alpha = first.artifact->rgba8[pixel + 3U];
        REQUIRE(alpha == 0U || alpha == 255U);
        if (alpha == 255U) {
            saw_covered = true;
            REQUIRE(first.artifact->rgba8[pixel] == first.artifact->rgba8[pixel + 1U]);
            REQUIRE(first.artifact->rgba8[pixel] == first.artifact->rgba8[pixel + 2U]);
            REQUIRE(first.artifact->rgba8[pixel] == 102U);
        } else {
            REQUIRE(first.artifact->rgba8[pixel] == 0U);
            REQUIRE(first.artifact->rgba8[pixel + 1U] == 0U);
            REQUIRE(first.artifact->rgba8[pixel + 2U] == 0U);
        }
    }
    REQUIRE(saw_covered);

    const auto key = carto::production::bake_thickness_cook_key(recipe);
    REQUIRE(key);
    REQUIRE(key.value().algorithm == carto::production::kBakeThicknessAlgorithm);
    auto other_distance = recipe;
    other_distance.maximum_distance = 0.75;
    const auto other_key = carto::production::bake_thickness_cook_key(other_distance);
    REQUIRE(other_key && other_key.value() != key.value());
    auto other_bias = recipe;
    other_bias.surface_bias = 2e-5;
    const auto other_bias_key = carto::production::bake_thickness_cook_key(other_bias);
    REQUIRE(other_bias_key && other_bias_key.value() != key.value());
    carto::production::ProductionCookGraph graph;
    const auto registered = carto::production::register_thickness_graph_node(graph, recipe);
    REQUIRE(registered && registered.value() == key.value());
    REQUIRE(graph.validate());

    const auto artifact_bytes = first.artifact->serialize();
    REQUIRE(artifact_bytes);
    const auto reopened = carto::production::ThicknessMapArtifact::deserialize(
        artifact_bytes.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == first.artifact->artifact_digest);
    REQUIRE(reopened.value().rgba8 == first.artifact->rgba8);
    auto tampered = artifact_bytes.value();
    tampered.back() ^= 1U;
    REQUIRE(!carto::production::ThicknessMapArtifact::deserialize(tampered));
    auto truncated = artifact_bytes.value();
    truncated.pop_back();
    REQUIRE(!carto::production::ThicknessMapArtifact::deserialize(truncated));
    auto trailing = artifact_bytes.value();
    trailing.push_back(0U);
    REQUIRE(!carto::production::ThicknessMapArtifact::deserialize(trailing));
    auto invalid_metric = *first.artifact;
    invalid_metric.maximum_quantization_error = 0.0;
    REQUIRE(!invalid_metric.validate());
    auto malformed_pixels = *first.artifact;
    for (std::size_t pixel = 3U; pixel < malformed_pixels.rgba8.size(); pixel += 4U) {
        if (malformed_pixels.rgba8[pixel] == 255U) {
            malformed_pixels.rgba8[pixel + 1U] ^= 1U;
            break;
        }
    }
    REQUIRE(malformed_pixels.refresh_digests());
    REQUIRE(!malformed_pixels.validate());
}

void thickness_projection_exit_and_uv_failures_are_closed() {
    auto target = make_target();
    auto far_source = make_triangle(
        {{{0.0, 0.0, 2.0}, {1.0, 0.0, 2.0}, {0.0, 1.0, 2.0}}}, false);
    carto::production::ThicknessRecipe projection_recipe;
    projection_recipe.binding = make_recipe(far_source, target);
    projection_recipe.binding.identity = "bake/target/thickness-projection-miss";
    const auto projection_miss = carto::production::compile_thickness_cpu(
        projection_recipe, far_source, target);
    REQUIRE(!projection_miss.succeeded());
    REQUIRE(!projection_miss.artifact);
    REQUIRE(projection_miss.projection_misses > 0U);
    REQUIRE(has_thickness_diagnostic(projection_miss, "thickness_projection_miss"));

    auto open_source = make_triangle(
        {{{0.0, 0.0, 0.05}, {1.0, 0.0, 0.05}, {0.0, 1.0, 0.05}}}, false);
    carto::production::ThicknessRecipe exit_recipe;
    exit_recipe.binding = make_recipe(open_source, target);
    exit_recipe.binding.identity = "bake/target/thickness-exit-miss";
    const auto exit_miss = carto::production::compile_thickness_cpu(
        exit_recipe, open_source, target);
    REQUIRE(!exit_miss.succeeded());
    REQUIRE(!exit_miss.artifact);
    REQUIRE(exit_miss.thickness_misses > 0U);
    REQUIRE(has_thickness_diagnostic(exit_miss, "thickness_exit_miss"));

    auto partial_source = make_thickness_shell(true);
    carto::production::ThicknessRecipe partial_recipe;
    partial_recipe.binding = make_recipe(partial_source, target);
    partial_recipe.binding.identity = "bake/target/thickness-partial";
    partial_recipe.reject_thickness_misses = false;
    const auto partial = carto::production::compile_thickness_cpu(
        partial_recipe, partial_source, target);
    if (!partial.succeeded()) {
        std::cerr << "partial counts: " << partial.covered_texels << ' '
                  << partial.projected_texels << ' ' << partial.thickness_hits << ' '
                  << partial.thickness_misses << '\n';
        for (const auto& diagnostic : partial.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        }
    }
    REQUIRE(partial.succeeded());
    REQUIRE(partial.artifact);
    REQUIRE(partial.thickness_hits > 0U);
    REQUIRE(partial.thickness_misses > 0U);
    const auto truncated_warning = std::find_if(
        partial.diagnostics.begin(), partial.diagnostics.end(),
        [](const carto::production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "diagnostics_truncated";
        });
    REQUIRE(truncated_warning != partial.diagnostics.end());
    REQUIRE(!truncated_warning->is_error);
    REQUIRE(!partial.artifact->thickness_miss_samples.empty());
    REQUIRE(partial.artifact->validate());
    const auto partial_bytes = partial.artifact->serialize();
    REQUIRE(partial_bytes);
    const auto partial_reopened = carto::production::ThicknessMapArtifact::deserialize(
        partial_bytes.value());
    REQUIRE(partial_reopened);
    REQUIRE(partial_reopened.value().thickness_misses == partial.thickness_misses);

    auto out_of_range_recipe = partial_recipe;
    out_of_range_recipe.binding = make_recipe(make_thickness_shell(), target);
    out_of_range_recipe.binding.identity = "bake/target/thickness-out-of-range";
    out_of_range_recipe.maximum_distance = 0.1;
    const auto out_of_range = carto::production::compile_thickness_cpu(
        out_of_range_recipe, make_thickness_shell(), target);
    REQUIRE(!out_of_range.succeeded());
    REQUIRE(!out_of_range.artifact);
    REQUIRE(out_of_range.thickness_misses > 0U);

    auto source = make_thickness_shell();
    auto cross_tile_target = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, true, false,
        {{{0.0, 0.0}, {1.2, 0.0}, {0.0, 1.0}}});
    carto::production::ThicknessRecipe cross_tile_recipe;
    cross_tile_recipe.binding = make_recipe(source, cross_tile_target);
    cross_tile_recipe.binding.identity = "bake/target/thickness-cross-tile";
    const auto cross_tile = carto::production::compile_thickness_cpu(
        cross_tile_recipe, source, cross_tile_target);
    REQUIRE(!cross_tile.succeeded());
    REQUIRE(!cross_tile.artifact);

    auto overlapping_target = make_overlapping_target();
    carto::production::ThicknessRecipe overlap_recipe;
    overlap_recipe.binding = make_recipe(source, overlapping_target);
    overlap_recipe.binding.identity = "bake/target/thickness-overlap";
    const auto overlap = carto::production::compile_thickness_cpu(
        overlap_recipe, source, overlapping_target);
    REQUIRE(!overlap.succeeded());
    REQUIRE(!overlap.artifact);
}

void thickness_publication_revalidates_sources_and_key() {
    auto source = make_thickness_shell();
    auto target = make_target();
    carto::production::ThicknessRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/thickness-publish";
    recipe.maximum_distance = 0.5;
    const auto baked = carto::production::compile_thickness_cpu(recipe, source, target);
    REQUIRE(baked.succeeded());
    const auto key = carto::production::bake_thickness_cook_key(recipe);
    const auto payload = baked.artifact->serialize();
    REQUIRE(key && payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;

    REQUIRE(carto::production::publish_thickness_map(
        store, receipt, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    const auto reopened = carto::production::read_thickness_map(
        store, recipe, source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == baked.artifact->artifact_digest);
    auto changed_recipe = recipe;
    changed_recipe.maximum_distance = 0.6;
    REQUIRE(!carto::production::read_thickness_map(
        store, changed_recipe, source, target, policy));

    auto changed_source = source;
    REQUIRE(changed_source.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.06}));
    REQUIRE(!carto::production::read_thickness_map(
        store, recipe, changed_source, target, policy));
    REQUIRE(!carto::production::publish_thickness_map(
        store, receipt, *baked.artifact, changed_source, target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto changed_target = target;
    REQUIRE(changed_target.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    REQUIRE(!carto::production::read_thickness_map(
        store, recipe, source, changed_target, policy));
    REQUIRE(!carto::production::publish_thickness_map(
        store, receipt, *baked.artifact, source, changed_target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto wrong_key = receipt;
    wrong_key.key.platform_profile = "carto.cpu-different.v1";
    REQUIRE(wrong_key.refresh_digest());
    REQUIRE(!carto::production::publish_thickness_map(
        store, wrong_key, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
}

void height_displacement_cpu_is_signed_deterministic_and_round_trips() {
    auto source = make_high_surface();
    auto target = make_target();
    carto::production::HeightDisplacementRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/height-displacement";
    recipe.normalization_distance = 0.5;
    REQUIRE(recipe.validate());

    const std::string recipe_bytes = recipe.serialize();
    const auto reopened_recipe =
        carto::production::HeightDisplacementRecipe::deserialize(recipe_bytes);
    REQUIRE(reopened_recipe);
    REQUIRE(reopened_recipe.value().serialize() == recipe_bytes);
    REQUIRE(!carto::production::HeightDisplacementRecipe::deserialize(
        recipe_bytes + "unknown=1\n"));
    auto invalid_recipe = recipe;
    invalid_recipe.normalization_distance = 0.0;
    REQUIRE(!invalid_recipe.validate());
    invalid_recipe.normalization_distance = std::numeric_limits<double>::infinity();
    REQUIRE(!invalid_recipe.validate());

    const auto first = carto::production::compile_height_displacement_cpu(
        recipe, source, target);
    const auto second = carto::production::compile_height_displacement_cpu(
        recipe, source, target);
    REQUIRE(first.succeeded());
    REQUIRE(second.succeeded());
    REQUIRE(first.covered_texels == first.projected_texels);
    REQUIRE(first.artifact->rgba8 == second.artifact->rgba8);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->minimum_signed_displacement > 0.0);
    REQUIRE(first.artifact->maximum_signed_displacement >
        first.artifact->minimum_signed_displacement);
    REQUIRE(first.artifact->maximum_signed_displacement < recipe.normalization_distance);
    REQUIRE(first.artifact->clamped_texels == 0U);
    REQUIRE(std::abs(first.artifact->maximum_quantization_error - 0.5 / 255.0) < 1e-15);
    REQUIRE(first.artifact->validate());

    bool saw_positive = false;
    for (std::size_t pixel = 0U; pixel < first.artifact->rgba8.size(); pixel += 4U) {
        const auto alpha = first.artifact->rgba8[pixel + 3U];
        REQUIRE(alpha == 0U || alpha == 255U);
        if (alpha == 255U) {
            saw_positive = true;
            REQUIRE(first.artifact->rgba8[pixel] > 128U);
            REQUIRE(first.artifact->rgba8[pixel] == first.artifact->rgba8[pixel + 1U]);
            REQUIRE(first.artifact->rgba8[pixel] == first.artifact->rgba8[pixel + 2U]);
        } else {
            REQUIRE(first.artifact->rgba8[pixel] == 128U);
            REQUIRE(first.artifact->rgba8[pixel + 1U] == 128U);
            REQUIRE(first.artifact->rgba8[pixel + 2U] == 128U);
        }
    }
    REQUIRE(saw_positive);

    auto negative_source = make_triangle(
        {{{0.0, 0.0, -0.05}, {1.0, 0.0, -0.05}, {0.0, 1.0, -0.05}}}, false);
    auto negative_recipe = recipe;
    negative_recipe.binding = make_recipe(negative_source, target);
    negative_recipe.binding.identity = "bake/target/height-displacement-negative";
    const auto negative = carto::production::compile_height_displacement_cpu(
        negative_recipe, negative_source, target);
    REQUIRE(negative.succeeded());
    REQUIRE(negative.artifact->maximum_signed_displacement < 0.0);
    bool saw_negative = false;
    for (std::size_t pixel = 0U; pixel < negative.artifact->rgba8.size(); pixel += 4U) {
        if (negative.artifact->rgba8[pixel + 3U] == 255U) {
            saw_negative = true;
            REQUIRE(negative.artifact->rgba8[pixel] < 128U);
        }
    }
    REQUIRE(saw_negative);

    auto zero_source = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, false);
    auto zero_recipe = recipe;
    zero_recipe.binding = make_recipe(zero_source, target);
    zero_recipe.binding.identity = "bake/target/height-displacement-zero";
    const auto zero = carto::production::compile_height_displacement_cpu(
        zero_recipe, zero_source, target);
    REQUIRE(zero.succeeded());
    for (std::size_t pixel = 3U; pixel < zero.artifact->rgba8.size(); pixel += 4U) {
        if (zero.artifact->rgba8[pixel] == 255U) {
            REQUIRE(zero.artifact->rgba8[pixel - 3U] == 128U);
        }
    }

    auto clamped_recipe = recipe;
    clamped_recipe.normalization_distance = 0.01;
    const auto rejected_clamp = carto::production::compile_height_displacement_cpu(
        clamped_recipe, source, target);
    REQUIRE(!rejected_clamp.succeeded());
    REQUIRE(!rejected_clamp.artifact);
    REQUIRE(rejected_clamp.clamped_texels > 0U);
    REQUIRE(has_height_displacement_diagnostic(
        rejected_clamp, "height_displacement_clamped"));
    clamped_recipe.reject_clamped_samples = false;
    const auto allowed_clamp = carto::production::compile_height_displacement_cpu(
        clamped_recipe, source, target);
    REQUIRE(allowed_clamp.succeeded());
    REQUIRE(allowed_clamp.artifact->clamped_texels > 0U);
    REQUIRE(allowed_clamp.artifact->maximum_signed_displacement >
        clamped_recipe.normalization_distance);
    REQUIRE(allowed_clamp.artifact->validate());

    const auto key = carto::production::bake_height_displacement_cook_key(recipe);
    REQUIRE(key);
    REQUIRE(key.value().algorithm ==
        carto::production::kBakeHeightDisplacementAlgorithm);
    auto changed_range = recipe;
    changed_range.normalization_distance = 0.75;
    const auto changed_key =
        carto::production::bake_height_displacement_cook_key(changed_range);
    REQUIRE(changed_key && changed_key.value() != key.value());
    auto changed_clamp_policy = recipe;
    changed_clamp_policy.reject_clamped_samples = false;
    const auto changed_clamp_key =
        carto::production::bake_height_displacement_cook_key(changed_clamp_policy);
    REQUIRE(changed_clamp_key && changed_clamp_key.value() != key.value());
    carto::production::ProductionCookGraph graph;
    const auto registered =
        carto::production::register_height_displacement_graph_node(graph, recipe);
    REQUIRE(registered && registered.value() == key.value());
    REQUIRE(graph.validate());

    const auto artifact_bytes = first.artifact->serialize();
    REQUIRE(artifact_bytes);
    const auto reopened = carto::production::HeightDisplacementMapArtifact::deserialize(
        artifact_bytes.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == first.artifact->artifact_digest);
    REQUIRE(reopened.value().rgba8 == first.artifact->rgba8);
    auto tampered = artifact_bytes.value();
    tampered.back() ^= 1U;
    REQUIRE(!carto::production::HeightDisplacementMapArtifact::deserialize(tampered));
    auto truncated = artifact_bytes.value();
    truncated.pop_back();
    REQUIRE(!carto::production::HeightDisplacementMapArtifact::deserialize(truncated));
    auto trailing = artifact_bytes.value();
    trailing.push_back(0U);
    REQUIRE(!carto::production::HeightDisplacementMapArtifact::deserialize(trailing));
    auto malformed_pixels = *first.artifact;
    for (std::size_t pixel = 3U; pixel < malformed_pixels.rgba8.size(); pixel += 4U) {
        if (malformed_pixels.rgba8[pixel] == 255U) {
            malformed_pixels.rgba8[pixel + 1U] ^= 1U;
            break;
        }
    }
    REQUIRE(malformed_pixels.refresh_digests());
    REQUIRE(!malformed_pixels.validate());
    auto forged_clamp_metric = *first.artifact;
    forged_clamp_metric.minimum_signed_displacement =
        recipe.normalization_distance + 0.01;
    forged_clamp_metric.maximum_signed_displacement =
        recipe.normalization_distance + 0.01;
    REQUIRE(forged_clamp_metric.refresh_digests());
    REQUIRE(!forged_clamp_metric.validate());
}

void height_displacement_projection_and_uv_failures_are_closed() {
    auto target = make_target();
    auto far_source = make_triangle(
        {{{0.0, 0.0, 2.0}, {1.0, 0.0, 2.0}, {0.0, 1.0, 2.0}}}, false);
    carto::production::HeightDisplacementRecipe miss_recipe;
    miss_recipe.binding = make_recipe(far_source, target);
    miss_recipe.binding.identity = "bake/target/height-displacement-miss";
    const auto miss = carto::production::compile_height_displacement_cpu(
        miss_recipe, far_source, target);
    REQUIRE(!miss.succeeded());
    REQUIRE(!miss.artifact);
    REQUIRE(miss.projection_misses > 0U);
    REQUIRE(has_height_displacement_diagnostic(
        miss, "height_displacement_projection_miss"));

    auto partial_source = make_triangle(
        {{{0.0, 0.0, 0.05}, {0.5, 0.0, 0.05}, {0.0, 0.5, 0.05}}}, false);
    carto::production::HeightDisplacementRecipe partial_recipe;
    partial_recipe.binding = make_recipe(partial_source, target);
    partial_recipe.binding.identity = "bake/target/height-displacement-partial";
    partial_recipe.binding.reject_projection_misses = false;
    const auto partial = carto::production::compile_height_displacement_cpu(
        partial_recipe, partial_source, target);
    REQUIRE(partial.succeeded());
    REQUIRE(partial.projected_texels > 0U);
    REQUIRE(partial.projection_misses > 0U);
    REQUIRE(!partial.artifact->projection_miss_samples.empty());
    REQUIRE(partial.artifact->validate());

    auto source = make_high_surface();
    auto cross_tile_target = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, true, false,
        {{{0.0, 0.0}, {1.2, 0.0}, {0.0, 1.0}}});
    carto::production::HeightDisplacementRecipe cross_tile_recipe;
    cross_tile_recipe.binding = make_recipe(source, cross_tile_target);
    cross_tile_recipe.binding.identity = "bake/target/height-displacement-cross-tile";
    const auto cross_tile = carto::production::compile_height_displacement_cpu(
        cross_tile_recipe, source, cross_tile_target);
    REQUIRE(!cross_tile.succeeded());
    REQUIRE(!cross_tile.artifact);

    auto overlapping_target = make_overlapping_target();
    carto::production::HeightDisplacementRecipe overlap_recipe;
    overlap_recipe.binding = make_recipe(source, overlapping_target);
    overlap_recipe.binding.identity = "bake/target/height-displacement-overlap";
    const auto overlap = carto::production::compile_height_displacement_cpu(
        overlap_recipe, source, overlapping_target);
    REQUIRE(!overlap.succeeded());
    REQUIRE(!overlap.artifact);
}

void height_displacement_publication_revalidates_sources_and_key() {
    auto source = make_high_surface();
    auto target = make_target();
    carto::production::HeightDisplacementRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/height-displacement-publish";
    recipe.normalization_distance = 0.5;
    const auto baked = carto::production::compile_height_displacement_cpu(
        recipe, source, target);
    REQUIRE(baked.succeeded());
    const auto key = carto::production::bake_height_displacement_cook_key(recipe);
    const auto payload = baked.artifact->serialize();
    REQUIRE(key && payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;

    REQUIRE(carto::production::publish_height_displacement_map(
        store, receipt, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    const auto reopened = carto::production::read_height_displacement_map(
        store, recipe, source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == baked.artifact->artifact_digest);

    auto changed_recipe = recipe;
    changed_recipe.normalization_distance = 0.75;
    REQUIRE(!carto::production::read_height_displacement_map(
        store, changed_recipe, source, target, policy));
    auto changed_source = source;
    REQUIRE(changed_source.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.06}));
    REQUIRE(!carto::production::read_height_displacement_map(
        store, recipe, changed_source, target, policy));
    REQUIRE(!carto::production::publish_height_displacement_map(
        store, receipt, *baked.artifact, changed_source, target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto changed_target = target;
    REQUIRE(changed_target.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    REQUIRE(!carto::production::read_height_displacement_map(
        store, recipe, source, changed_target, policy));
    REQUIRE(!carto::production::publish_height_displacement_map(
        store, receipt, *baked.artifact, source, changed_target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto wrong_key = receipt;
    wrong_key.key.platform_profile = "carto.cpu-different.v1";
    REQUIRE(wrong_key.refresh_digest());
    REQUIRE(!carto::production::publish_height_displacement_map(
        store, wrong_key, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
}

void bent_normal_cpu_is_deterministic_tangent_space_and_round_trips() {
    auto source = make_ao_occluder_scene();
    auto target = make_target();
    carto::production::AmbientOcclusionRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/bent";
    recipe.sample_count = 64U;
    recipe.max_distance = 0.8;
    recipe.origin_bias = 1e-4;
    recipe.seed = 0x5a17U;
    REQUIRE(recipe.validate());

    const auto first = carto::production::compile_bent_normal_cpu(recipe, source, target);
    const auto second = carto::production::compile_bent_normal_cpu(recipe, source, target);
    REQUIRE(first.succeeded());
    REQUIRE(second.succeeded());
    REQUIRE(first.covered_texels > 0U);
    REQUIRE(first.rays_cast == first.covered_texels * recipe.sample_count);
    REQUIRE(first.occluded_rays > 0U);
    REQUIRE(first.artifact->rgba8 == second.artifact->rgba8);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->algorithm == carto::production::kBakeBentNormalAlgorithm);
    REQUIRE(first.artifact->validate());

    auto open_recipe = recipe;
    const auto open_source = bind_bake_source(recipe.binding.source.identity, target);
    REQUIRE(open_source);
    open_recipe.binding.source = open_source.value();
    const auto open_bake = carto::production::compile_bent_normal_cpu(
        open_recipe, target, target);
    REQUIRE(open_bake.succeeded());
    bool blocker_changed_bent_direction = false;
    for (std::size_t pixel = 0U; pixel < first.artifact->rgba8.size(); pixel += 4U) {
        if (first.artifact->rgba8[pixel + 3U] == 255U &&
            (first.artifact->rgba8[pixel] != open_bake.artifact->rgba8[pixel] ||
             first.artifact->rgba8[pixel + 1U] != open_bake.artifact->rgba8[pixel + 1U] ||
             first.artifact->rgba8[pixel + 2U] != open_bake.artifact->rgba8[pixel + 2U])) {
            blocker_changed_bent_direction = true;
            break;
        }
    }
    REQUIRE(blocker_changed_bent_direction);

    auto directx_recipe = recipe;
    directx_recipe.binding.normal_convention =
        carto::production::BakeNormalConvention::directx_y_minus;
    const auto directx = carto::production::compile_bent_normal_cpu(
        directx_recipe, source, target);
    REQUIRE(directx.succeeded());
    REQUIRE(directx.artifact->rgba8.size() == first.artifact->rgba8.size());
    for (std::size_t pixel = 0U; pixel < first.artifact->rgba8.size(); pixel += 4U) {
        REQUIRE(directx.artifact->rgba8[pixel + 3U] == first.artifact->rgba8[pixel + 3U]);
        if (first.artifact->rgba8[pixel + 3U] == 255U) {
            REQUIRE(directx.artifact->rgba8[pixel] == first.artifact->rgba8[pixel]);
            const int y_sum = static_cast<int>(directx.artifact->rgba8[pixel + 1U]) +
                static_cast<int>(first.artifact->rgba8[pixel + 1U]);
            REQUIRE(std::abs(y_sum - 255) <= 1);
            REQUIRE(directx.artifact->rgba8[pixel + 2U] == first.artifact->rgba8[pixel + 2U]);
        }
    }

    auto invalid_metric = *first.artifact;
    invalid_metric.sampling_standard_error_upper_bound = 0.0;
    REQUIRE(!invalid_metric.validate());
    auto unsupported_algorithm = *first.artifact;
    unsupported_algorithm.algorithm = "carto.bake.cpu.bent-normal.unverified";
    REQUIRE(!unsupported_algorithm.validate());
    auto invalid_encoding = *first.artifact;
    std::size_t first_covered = invalid_encoding.rgba8.size();
    for (std::size_t pixel = 3U; pixel < invalid_encoding.rgba8.size(); pixel += 4U) {
        if (invalid_encoding.rgba8[pixel] == 255U) {
            first_covered = pixel - 3U;
            break;
        }
    }
    REQUIRE(first_covered < invalid_encoding.rgba8.size());
    invalid_encoding.rgba8[first_covered] = 0U;
    REQUIRE(invalid_encoding.refresh_digests());
    REQUIRE(!invalid_encoding.validate());

    const auto key = carto::production::bake_bent_normal_cook_key(recipe);
    REQUIRE(key);
    REQUIRE(key.value().algorithm == carto::production::kBakeBentNormalAlgorithm);
    auto other_sampling = recipe;
    ++other_sampling.seed;
    const auto other_key = carto::production::bake_bent_normal_cook_key(other_sampling);
    REQUIRE(other_key && other_key.value() != key.value());
    carto::production::ProductionCookGraph graph;
    const auto registered = carto::production::register_bent_normal_graph_node(graph, recipe);
    REQUIRE(registered && registered.value() == key.value());
    REQUIRE(graph.validate());

    const auto bytes = first.artifact->serialize();
    REQUIRE(bytes);
    const auto reopened = carto::production::BentNormalMapArtifact::deserialize(bytes.value());
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == first.artifact->artifact_digest);
    REQUIRE(reopened.value().rgba8 == first.artifact->rgba8);
    auto tampered = bytes.value();
    tampered.back() ^= 1U;
    REQUIRE(!carto::production::BentNormalMapArtifact::deserialize(tampered));
    auto truncated = bytes.value();
    truncated.pop_back();
    REQUIRE(!carto::production::BentNormalMapArtifact::deserialize(truncated));
    auto trailing = bytes.value();
    trailing.push_back(0U);
    REQUIRE(!carto::production::BentNormalMapArtifact::deserialize(trailing));
}

void bent_normal_publication_revalidates_sources_and_key() {
    auto source = make_ao_occluder_scene();
    auto target = make_target();
    carto::production::AmbientOcclusionRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/bent-publish";
    recipe.sample_count = 16U;
    recipe.max_distance = 0.8;
    const auto baked = carto::production::compile_bent_normal_cpu(recipe, source, target);
    REQUIRE(baked.succeeded());
    const auto key = carto::production::bake_bent_normal_cook_key(recipe);
    const auto payload = baked.artifact->serialize();
    REQUIRE(key && payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;

    REQUIRE(carto::production::publish_bent_normal_map(
        store, receipt, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
    const auto reopened = carto::production::read_bent_normal_map(
        store, recipe, source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == baked.artifact->artifact_digest);

    auto changed_source = source;
    REQUIRE(changed_source.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    REQUIRE(!carto::production::read_bent_normal_map(
        store, recipe, changed_source, target, policy));
    REQUIRE(!carto::production::publish_bent_normal_map(
        store, receipt, *baked.artifact, changed_source, target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto changed_target = target;
    REQUIRE(changed_target.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    REQUIRE(!carto::production::read_bent_normal_map(
        store, recipe, source, changed_target, policy));
    REQUIRE(!carto::production::publish_bent_normal_map(
        store, receipt, *baked.artifact, source, changed_target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto wrong_key = receipt;
    wrong_key.key.platform_profile = "carto.cpu-different.v1";
    REQUIRE(wrong_key.refresh_digest());
    REQUIRE(!carto::production::publish_bent_normal_map(
        store, wrong_key, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
}

void ambient_occlusion_publication_revalidates_sources_and_key() {
    auto source = make_ao_occluder_scene();
    auto target = make_target();
    carto::production::AmbientOcclusionRecipe recipe;
    recipe.binding = make_recipe(source, target);
    recipe.binding.identity = "bake/target/ao-publish";
    recipe.sample_count = 16U;
    recipe.max_distance = 0.8;
    recipe.seed = 19U;
    const auto baked = carto::production::compile_ambient_occlusion_cpu(
        recipe, source, target);
    REQUIRE(baked.succeeded());
    const auto key = carto::production::bake_ambient_occlusion_cook_key(recipe);
    const auto payload = baked.artifact->serialize();
    REQUIRE(key && payload);

    TempDirectory directory;
    carto::assets::BlobStore blobs(directory.path() / "blobs");
    carto::production::ProductionArtifactStore store(blobs);
    carto::production::ProductionArtifactReceipt receipt;
    receipt.key = key.value();
    receipt.status = carto::production::ReceiptStatus::admitted;
    receipt.output_digest = carto::assets::sha256(payload.value());
    receipt.output_bytes = static_cast<std::uint64_t>(payload.value().size());
    receipt.quality.confidence = 1.0;
    REQUIRE(receipt.refresh_digest());
    carto::production::AdmissionPolicy policy;

    REQUIRE(carto::production::publish_ambient_occlusion_map(
        store, receipt, *baked.artifact, source, target, policy));
    const auto reopened = carto::production::read_ambient_occlusion_map(
        store, recipe, source, target, policy);
    REQUIRE(reopened);
    REQUIRE(reopened.value().artifact_digest == baked.artifact->artifact_digest);
    REQUIRE(store.ledger().size() == 1U);

    auto changed_target = target;
    REQUIRE(changed_target.set_vertex_position(VertexId{1U}, {0.0, 0.0, 0.01}));
    REQUIRE(!carto::production::read_ambient_occlusion_map(
        store, recipe, source, changed_target, policy));
    REQUIRE(!carto::production::publish_ambient_occlusion_map(
        store, receipt, *baked.artifact, source, changed_target, policy));
    REQUIRE(store.ledger().size() == 1U);

    auto wrong_key = receipt;
    wrong_key.key.algorithm = "carto.bake.cpu.ambient-occlusion.other";
    REQUIRE(wrong_key.refresh_digest());
    REQUIRE(!carto::production::publish_ambient_occlusion_map(
        store, wrong_key, *baked.artifact, source, target, policy));
    REQUIRE(store.ledger().size() == 1U);
}

void world_position_map_is_snapshot_bound_and_hostile_inputs_fail_closed() {
    using namespace carto;
    const core::Transform world_transform{
        {10.0, 2.0, -1.0}, {0.0, 0.0, 1.0, 0.0}, {2.0, 3.0, 4.0}};
    WorldPositionProject fixture(make_target(), world_transform);
    const auto recipe = bind_world_position_recipe(
        "bake/node/world-position", fixture.document, fixture.node.value, 7U, 16U, 16U);
    REQUIRE(recipe);
    const std::string recipe_text = recipe.value().serialize();
    const auto reopened_recipe = production::WorldPositionRecipe::deserialize(recipe_text);
    REQUIRE(reopened_recipe && reopened_recipe.value().serialize() == recipe_text);
    REQUIRE(reopened_recipe.value().source_namespace == fixture.document.source_namespace());

    const auto first = compile_world_position_cpu(recipe.value(), fixture.document);
    const auto second = compile_world_position_cpu(recipe.value(), fixture.document);
    REQUIRE(first.succeeded() && second.succeeded());
    REQUIRE(first.covered_texels == second.covered_texels && first.covered_texels > 0U);
    REQUIRE(first.artifact->output_bytes == second.artifact->output_bytes);
    REQUIRE(first.artifact->output_digest == second.artifact->output_digest);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->validate());

    const std::size_t first_texel = 0U;
    const std::uint64_t face = read_u64_le(first.artifact->output_bytes, first_texel);
    const double x = std::bit_cast<double>(read_u64_le(first.artifact->output_bytes, first_texel + 8U));
    const double y = std::bit_cast<double>(read_u64_le(first.artifact->output_bytes, first_texel + 16U));
    const double z = std::bit_cast<double>(read_u64_le(first.artifact->output_bytes, first_texel + 24U));
    REQUIRE(face == 1U);
    REQUIRE(x == 10.0 - 2.0 / 32.0 && y == 2.0 - 3.0 / 32.0 && z == -1.0);

    const auto encoded = first.artifact->serialize();
    REQUIRE(encoded);
    const auto reopened_artifact = production::WorldPositionMapArtifact::deserialize(encoded.value());
    REQUIRE(reopened_artifact &&
        reopened_artifact.value().artifact_digest == first.artifact->artifact_digest);
    auto tampered = encoded.value();
    tampered[tampered.size() / 2U] ^= 0x01U;
    REQUIRE(!production::WorldPositionMapArtifact::deserialize(tampered));
    REQUIRE(!production::WorldPositionRecipe::deserialize(recipe_text + "unknown=1\n"));

    const auto saved = fixture.document.save_atomic(fixture.directory.path() / "world.carto");
    REQUIRE(saved);
    const auto reopened_project = project::ProjectDocument::load(
        fixture.directory.path() / "world.carto");
    REQUIRE(reopened_project);
    const auto reopened_binding = bind_world_position_recipe(
        "bake/node/world-position", reopened_project.value(), fixture.node.value, 7U, 16U, 16U);
    REQUIRE(reopened_binding && reopened_binding.value().serialize() == recipe_text);
    const auto reopened_output = compile_world_position_cpu(
        reopened_binding.value(), reopened_project.value());
    REQUIRE(reopened_output.succeeded() &&
        reopened_output.artifact->artifact_digest == first.artifact->artifact_digest);

    ProductionCookGraph graph;
    const auto cook_key = register_world_position_graph_node(graph, recipe.value());
    REQUIRE(cook_key && graph.validate() && graph.size() == 1U);
    const auto same_key = production::world_position_cook_key(recipe.value());
    REQUIRE(same_key && same_key.value() == cook_key.value());

    auto forged_transform = recipe.value();
    forged_transform.target_world_transform.translation.x += 1.0;
    REQUIRE(!production::validate_world_position_source(forged_transform, fixture.document));
    REQUIRE(!compile_world_position_cpu(forged_transform, fixture.document).succeeded());

    journal::Journal journal(fixture.directory.path() / "world-position-journal.log");
    auto transaction = project::ProjectTransaction::begin(
        fixture.document, journal, "bake-world-position-test");
    REQUIRE(transaction);
    REQUIRE(transaction.value().set_object_transform(
        fixture.node, core::Transform{{11.0, 2.0, -1.0}}));
    REQUIRE(transaction.value().commit("bake.world-position.transform"));
    const auto stale = compile_world_position_cpu(recipe.value(), fixture.document);
    REQUIRE(!stale.succeeded() && !stale.artifact.has_value());

    WorldPositionProject missing_uv(make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, false));
    REQUIRE(!bind_world_position_recipe(
        "bake/missing-uv", missing_uv.document, missing_uv.node.value, 7U, 8U, 8U));

    WorldPositionProject overlap(make_overlapping_target());
    const auto overlap_recipe = bind_world_position_recipe(
        "bake/overlap", overlap.document, overlap.node.value, 7U, 8U, 8U);
    REQUIRE(overlap_recipe);
    const auto overlap_report = compile_world_position_cpu(
        overlap_recipe.value(), overlap.document);
    REQUIRE(!overlap_report.succeeded() && !overlap_report.artifact.has_value());
    REQUIRE(std::any_of(overlap_report.diagnostics.begin(), overlap_report.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "overlapping_uv_islands";
        }));

    EditableMesh cross_tile_mesh = make_triangle(
        {{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}, true, false,
        {{{0.0, 0.0}, {1.25, 0.0}, {0.0, 1.0}}});
    WorldPositionProject cross_tile(std::move(cross_tile_mesh));
    const auto cross_tile_recipe = bind_world_position_recipe(
        "bake/cross-tile", cross_tile.document, cross_tile.node.value, 7U, 8U, 8U);
    REQUIRE(cross_tile_recipe);
    const auto cross_tile_report = compile_world_position_cpu(
        cross_tile_recipe.value(), cross_tile.document);
    REQUIRE(!cross_tile_report.succeeded() && !cross_tile_report.artifact.has_value());
}

void material_surface_id_map_is_snapshot_bound_and_hostile_inputs_fail_closed() {
    using namespace carto;
    MaterialSurfaceIdProject fixture(make_material_surface_target());
    const auto domains = fixture.domains();
    const auto recipe = production::bind_material_surface_id_recipe(
        "bake/asset/material-surface-ids", fixture.document, fixture.mesh_asset,
        1U, "condition.material_region", 16U, 16U, domains);
    REQUIRE(recipe);
    const std::string recipe_text = recipe.value().serialize();
    const auto decoded_recipe = production::MaterialSurfaceIdRecipe::deserialize(recipe_text);
    REQUIRE(decoded_recipe && decoded_recipe.value().serialize() == recipe_text);
    REQUIRE(!production::MaterialSurfaceIdRecipe::deserialize(recipe_text + "unknown=1\n"));

    const auto first = production::compile_material_surface_id_cpu(
        recipe.value(), fixture.document);
    const auto second = production::compile_material_surface_id_cpu(
        recipe.value(), fixture.document);
    REQUIRE(first.succeeded() && second.succeeded());
    REQUIRE(first.covered_texels > 0U && first.covered_texels == second.covered_texels);
    REQUIRE(first.artifact->output_bytes == second.artifact->output_bytes);
    REQUIRE(first.artifact->output_digest == second.artifact->output_digest);
    REQUIRE(first.artifact->artifact_digest == second.artifact->artifact_digest);
    REQUIRE(first.artifact->validate());

    auto self_consistent_wrong_material = *first.artifact;
    bool changed_material = false;
    for (std::size_t offset = 0U; offset < self_consistent_wrong_material.output_bytes.size();
         offset += production::kMaterialSurfaceIdTexelBytes) {
        if (read_u64_le(self_consistent_wrong_material.output_bytes, offset) == 1U) {
            self_consistent_wrong_material.output_bytes[offset + 8U] = 72U;
            changed_material = true;
            break;
        }
    }
    REQUIRE(changed_material && self_consistent_wrong_material.refresh_digests());
    REQUIRE(self_consistent_wrong_material.validate());
    REQUIRE(!production::validate_material_surface_id_artifact_source(
        self_consistent_wrong_material, fixture.document));

    bool saw_paint = false;
    bool saw_rubber = false;
    for (std::size_t offset = 0U; offset < first.artifact->output_bytes.size();
         offset += production::kMaterialSurfaceIdTexelBytes) {
        const auto& bytes = first.artifact->output_bytes;
        const std::uint64_t face = read_u64_le(bytes, offset);
        if (face == 1U) {
            REQUIRE(read_u64_le(bytes, offset + 8U) == 71U);
            REQUIRE(read_u64_le(bytes, offset + 16U) == 901U);
            REQUIRE(read_u64_le(bytes, offset + 24U) == 101U);
            REQUIRE(bytes[offset + 32U] == 3U);
            saw_paint = true;
        } else if (face == 2U) {
            REQUIRE(read_u64_le(bytes, offset + 8U) == 72U);
            REQUIRE(read_u64_le(bytes, offset + 16U) == 0U);
            REQUIRE(read_u64_le(bytes, offset + 24U) == 102U);
            REQUIRE(bytes[offset + 32U] == 9U);
            saw_rubber = true;
        }
        if (face != 0U) REQUIRE(bytes[offset + 36U] == 0U);
    }
    REQUIRE(saw_paint && saw_rubber);

    const auto serialized = first.artifact->serialize();
    REQUIRE(serialized);
    const auto reopened_artifact = production::MaterialSurfaceIdMapArtifact::deserialize(
        serialized.value());
    REQUIRE(reopened_artifact && reopened_artifact.value().artifact_digest ==
        first.artifact->artifact_digest);
    auto tampered = serialized.value();
    tampered.back() ^= 0x01U;
    REQUIRE(!production::MaterialSurfaceIdMapArtifact::deserialize(tampered));
    auto malformed_texel = *first.artifact;
    const auto covered_offset = std::find_if(
        malformed_texel.output_bytes.begin(), malformed_texel.output_bytes.end(),
        [](std::uint8_t value) { return value != 0U; });
    REQUIRE(covered_offset != malformed_texel.output_bytes.end());
    const std::size_t first_nonzero = static_cast<std::size_t>(
        std::distance(malformed_texel.output_bytes.begin(), covered_offset));
    const std::size_t record_offset = first_nonzero - first_nonzero %
        production::kMaterialSurfaceIdTexelBytes;
    malformed_texel.output_bytes[record_offset + 36U] = 1U;
    REQUIRE(!malformed_texel.refresh_digests());

    const auto saved = fixture.document.save_atomic(
        fixture.directory.path() / "material-surface-id.carto");
    REQUIRE(saved);
    const auto reopened_project = project::ProjectDocument::load(
        fixture.directory.path() / "material-surface-id.carto");
    REQUIRE(reopened_project);
    const auto rebound = production::bind_material_surface_id_recipe(
        "bake/asset/material-surface-ids", reopened_project.value(),
        fixture.mesh_asset, 1U, "condition.material_region", 16U, 16U, domains);
    REQUIRE(rebound && rebound.value().serialize() == recipe_text);
    const auto reopened_bake = production::compile_material_surface_id_cpu(
        rebound.value(), reopened_project.value());
    REQUIRE(reopened_bake.succeeded() &&
        reopened_bake.artifact->artifact_digest == first.artifact->artifact_digest);

    ProductionCookGraph graph;
    const auto cook_key = production::register_material_surface_id_graph_node(
        graph, recipe.value());
    REQUIRE(cook_key && graph.validate() && graph.size() == 1U);
    const auto same_key = production::material_surface_id_cook_key(recipe.value());
    REQUIRE(same_key && same_key.value() == cook_key.value());

    auto stale_revision = recipe.value();
    stale_revision.source_revision = core::Revision{
        fixture.document.revision().value() + 1U};
    REQUIRE(!production::validate_material_surface_id_source(
        stale_revision, fixture.document));
    REQUIRE(!production::compile_material_surface_id_cpu(
        stale_revision, fixture.document).succeeded());

    auto missing_layer = recipe.value();
    missing_layer.material_region_layer = "stale.material_region";
    REQUIRE(!production::validate_material_surface_id_source(
        missing_layer, fixture.document));

    auto uncovered = recipe.value();
    uncovered.surface_domains.pop_back();
    REQUIRE(uncovered.validate());
    REQUIRE(!production::validate_material_surface_id_source(uncovered, fixture.document));

    auto overlapping_domains = recipe.value();
    overlapping_domains.surface_domains[1U].source_faces = {FaceId{1U}};
    const auto compiled_mesh = fixture.document.meshes().at(fixture.mesh_asset).compile();
    REQUIRE(compiled_mesh);
    REQUIRE(overlapping_domains.surface_domains[1U].refresh_digest(compiled_mesh.value()));
    REQUIRE(!overlapping_domains.validate());

    MaterialSurfaceIdProject missing_assignment(
        make_material_surface_target(), true);
    REQUIRE(!production::bind_material_surface_id_recipe(
        "bake/missing-assignment", missing_assignment.document,
        missing_assignment.mesh_asset, 1U, "condition.material_region", 8U, 8U,
        missing_assignment.domains()));

    MaterialSurfaceIdProject missing_uv(
        make_material_surface_target(false, false, false));
    REQUIRE(!production::bind_material_surface_id_recipe(
        "bake/missing-uv", missing_uv.document, missing_uv.mesh_asset,
        1U, "condition.material_region", 8U, 8U, missing_uv.domains()));

    MaterialSurfaceIdProject overlapping_uvs(make_material_surface_target(true));
    const auto overlapping_recipe = production::bind_material_surface_id_recipe(
        "bake/overlap", overlapping_uvs.document, overlapping_uvs.mesh_asset,
        1U, "condition.material_region", 8U, 8U, overlapping_uvs.domains());
    REQUIRE(overlapping_recipe);
    const auto overlap_report = production::compile_material_surface_id_cpu(
        overlapping_recipe.value(), overlapping_uvs.document);
    REQUIRE(!overlap_report.succeeded() && !overlap_report.artifact.has_value());
    REQUIRE(std::any_of(overlap_report.diagnostics.begin(), overlap_report.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "overlapping_uv_islands";
        }));

    MaterialSurfaceIdProject cross_tile_uvs(make_material_surface_target(false, true));
    const auto cross_tile_recipe = production::bind_material_surface_id_recipe(
        "bake/cross-tile", cross_tile_uvs.document, cross_tile_uvs.mesh_asset,
        1U, "condition.material_region", 8U, 8U, cross_tile_uvs.domains());
    REQUIRE(cross_tile_recipe);
    const auto cross_tile_report = production::compile_material_surface_id_cpu(
        cross_tile_recipe.value(), cross_tile_uvs.document);
    REQUIRE(!cross_tile_report.succeeded() && !cross_tile_report.artifact.has_value());
    REQUIRE(std::any_of(cross_tile_report.diagnostics.begin(), cross_tile_report.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "cross_tile_uv_triangle";
        }));
}

} // namespace

int main() {
    try {
        recipe_serialization_is_canonical_and_rejects_hostile_fields();
        cpu_normal_bake_is_deterministic_and_round_trips();
        tangent_normal_bake_uses_the_shared_uv0_tangent_domain();
        tangent_normal_bake_reports_uv_seam_frame_discontinuity();
        tangent_normal_bake_reports_projection_cage_crossing_without_rejecting();
        object_space_normal_bake_has_distinct_encoding_and_identity();
        stale_sources_and_projection_misses_fail_closed_without_mutation();
        same_revision_content_drift_invalidates_source_and_target_bindings();
        ambient_occlusion_cpu_is_deterministic_and_round_trips();
        curvature_cpu_is_deterministic_signed_and_round_trips();
        curvature_projection_and_uv_failures_are_closed();
        curvature_publication_revalidates_sources_and_receipt();
        thickness_cpu_is_deterministic_signed_by_distance_and_round_trips();
        thickness_projection_exit_and_uv_failures_are_closed();
        thickness_publication_revalidates_sources_and_key();
        height_displacement_cpu_is_signed_deterministic_and_round_trips();
        height_displacement_projection_and_uv_failures_are_closed();
        height_displacement_publication_revalidates_sources_and_key();
        bent_normal_cpu_is_deterministic_tangent_space_and_round_trips();
        bent_normal_publication_revalidates_sources_and_key();
        backface_and_uv_overlap_policies_are_explicit();
        cross_tile_uv_triangles_fail_closed();
        bake_nodes_use_production_cook_identity_and_invalidation();
        normal_map_publication_revalidates_both_meshes_and_receipt_identity();
        ambient_occlusion_publication_revalidates_sources_and_key();
        world_position_map_is_snapshot_bound_and_hostile_inputs_fail_closed();
        material_surface_id_map_is_snapshot_bound_and_hostile_inputs_fail_closed();
        std::cout << "BakeGraph CPU reference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
