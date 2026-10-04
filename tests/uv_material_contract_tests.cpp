#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/render/gpu_abi.hpp>
#include <carto/render/material.hpp>
#include <carto/render/tangent.hpp>

#include "project_document_access.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const char* message) : std::runtime_error(message) {}
};

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw TestFailure(#condition); \
    } while (false)

void uv_identity_reuses_corner_attribute_storage() {
    using carto::attributes::AttributeDomain;
    using carto::attributes::AttributeLayer;
    using carto::attributes::AttributeSet;
    using carto::attributes::AttributeType;
    using carto::attributes::UvSetDescriptor;
    using carto::attributes::UvSetTable;

    AttributeSet attributes({{AttributeDomain::corner, 3U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}, carto::core::Vec2d{1.0, 0.0},
         carto::core::Vec2d{0.0, 1.0}}}));
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv1.corners", AttributeDomain::corner, AttributeType::vec2},
        {carto::core::Vec2d{0.1, 0.1}, carto::core::Vec2d{0.9, 0.1},
         carto::core::Vec2d{0.1, 0.9}}}));

    UvSetTable table;
    REQUIRE(table.add(UvSetDescriptor{1U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(table.add(UvSetDescriptor{2U, "Lightmap", "uv1.corners", false, false, true, 1U}));
    REQUIRE(table.validate(attributes));
    REQUIRE(table.active_for_editing()->id == 1U);
    REQUIRE(table.active_for_render()->id == 1U);
    REQUIRE(table.set_active_for_editing(2U));
    REQUIRE(table.set_active_for_render(2U));
    REQUIRE(table.active_for_editing()->id == 2U);
    REQUIRE(table.active_for_render()->id == 2U);
    const std::array<carto::attributes::UvSetId, 1U> referenced{2U};
    REQUIRE(!table.remove(2U, referenced));
    REQUIRE(table.validate(attributes));
}

void uv_identity_rejects_ambiguous_or_wrong_layers() {
    using carto::attributes::AttributeDomain;
    using carto::attributes::AttributeLayer;
    using carto::attributes::AttributeSet;
    using carto::attributes::AttributeType;
    using carto::attributes::UvSetDescriptor;
    using carto::attributes::UvSetTable;

    AttributeSet attributes({{AttributeDomain::corner, 1U}, {AttributeDomain::vertex, 1U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"wrong", AttributeDomain::vertex, AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}}}));
    UvSetTable table;
    REQUIRE(table.add(UvSetDescriptor{1U, "bad", "wrong", false, false, false, 1U}));
    REQUIRE(!table.validate(attributes));

    REQUIRE(table.add(UvSetDescriptor{2U, "bad-corner", "corner-missing", false, false, false, 1U}));
    REQUIRE(!table.validate(attributes));
}

void material_bindings_are_explicit_and_gpu_packable() {
    carto::render::StandardMaterial material;
    material.base_color.asset = 0x100000001ULL;
    material.base_color.uv_set = 1U;
    material.base_color.sampler = 9U;
    material.base_color.transform.offset = {0.25, -0.5};
    material.base_color.channels.rgba[0] = carto::render::TextureChannel::green;
    material.metallic_roughness.asset = 2U;
    material.metallic_roughness.uv_set = 2U;
    material.normal.asset = 3U;
    material.normal.normal_convention = carto::render::NormalConvention::opengl_y_plus;
    material.occlusion.asset = 4U;
    material.emissive.asset = 5U;
    material.alpha_mode = carto::render::AlphaMode::masked;
    REQUIRE(material.validate());
    const auto packed = carto::render::pack_material_for_gpu(material);
    REQUIRE(packed);
    REQUIRE(packed.value().texture_asset_ids[0] == 1U);
    REQUIRE(packed.value().texture_asset_ids[1] == 1U);
    REQUIRE(packed.value().texture_asset_ids[4] == 3U);
    REQUIRE(packed.value().alpha_factors[1] == 1.0F);
    REQUIRE(packed.value().alpha_factors[2] == 1.0F);

    carto::render::MaterialAsset asset{7U, "brick", material, carto::core::Revision{3U}};
    REQUIRE(asset.validate());
    asset.id = 0U;
    REQUIRE(!asset.validate());

    material.normal.uv_set = 0U;
    REQUIRE(!material.validate());
}

void tangent_generation_is_deterministic_and_fail_closed() {
    const std::array<carto::render::TangentVertex, 3U> vertices = {{
        {{0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 0.0}},
        {{1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0}},
        {{0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0}},
    }};
    const std::array<std::uint32_t, 3U> indices = {0U, 1U, 2U};
    const auto tangents = carto::render::generate_tangents(vertices, indices);
    REQUIRE(tangents);
    REQUIRE(tangents.value().size() == 3U);
    REQUIRE(tangents.value().front().x > 0.999);
    REQUIRE(std::abs(tangents.value().front().y) < 1e-12);
    REQUIRE(std::abs(tangents.value().front().z) < 1e-12);
    REQUIRE(tangents.value().front().w == 1.0);

    const std::array<carto::render::TangentVertex, 3U> degenerate = {{
        vertices[0],
        {vertices[1].position, vertices[1].normal, {0.0, 0.0}},
        {vertices[2].position, vertices[2].normal, {0.0, 0.0}},
    }};
    REQUIRE(!carto::render::generate_tangents(degenerate, indices));
}

carto::geometry::EditableMesh make_uv_triangle() {
    carto::geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{carto::geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {carto::geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {carto::geometry::VertexId{3U}, {0.0, 1.0, 0.0}}},
        {{carto::geometry::FaceId{1U},
          {carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U},
           carto::geometry::VertexId{3U}}}}));
    return mesh;
}

carto::geometry::EditableMesh make_uv_material_quad(std::uint32_t first_region,
                                                     std::uint32_t second_region) {
    carto::geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{carto::geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {carto::geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {carto::geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {carto::geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{carto::geometry::FaceId{1U},
          {carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U},
           carto::geometry::VertexId{3U}}},
         {carto::geometry::FaceId{2U},
          {carto::geometry::VertexId{1U}, carto::geometry::VertexId{3U},
           carto::geometry::VertexId{4U}}}}));

    using carto::attributes::AttributeDomain;
    using carto::attributes::AttributeLayer;
    using carto::attributes::AttributeSet;
    using carto::attributes::AttributeType;
    using carto::attributes::UvSetDescriptor;
    using carto::attributes::UvSetTable;
    AttributeSet attributes({{AttributeDomain::corner, 6U}, {AttributeDomain::face, 2U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}, carto::core::Vec2d{1.0, 0.0},
         carto::core::Vec2d{1.0, 1.0}, carto::core::Vec2d{0.0, 0.0},
         carto::core::Vec2d{1.0, 1.0}, carto::core::Vec2d{0.0, 1.0}}}));
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"condition.material_region", AttributeDomain::face, AttributeType::uint32},
        {first_region, second_region}}));
    UvSetTable uv_sets;
    REQUIRE(uv_sets.add(UvSetDescriptor{1U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.validate());
    return mesh;
}

void editable_mesh_owns_validated_uv_payload() {
    using carto::attributes::AttributeDomain;
    using carto::attributes::AttributeLayer;
    using carto::attributes::AttributeSet;
    using carto::attributes::AttributeType;
    using carto::attributes::UvSetDescriptor;
    using carto::attributes::UvSetTable;

    auto mesh = make_uv_triangle();
    AttributeSet attributes({{AttributeDomain::corner, 3U}});
    REQUIRE(attributes.add_layer(AttributeLayer{
        {"uv0.corners", AttributeDomain::corner, AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}, carto::core::Vec2d{1.0, 0.0},
         carto::core::Vec2d{0.0, 1.0}}}));
    UvSetTable uv_sets;
    REQUIRE(uv_sets.add(UvSetDescriptor{7U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(mesh.has_attribute_payload());
    REQUIRE(mesh.validate());
    REQUIRE(mesh.uv_sets().active_for_render()->id == 7U);
    REQUIRE(mesh.attributes().find("uv0.corners") != nullptr);

    REQUIRE(mesh.set_vertex_position(carto::geometry::VertexId{1U}, {0.0, 0.0, 0.1}));
    REQUIRE(!mesh.add_vertex({0.0, 0.0, 1.0}));
    REQUIRE(!mesh.inset_face(carto::geometry::FaceId{1U}, 0.1));
    REQUIRE(mesh.clear_attribute_payload());
    REQUIRE(!mesh.has_attribute_payload());
    REQUIRE(mesh.add_vertex({0.0, 0.0, 1.0}));
}

void invalid_uv_cardinality_is_rejected_before_attachment() {
    auto mesh = make_uv_triangle();
    carto::attributes::AttributeSet attributes({{
        carto::attributes::AttributeDomain::corner, 2U}});
    REQUIRE(attributes.add_layer({
        {"uv0.corners", carto::attributes::AttributeDomain::corner,
         carto::attributes::AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}, carto::core::Vec2d{1.0, 0.0}}}));
    carto::attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(!mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    REQUIRE(!mesh.has_attribute_payload());
}

void project_v6_round_trips_uv_payload_and_reads_v5_without_one() {
    auto document = carto::project::ProjectDocument::create("UV persistence");
    REQUIRE(document);
    auto mesh = make_uv_triangle();
    carto::attributes::AttributeSet attributes({{
        carto::attributes::AttributeDomain::corner, 3U}});
    REQUIRE(attributes.add_layer({
        {"uv0.corners", carto::attributes::AttributeDomain::corner,
         carto::attributes::AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}, carto::core::Vec2d{1.0, 0.0},
         carto::core::Vec2d{0.0, 1.0}}}));
    carto::attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({9U, "Render UV", "uv0.corners", false, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    const auto mesh_id = carto::project::testing::access(document.value()).add_mesh(std::move(mesh));
    REQUIRE(mesh_id);

    const auto serialized = document.value().serialize();
    REQUIRE(serialized.find("CARTOGRAPHER_PROJECT 6") == 0U);
    REQUIRE(serialized.find("ATTRIBUTES\n") != std::string::npos);
    REQUIRE(serialized.find("UV_SET 9") != std::string::npos);
    const auto reopened = carto::project::ProjectDocument::deserialize(serialized);
    REQUIRE(reopened);
    const auto& restored = reopened.value().meshes().at(mesh_id.value());
    REQUIRE(restored.has_attribute_payload());
    REQUIRE(restored.uv_sets().active_for_render()->id == 9U);
    REQUIRE(restored.attributes().find("uv0.corners") != nullptr);
    REQUIRE(reopened.value().serialize() == serialized);

    std::string legacy = serialized;
    const auto header = legacy.find("CARTOGRAPHER_PROJECT 6");
    REQUIRE(header == 0U);
    legacy.replace(header, std::string("CARTOGRAPHER_PROJECT 6").size(),
                   "CARTOGRAPHER_PROJECT 5");
    const auto attributes_start = legacy.find("ATTRIBUTES\n");
    const auto topology_start = legacy.find("TOPOLOGY_RECEIPTS ", attributes_start);
    REQUIRE(attributes_start != std::string::npos && topology_start != std::string::npos);
    legacy.erase(attributes_start, topology_start - attributes_start);
    const auto legacy_reopened = carto::project::ProjectDocument::deserialize(legacy);
    REQUIRE(legacy_reopened);
    REQUIRE(!legacy_reopened.value().meshes().at(mesh_id.value()).has_attribute_payload());

    std::string wrong_cardinality = serialized;
    const auto domain_record = wrong_cardinality.find("DOMAIN_COUNT 4 3\n");
    REQUIRE(domain_record != std::string::npos);
    wrong_cardinality.replace(domain_record, std::string("DOMAIN_COUNT 4 3\n").size(),
                              "DOMAIN_COUNT 4 2\n");
    REQUIRE(!carto::project::ProjectDocument::deserialize(wrong_cardinality));

    std::string malformed_value = serialized;
    const auto value_record = malformed_value.find("ATTRIBUTE_VALUE 0 0\n");
    REQUIRE(value_record != std::string::npos);
    malformed_value.replace(value_record, std::string("ATTRIBUTE_VALUE 0 0\n").size(),
                            "ATTRIBUTE_VALUE nope 0\n");
    REQUIRE(!carto::project::ProjectDocument::deserialize(malformed_value));
}

void gltf_export_emits_active_corner_uvs_and_splits_seams() {
    auto document = carto::project::ProjectDocument::create("UV glTF export");
    REQUIRE(document);
    carto::geometry::EditableMesh mesh;
    REQUIRE(mesh.insert_bulk(
        {{carto::geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {carto::geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {carto::geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {carto::geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{carto::geometry::FaceId{1U},
          {carto::geometry::VertexId{1U}, carto::geometry::VertexId{2U},
           carto::geometry::VertexId{3U}}},
         {carto::geometry::FaceId{2U},
          {carto::geometry::VertexId{1U}, carto::geometry::VertexId{3U},
           carto::geometry::VertexId{4U}}}}));

    carto::attributes::AttributeSet attributes({{
        carto::attributes::AttributeDomain::corner, 6U}});
    REQUIRE(attributes.add_layer({
        {"uv0.corners", carto::attributes::AttributeDomain::corner,
         carto::attributes::AttributeType::vec2},
        {carto::core::Vec2d{0.0, 0.0}, carto::core::Vec2d{1.0, 0.0},
         carto::core::Vec2d{1.0, 1.0}, carto::core::Vec2d{0.25, 0.25},
         carto::core::Vec2d{0.75, 0.75}, carto::core::Vec2d{0.0, 1.0}}}));
    carto::attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "UV0", "uv0.corners", true, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));
    const auto mesh_id = carto::project::testing::access(document.value()).add_mesh(std::move(mesh));
    REQUIRE(mesh_id);
    const auto object = carto::project::testing::access(document.value()).create_object("UV box");
    REQUIRE(object);
    REQUIRE(carto::project::testing::access(document.value()).attach_mesh(
        object.value(), mesh_id.value()));

    const auto path = std::filesystem::temp_directory_path() /
        "cartographer_uv_material_contract.gltf";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto exported = carto::io::export_gltf(document.value(), path);
    REQUIRE(exported);
    REQUIRE(exported.value().vertices == 6U);
    REQUIRE(exported.value().triangles == 2U);
    REQUIRE(exported.value().warnings.size() == 2U);

    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    const std::string text(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(text.find("TEXCOORD_0") != std::string::npos);
    REQUIRE(text.find("TANGENT") != std::string::npos);
    REQUIRE(text.find("no UV") == std::string::npos);
    std::filesystem::remove(path, ignored);
}

void gltf_export_preserves_condition_material_regions_in_extras() {
    auto document = carto::project::ProjectDocument::create("Material region export");
    REQUIRE(document);
    auto mesh = make_uv_material_quad(1001U, 1002U);
    const auto mesh_id = carto::project::testing::access(document.value()).add_mesh(std::move(mesh));
    REQUIRE(mesh_id);
    const auto object = carto::project::testing::access(document.value()).create_object("Region quad");
    REQUIRE(object);
    REQUIRE(carto::project::testing::access(document.value()).attach_mesh(
        object.value(), mesh_id.value()));

    const auto path = std::filesystem::temp_directory_path() /
        "cartographer_condition_material_region.gltf";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto exported = carto::io::export_gltf(document.value(), path);
    REQUIRE(exported);
    REQUIRE(exported.value().warnings.size() == 2U);
    REQUIRE(exported.value().warnings.front().find("condition.material_region") !=
            std::string::npos);

    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    const std::string text(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    REQUIRE(text.find("condition_material_region") != std::string::npos);
    REQUIRE(text.find("\"id\":1,\"region\":1001") != std::string::npos);
    REQUIRE(text.find("\"id\":2,\"region\":1002") != std::string::npos);
    REQUIRE(text.find("\"materials\"") == std::string::npos);
    REQUIRE(text.find("TEXCOORD_0") != std::string::npos);
    REQUIRE(text.find("TANGENT") != std::string::npos);
    std::filesystem::remove(path, ignored);
}

void gltf_export_rejects_invalid_condition_material_regions() {
    auto document = carto::project::ProjectDocument::create("Invalid material region export");
    REQUIRE(document);
    auto mesh = make_uv_material_quad(1001U, 0U);
    const auto mesh_id = carto::project::testing::access(document.value()).add_mesh(std::move(mesh));
    REQUIRE(mesh_id);
    const auto object = carto::project::testing::access(document.value()).create_object("Invalid region quad");
    REQUIRE(object);
    REQUIRE(carto::project::testing::access(document.value()).attach_mesh(
        object.value(), mesh_id.value()));

    const auto path = std::filesystem::temp_directory_path() /
        "cartographer_invalid_condition_material_region.gltf";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto exported = carto::io::export_gltf(document.value(), path);
    REQUIRE(!exported);
    REQUIRE(exported.error().message.find("non-zero") != std::string::npos);
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    try {
        uv_identity_reuses_corner_attribute_storage();
        uv_identity_rejects_ambiguous_or_wrong_layers();
        material_bindings_are_explicit_and_gpu_packable();
        tangent_generation_is_deterministic_and_fail_closed();
        editable_mesh_owns_validated_uv_payload();
        invalid_uv_cardinality_is_rejected_before_attachment();
        project_v6_round_trips_uv_payload_and_reads_v5_without_one();
        gltf_export_emits_active_corner_uvs_and_splits_seams();
        gltf_export_preserves_condition_material_regions_in_extras();
        gltf_export_rejects_invalid_condition_material_regions();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
