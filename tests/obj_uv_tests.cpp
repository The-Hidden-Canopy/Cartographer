#include <carto/io/obj.hpp>

#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>

namespace {

using namespace carto;
using CornerKey = std::pair<geometry::FaceId, geometry::VertexId>;

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
            ("cartographer-obj-uv-" + std::to_string(stamp));
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

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "temporary OBJ fixture opens");
    output << text;
    output.close();
    require(static_cast<bool>(output), "temporary OBJ fixture writes");
}

std::map<CornerKey, core::Vec2d> corner_uvs(const geometry::EditableMesh& mesh) {
    std::map<CornerKey, core::Vec2d> result;
    const auto topology = mesh.topology();
    const auto* layer = mesh.attributes().find("cartographer.uv0");
    require(topology && layer != nullptr &&
        layer->descriptor.domain == attributes::AttributeDomain::corner &&
        layer->descriptor.type == attributes::AttributeType::vec2 &&
        layer->values.size() == topology.value().corners.size(),
        "mesh contains a cardinality-correct corner UV layer");
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* uv = std::get_if<core::Vec2d>(&layer->values[index]);
        require(uv != nullptr, "corner UV uses vec2 values");
        result.emplace(CornerKey{topology.value().corners[index].face,
            topology.value().corners[index].vertex}, *uv);
    }
    return result;
}

std::map<CornerKey, core::Vec3d> corner_normals(const geometry::EditableMesh& mesh) {
    std::map<CornerKey, core::Vec3d> result;
    const auto topology = mesh.topology();
    const auto* layer = mesh.attributes().find("cartographer.normal0");
    require(topology && layer != nullptr &&
        layer->descriptor.domain == attributes::AttributeDomain::corner &&
        layer->descriptor.type == attributes::AttributeType::vec3 &&
        layer->values.size() == topology.value().corners.size(),
        "mesh contains a cardinality-correct corner normal layer");
    for (std::size_t index = 0U; index < topology.value().corners.size(); ++index) {
        const auto* normal = std::get_if<core::Vec3d>(&layer->values[index]);
        require(normal != nullptr, "corner normal uses vec3 values");
        result.emplace(CornerKey{topology.value().corners[index].face,
            topology.value().corners[index].vertex}, *normal);
    }
    return result;
}

geometry::EditableMesh quad_with_uvs() {
    geometry::EditableMesh mesh;
    require(static_cast<bool>(mesh.insert_bulk(
        {{geometry::VertexId{1U}, {0.0, 0.0, 0.0}},
         {geometry::VertexId{2U}, {1.0, 0.0, 0.0}},
         {geometry::VertexId{3U}, {1.0, 1.0, 0.0}},
         {geometry::VertexId{4U}, {0.0, 1.0, 0.0}}},
        {{geometry::FaceId{1U}, {geometry::VertexId{1U}, geometry::VertexId{2U},
            geometry::VertexId{3U}, geometry::VertexId{4U}}}})),
        "UV export fixture topology inserts");
    const auto topology = mesh.topology();
    require(static_cast<bool>(topology), "UV export fixture topology resolves");
    const std::map<geometry::VertexId, core::Vec2d> by_vertex{
        {geometry::VertexId{1U}, {0.125, 0.25}},
        {geometry::VertexId{2U}, {0.875, 0.25}},
        {geometry::VertexId{3U}, {0.875, 0.75}},
        {geometry::VertexId{4U}, {0.125, 0.75}},
    };
    std::vector<attributes::AttributeValue> values;
    for (const auto& corner : topology.value().corners) {
        values.emplace_back(by_vertex.at(corner.vertex));
    }
    attributes::AttributeSet payload({
        {attributes::AttributeDomain::corner, values.size()}});
    require(static_cast<bool>(payload.add_layer(attributes::AttributeLayer{
        {"cartographer.uv0", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec2}, std::move(values)})),
        "UV export fixture layer admits");
    attributes::UvSetTable sets;
    require(static_cast<bool>(sets.add(attributes::UvSetDescriptor{
        1U, "Fixture UV", "cartographer.uv0", true, true, false, 1U})),
        "UV export fixture descriptor admits");
    require(static_cast<bool>(mesh.set_attribute_payload(std::move(payload), std::move(sets))),
        "UV export fixture payload attaches");
    return mesh;
}

void import_preserves_negative_indices_and_corner_seams() {
    TempDirectory temp;
    const auto path = temp.path() / "seams.obj";
    write_text(path,
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0.5 0.5\nvt 0.75 1\nvt 0 1\n"
        "f -4/-6 -3/-5 -2/-4\n"
        "f 1/4 3/5 4/6\n");
    const auto imported = io::import_obj(path);
    require(imported && imported.value().mesh.vertex_count() == 4U &&
        imported.value().mesh.face_count() == 2U,
        "OBJ UVs import with relative vertex and texture indices");
    const auto values = corner_uvs(imported.value().mesh);
    const auto first_seam = values.find({geometry::FaceId{1U}, geometry::VertexId{1U}});
    const auto second_seam = values.find({geometry::FaceId{2U}, geometry::VertexId{1U}});
    require(first_seam != values.end() && second_seam != values.end() &&
        first_seam->second.x == 0.0 && first_seam->second.y == 0.0 &&
        second_seam->second.x == 0.5 && second_seam->second.y == 0.5,
        "same shared vertex retains distinct face-corner UV seam values");
    const auto* editing = imported.value().mesh.uv_sets().active_for_editing();
    const auto* rendering = imported.value().mesh.uv_sets().active_for_render();
    require(editing != nullptr && rendering != nullptr &&
        editing->layer_id == "cartographer.uv0" && rendering->layer_id == editing->layer_id,
        "imported UV set is explicitly active for editing and rendering");
}

void import_export_preserves_custom_normals_across_shared_vertex_seams() {
    TempDirectory temp;
    const auto source_path = temp.path() / "normal-seams.obj";
    write_text(source_path,
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vn 0 0 1\nvn 0 1 1\n"
        "f 1//1 2//1 3//1\n"
        "f -4//-1 -2//-1 -1//-1\n");
    const auto imported = io::import_obj(source_path);
    require(imported && imported.value().mesh.face_count() == 2U,
        "OBJ custom normals import with positive and relative indices");
    const auto source_normals = corner_normals(imported.value().mesh);
    require(source_normals.size() == 6U &&
        source_normals.at({geometry::FaceId{1U}, geometry::VertexId{1U}}).z == 1.0 &&
        source_normals.at({geometry::FaceId{2U}, geometry::VertexId{1U}}).y == 1.0,
        "shared vertex retains different authored normals on its two face corners");

    const auto exported_path = temp.path() / "normal-seams-roundtrip.obj";
    const auto exported = io::export_obj(imported.value().mesh, exported_path);
    require(exported && exported.value().triangles == 2U &&
        std::any_of(exported.value().warnings.begin(), exported.value().warnings.end(),
            [](const std::string& warning) {
                return warning.find("custom corner normals") != std::string::npos;
            }),
        "OBJ export reports that custom corner normals were preserved");
    const auto reopened = io::import_obj(exported_path);
    require(reopened && reopened.value().mesh.face_count() == 2U,
        "custom-normal OBJ export reimports");
    const auto reopened_normals = corner_normals(reopened.value().mesh);
    require(reopened_normals.size() == source_normals.size(),
        "round trip preserves every face-corner normal");
    for (const auto& [corner, normal] : source_normals) {
        const auto found = reopened_normals.find(corner);
        require(found != reopened_normals.end() &&
            found->second.x == normal.x && found->second.y == normal.y &&
            found->second.z == normal.z,
            "round trip preserves exact corner-normal values and seam assignment");
    }
}

void import_export_preserves_uvs_and_normals_in_combined_face_references() {
    TempDirectory temp;
    const auto source_path = temp.path() / "combined-seams.obj";
    write_text(source_path,
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0.5 0.5\nvt 0.75 1\nvt 0 1\n"
        "vn 0 0 1\nvn 0 1 1\n"
        "f 1/1/1 2/2/1 3/3/1\n"
        "f -4/-3/-1 -2/-2/-1 -1/-1/-1\n");
    const auto imported = io::import_obj(source_path);
    require(imported && imported.value().mesh.face_count() == 2U,
        "combined OBJ position/UV/normal references import");
    const auto source_uvs = corner_uvs(imported.value().mesh);
    const auto source_normals = corner_normals(imported.value().mesh);
    require(source_uvs.at({geometry::FaceId{1U}, geometry::VertexId{1U}}).x == 0.0 &&
        source_uvs.at({geometry::FaceId{2U}, geometry::VertexId{1U}}).x == 0.5 &&
        source_normals.at({geometry::FaceId{1U}, geometry::VertexId{1U}}).z == 1.0 &&
        source_normals.at({geometry::FaceId{2U}, geometry::VertexId{1U}}).y == 1.0,
        "UV and custom-normal seams independently retain shared-vertex corner values");

    const auto exported_path = temp.path() / "combined-seams-roundtrip.obj";
    require(static_cast<bool>(io::export_obj(imported.value().mesh, exported_path)),
        "combined UV and custom-normal payload exports");
    const auto reopened = io::import_obj(exported_path);
    require(reopened && reopened.value().mesh.face_count() == 2U,
        "combined UV and custom-normal export reimports");
    const auto reopened_uvs = corner_uvs(reopened.value().mesh);
    const auto reopened_normals = corner_normals(reopened.value().mesh);
    require(reopened_uvs.size() == source_uvs.size() &&
        reopened_normals.size() == source_normals.size(),
        "combined round trip preserves every corner payload");
    for (const auto& [corner, uv] : source_uvs) {
        const auto found = reopened_uvs.find(corner);
        require(found != reopened_uvs.end() && found->second.x == uv.x &&
            found->second.y == uv.y,
            "combined round trip preserves exact UV corner values");
    }
    for (const auto& [corner, normal] : source_normals) {
        const auto found = reopened_normals.find(corner);
        require(found != reopened_normals.end() && found->second.x == normal.x &&
            found->second.y == normal.y && found->second.z == normal.z,
            "combined round trip preserves exact normal corner values");
    }
}

void import_rejects_partial_or_invalid_custom_normals() {
    TempDirectory temp;
    const auto check_rejected = [&temp](const char* name, const std::string& source) {
        const auto path = temp.path() / name;
        write_text(path, source);
        require(!io::import_obj(path), "partial or invalid custom normals are refused");
    };
    check_rejected("partial-normal-corners.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2 3//1\n");
    check_rejected("partial-normal-faces.obj",
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvn 0 0 1\n"
        "f 1//1 2//1 3//1\nf 1 3 4\n");
    check_rejected("out-of-range-normal.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//2 2//1 3//1\n");
    check_rejected("zero-normal.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 0\nf 1//1 2//1 3//1\n");
}

void import_rejects_partial_or_malformed_uvs_atomically() {
    TempDirectory temp;
    const auto check_rejected = [&temp](const char* name, const std::string& source) {
        const auto path = temp.path() / name;
        write_text(path, source);
        require(!io::import_obj(path), "partial or malformed UV data is refused");
    };
    check_rejected("mixed-corners.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n"
        "f 1/1 2 3/3\n");
    check_rejected("mixed-faces.obj",
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "f 1/1 2/2 3/3\nf 1 3 4\n");
    check_rejected("bad-uv-index.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/1 2/2 3/1\n");
    check_rejected("nonfinite-uv.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt nan 0\nvt 1 0\nvt 0 1\n"
        "f 1/1 2/2 3/3\n");
    check_rejected("extra-slash.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\n"
        "f 1/1/1/1 2/2 3/3\n");
}

void topology_repair_keeps_face_corner_uv_association() {
    TempDirectory temp;
    const auto path = temp.path() / "repaired-seams.obj";
    write_text(path,
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "vt 0.25 0.25\nvt 0.75 0.25\nvt 0.25 0.75\n"
        "f 1/1 2/2 3/3\nf 1/4 2/5 3/6\n");
    const auto imported = io::import_obj(path);
    require(imported && imported.value().mesh.vertex_count() == 6U &&
        imported.value().report.warnings.size() == 1U,
        "directed-edge topology repair remains active for UV meshes");
    const auto values = corner_uvs(imported.value().mesh);
    const auto repaired_first = values.find({geometry::FaceId{2U}, geometry::VertexId{4U}});
    const auto repaired_second = values.find({geometry::FaceId{2U}, geometry::VertexId{5U}});
    require(repaired_first != values.end() && repaired_second != values.end() &&
        repaired_first->second.x == 0.25 && repaired_first->second.y == 0.25 &&
        repaired_second->second.x == 0.75 && repaired_second->second.y == 0.25,
        "UVs remain bound to local corners after repaired vertex identities change");
}

void export_and_reimport_preserves_uvs_through_polygon_triangulation() {
    TempDirectory temp;
    const auto source = quad_with_uvs();
    const auto path = temp.path() / "uv-quad.obj";
    const auto exported = io::export_obj(source, path);
    require(exported && exported.value().triangles == 2U &&
        !exported.value().warnings.empty(),
        "OBJ exporter emits triangulated geometry and reports remaining feature loss");
    const auto imported = io::import_obj(path);
    require(imported && imported.value().mesh.face_count() == 2U,
        "triangulated OBJ can be reimported");
    const auto source_uvs = corner_uvs(source);
    const auto imported_uvs = corner_uvs(imported.value().mesh);
    const std::map<geometry::VertexId, core::Vec2d> expected_by_vertex{
        {geometry::VertexId{1U}, {0.125, 0.25}},
        {geometry::VertexId{2U}, {0.875, 0.25}},
        {geometry::VertexId{3U}, {0.875, 0.75}},
        {geometry::VertexId{4U}, {0.125, 0.75}},
    };
    require(source_uvs.size() == 4U && imported_uvs.size() == 6U,
        "export preserves each polygon corner while triangulating the face");
    for (const auto& [corner, uv] : imported_uvs) {
        static_cast<void>(corner);
        const auto expected = expected_by_vertex.find(corner.second);
        require(expected != expected_by_vertex.end() &&
            expected->second.x == uv.x && expected->second.y == uv.y,
            "every triangulated output corner retains its source UV value");
    }
}

void export_and_import_preserves_face_material_regions() {
    TempDirectory temp;
    auto source = quad_with_uvs();
    auto payload = source.attributes();
    require(static_cast<bool>(payload.set_domain_count(
        attributes::AttributeDomain::face, 1U)),
        "material fixture declares face-domain cardinality");
    require(static_cast<bool>(payload.add_layer(attributes::AttributeLayer{
        {"condition.material_region", attributes::AttributeDomain::face,
            attributes::AttributeType::uint32},
        {attributes::AttributeValue{std::uint32_t{2U}}}})),
        "material fixture adds a foliage region ID");
    require(static_cast<bool>(source.set_attribute_payload(
        std::move(payload), source.uv_sets())),
        "material fixture attaches UV and region payload together");

    const auto path = temp.path() / "material-quad.obj";
    const auto exported = io::export_obj(source, path);
    require(exported && exported.value().triangles == 2U &&
        std::any_of(exported.value().warnings.begin(), exported.value().warnings.end(),
            [](const std::string& warning) {
                return warning.find("region IDs") != std::string::npos;
            }),
        "OBJ export preserves face-region IDs while warning that material assets are omitted");
    const auto imported = io::import_obj(path);
    require(static_cast<bool>(imported) && imported.value().mesh.face_count() == 2U,
        "triangulated material-region OBJ reimports");
    const auto* regions = imported.value().mesh.attributes().find(
        "condition.material_region");
    require(regions != nullptr &&
        regions->descriptor.domain == attributes::AttributeDomain::face &&
        regions->descriptor.type == attributes::AttributeType::uint32 &&
        regions->values.size() == 2U,
        "OBJ reimport restores a face-domain material-region layer");
    for (const auto& value : regions->values) {
        const auto* id = std::get_if<std::uint32_t>(&value);
        require(id != nullptr && *id == 2U,
            "every triangulated face retains its source foliage material region");
    }
    const auto* uvs = imported.value().mesh.attributes().find("cartographer.uv0");
    require(uvs != nullptr && uvs->values.size() == 6U,
        "material-region and UV domains coexist through OBJ transfer");
}

void import_rejects_incomplete_material_region_tables() {
    TempDirectory temp;
    const auto path = temp.path() / "incomplete-regions.obj";
    write_text(path,
        "# CARTOGRAPHER_FACE_MATERIAL_REGIONS 1 condition.material_region\n"
        "# CARTOGRAPHER_FACE_MATERIAL 1 1\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3\nf 1 3 4\n");
    const auto imported = io::import_obj(path);
    require(!imported,
        "partial face material-region metadata is rejected instead of appearing live");
}

void export_rejects_conflicting_material_region_aliases_without_replacing_file() {
    TempDirectory temp;
    auto source = quad_with_uvs();
    auto payload = source.attributes();
    require(static_cast<bool>(payload.set_domain_count(
        attributes::AttributeDomain::face, 1U)),
        "conflicting material fixture declares face-domain cardinality");
    require(static_cast<bool>(payload.add_layer(attributes::AttributeLayer{
        {"condition.material_region", attributes::AttributeDomain::face,
            attributes::AttributeType::uint32},
        {attributes::AttributeValue{std::uint32_t{2U}}}})),
        "conflicting material fixture adds the condition region ID");
    require(static_cast<bool>(payload.add_layer(attributes::AttributeLayer{
        {"cartographer.material_slot", attributes::AttributeDomain::face,
            attributes::AttributeType::uint32},
        {attributes::AttributeValue{std::uint32_t{3U}}}})),
        "conflicting material fixture adds the generic slot ID");
    require(static_cast<bool>(source.set_attribute_payload(
        std::move(payload), source.uv_sets())),
        "conflicting material fixture attaches both aliases");

    const auto path = temp.path() / "existing.obj";
    write_text(path, "keep existing file\n");
    const auto exported = io::export_obj(source, path);
    require(!exported,
        "export refuses different values for the two material-region aliases");
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    require(contents == "keep existing file\n",
        "rejected conflicting aliases leave the existing export untouched");
}

} // namespace

int main() {
    import_preserves_negative_indices_and_corner_seams();
    import_export_preserves_custom_normals_across_shared_vertex_seams();
    import_export_preserves_uvs_and_normals_in_combined_face_references();
    import_rejects_partial_or_invalid_custom_normals();
    import_rejects_partial_or_malformed_uvs_atomically();
    topology_repair_keeps_face_corner_uv_association();
    export_and_reimport_preserves_uvs_through_polygon_triangulation();
    export_and_import_preserves_face_material_regions();
    import_rejects_incomplete_material_region_tables();
    export_rejects_conflicting_material_region_aliases_without_replacing_file();
    std::cout << "Cartographer OBJ UV tests passed\n";
    return EXIT_SUCCESS;
}
