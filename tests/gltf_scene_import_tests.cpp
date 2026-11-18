#include "application_access.hpp"
#include "gltf_test_payload.hpp"
#include "scene_import_test_support.hpp"
#include <carto/io/generated_mesh.hpp>
#include <carto/core/json.hpp>
#include <carto/io/gltf_scene_import.hpp>
#include <carto/production/authoring_viewport.hpp>
#include <sstream>
namespace {
std::string b64(std::span<const std::uint8_t> data) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const auto n = (std::uint32_t(data[i]) << 16) |
                       (i + 1 < data.size() ? std::uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < data.size() ? data[i + 2] : 0);
        result += alphabet[n >> 18];
        result += alphabet[(n >> 12) & 63];
        result += i + 1 < data.size() ? alphabet[(n >> 6) & 63] : '=';
        result += i + 2 < data.size() ? alphabet[n & 63] : '=';
    }
    return result;
}
void append(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::vector<std::uint8_t> fixture(bool glb = false, std::array<float, 3> normal = {0.f, 0.f, 1.f}) {
    // Independently authored synthetic glTF, not produced by Cartographer's
    // exporter: interleaved float vertices, uint8 indices, two mesh instances.
    std::vector<std::uint8_t> binary;
    for (const auto p : std::array<core::Vec2d, 3>{{{0, 0}, {1, 0}, {0, 1}}})
        for (float x : {float(p.x), float(p.y), 0.f, normal[0], normal[1], normal[2], 0.f, 1.f, 0.f, -1.f, float(p.x),
                        float(p.y)})
            append(binary, std::bit_cast<std::uint32_t>(x));
    binary.insert(binary.end(), {0, 1, 2});
    const auto length = binary.size();
    std::ostringstream j;
    j << R"({"asset":{"version":"2.0","generator":"Independent synthetic fixture"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"assembly","translation":[0,0,0],"children":[1,2]},{"name":"first","mesh":0},{"name":"mirrored","mesh":0,"matrix":[-1,0,0,0,0,2,0,0,0,0,1,0,2,0,0,1]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3},"indices":4,"material":0}]}],"materials":[{"name":"synthetic emission","doubleSided":true,"emissiveFactor":[1,0.2,0.1],"pbrMetallicRoughness":{"baseColorFactor":[0.2,0.3,0.4,1],"metallicFactor":0.3,"roughnessFactor":0.6}}],"buffers":[{"byteLength":)"
      << length;
    if (!glb)
        j << ",\"uri\":\"data:application/octet-stream;base64," << b64(binary) << '"';
    j << R"(}],"bufferViews":[{"buffer":0,"byteLength":144,"byteStride":48},{"buffer":0,"byteOffset":144,"byteLength":3}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":0,"byteOffset":24,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":0,"byteOffset":40,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":1,"componentType":5121,"count":3,"type":"SCALAR"}]})";
    auto json = j.str();
    if (!glb)
        return {json.begin(), json.end()};
    while (json.size() % 4)
        json += ' ';
    while (binary.size() % 4)
        binary.push_back(0);
    std::vector<std::uint8_t> bytes;
    append(bytes, 0x46546c67);
    append(bytes, 2);
    append(bytes, static_cast<std::uint32_t>(28 + json.size() + binary.size()));
    append(bytes, static_cast<std::uint32_t>(json.size()));
    append(bytes, 0x4e4f534a);
    bytes.insert(bytes.end(), json.begin(), json.end());
    append(bytes, static_cast<std::uint32_t>(binary.size()));
    append(bytes, 0x004e4942);
    bytes.insert(bytes.end(), binary.begin(), binary.end());
    return bytes;
}
std::vector<std::uint8_t> replace(std::vector<std::uint8_t> bytes, std::string_view old,
                                  std::string_view text) {
    std::string s(bytes.begin(), bytes.end());
    const auto pos = s.find(old);
    check(pos != std::string::npos, "mutation target exists");
    s.replace(pos, old.size(), text);
    return {s.begin(), s.end()};
}
std::vector<std::uint8_t> color_fixture(
    const std::array<std::array<float, 4>, 3>& colors, unsigned components = 4,
    bool normalized_bytes = false) {
    std::vector<std::uint8_t> bytes;
    for (const auto& p : std::array<std::array<float, 3>, 3>{{{0,0,0},{1,0,0},{0,1,0}}})
        for (float v : p) append(bytes, std::bit_cast<std::uint32_t>(v));
    for (const auto& c : colors)
        for (unsigned i = 0; i < components; ++i)
            if (normalized_bytes) bytes.push_back(static_cast<std::uint8_t>(c[i]));
            else append(bytes, std::bit_cast<std::uint32_t>(c[i]));
    const auto color_bytes = bytes.size() - 36;
    std::ostringstream json;
    json << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"COLOR_0":1},"material":0}]}],"materials":[{"doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"metallicFactor":0,"roughnessFactor":0.5}}],"buffers":[{"byteLength":)"
         << bytes.size() << R"(,"uri":"data:application/octet-stream;base64,)" << b64(bytes)
         << R"("}],"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":)"
         << color_bytes << R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":)"
         << (normalized_bytes ? 5121 : 5126) << R"(,"normalized":)"
         << (normalized_bytes ? "true" : "false") << R"(,"count":3,"type":"VEC)" << components << R"("}]})";
    const auto text = json.str();
    return {text.begin(), text.end()};
}
void check_corner_colors(const geometry::EditableMesh& mesh,
                         const std::array<std::array<float, 4>, 3>& expected) {
    const auto* layer = mesh.attributes().find("gltf.color0");
    check(layer && layer->descriptor.domain == attributes::AttributeDomain::corner &&
          layer->descriptor.type == attributes::AttributeType::color4 && layer->values.size() == 3,
          "COLOR_0 creates exactly three typed corner colors");
    for (std::size_t i = 0; i < 3; ++i) {
        const auto actual = std::get<core::Vec4d>(layer->values[i]);
        check(actual.x == expected[i][0] && actual.y == expected[i][1] &&
              actual.z == expected[i][2] && actual.w == expected[i][3],
              "corner RGBA components retain their exact promoted float values");
    }
}
project::ProjectDocument document(application::ApplicationSession &s) {
    return take(s.authoring_viewport_source(s.snapshot().project_revision));
}
void send(application::ApplicationSession &s, const application::ApplicationAction &action) {
    take(application::testing::dispatch(s, action));
}
void reject(const std::vector<std::uint8_t> &bytes, const char *reason) {
    check(!io::import_gltf_scene(bytes), reason);
}
} // namespace
int main(int argc, char **argv) {
    try {
        // Deterministic grayscale PNG fixtures generated with Python stdlib zlib:
        // filter-zero rows, 8-bit grayscale zero pixels, no color metadata.
        // Full 4096-square decode is 64 MiB RGBA; aggregate admission stays 256 MiB.
        {
            const auto read_fixture = [](const char* name) {
                const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
                std::ifstream stream(path, std::ios::binary);
                check(bool(stream), "4K PNG fixture is readable");
                return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream), {});
            };
            const auto texture_source = [](const std::vector<std::uint8_t>& png, unsigned count) {
                const auto image = std::string("{\"uri\":\"data:image/png;base64,") + b64(png) + "\"}";
                std::string json = "{\"asset\":{\"version\":\"2.0\"},\"images\":[";
                for (unsigned i = 0; i < count; ++i) { if (i) json += ','; json += image; }
                json += "]}";
                return std::vector<std::uint8_t>(json.begin(), json.end());
            };
            const auto png = read_fixture("gltf-texture-4096.png");
            const auto imported = take(io::import_gltf_materials(texture_source(png, 1), false));
            check(imported.catalog.textures().begin()->second.width == 4096 &&
                  imported.catalog.textures().begin()->second.height == 4096 &&
                  imported.encoded_images.at(0) == png,
                  "full 4K glTF image decodes and retains exact source bytes");
            const auto oversized = io::import_gltf_materials(
                texture_source(read_fixture("gltf-texture-4097x1.png"), 1), false);
            check(!oversized, "4097-pixel dimension remains refused");
            const auto aggregate = io::import_gltf_materials(texture_source(png, 5), false);
            check(!aggregate && aggregate.error().message.find("256 MiB") != std::string::npos,
                  "fifth full 4K texture refuses at unchanged decoded aggregate bound");
        }
        // Authored float32 components survive JSON glTF and GLB import exactly,
        // including signed zero. Slightly off-unit accepted input is normalized;
        // the existing admission bound still rejects genuinely nonunit input.
        for (bool binary : {false, true}) {
            const std::array<float, 3> source{0.6f, -0.0f, 0.8f};
            const core::Vec3d expected{source[0], source[1], source[2]};
            check(std::abs(expected.length_squared() - 1) > 0 &&
                  std::abs(expected.length_squared() - 1) <= 4e-7,
                  "preservation fixture differs from exact unit in float32 rounding range");
            const auto parsed = take(io::import_gltf_scene(fixture(binary, source)));
            const auto* normals = parsed.meshes[0].attributes().find("cartographer.normal0");
            check(normals && normals->values.size() == 3, "authored corner normals present");
            for (const auto& value : normals->values) {
                const auto actual = std::get<core::Vec3d>(value);
                check(std::bit_cast<std::uint64_t>(actual.x) == std::bit_cast<std::uint64_t>(expected.x) &&
                      std::bit_cast<std::uint64_t>(actual.y) == std::bit_cast<std::uint64_t>(expected.y) &&
                      std::bit_cast<std::uint64_t>(actual.z) == std::bit_cast<std::uint64_t>(expected.z),
                      "float32 authored normals retain exact promoted component bits");
            }
            const std::array<float, 3> off_unit{0.60001f, 0.f, 0.8f};
            const core::Vec3d before{off_unit[0], off_unit[1], off_unit[2]};
            check(std::abs(before.length_squared() - 1) > 4e-7 &&
                  std::abs(before.length_squared() - 1) < 1e-4,
                  "normalization fixture lies between preservation and rejection bounds");
            const auto normalized = take(io::import_gltf_scene(fixture(binary, off_unit)));
            const auto after = std::get<core::Vec3d>(
                normalized.meshes[0].attributes().find("cartographer.normal0")->values[0]);
            check(std::abs(after.length_squared() - 1) < 1e-12 && after.x != before.x,
                  "off-unit admitted normals are renormalized");
            const auto rejected = io::import_gltf_scene(fixture(binary, {0.61f, 0.f, 0.8f}));
            check(!rejected && rejected.error().message.find("authored normal is not unit length") != std::string::npos,
                  "nonunit authored normals fail at unchanged admission bound");
        }
        // The large exception is bounded, numeric-only and opt-in. Ordinary
        // arrays and all default parser users retain the original limit.
        const auto make_array = [](std::size_t count) {
            std::string a = "[";
            for (std::size_t i = 0; i < count; ++i) { if (i) a += ','; a += '1'; }
            return a + "]";
        };
        const auto large = make_array(107390);
        const auto metadata = std::string("{\"cartographer_triangle_faces\":") + large + "}";
        check(!core::json::parse_object(metadata), "default JSON limit remains intact");
        check(bool(core::json::parse_object_with_numeric_array_limit(metadata, {"cartographer_triangle_faces"}, 200000)),
              "large numeric provenance is accepted");
        check(!core::json::parse_object_with_numeric_array_limit(
                  std::string("{\"cartographer_triangle_faces\":") + make_array(200001) + "}",
                  {"cartographer_triangle_faces"}, 200000), "provenance bound enforced");
        check(!core::json::parse_object_with_numeric_array_limit(
                  "{\"cartographer_triangle_faces\":[[1]]}", {"cartographer_triangle_faces"}, 200000),
              "nested provenance entries rejected");
        check(!core::json::parse_object_with_numeric_array_limit(
                  std::string("{\"unrelated\":") + make_array(4097) + "}",
                  {"cartographer_triangle_faces"}, 200000), "unrelated array bound preserved");
        check(!core::json::parse_object_with_numeric_array_limit(
                  "{\"cartographer_triangle_faces\":[1],\"cartographer_triangle_faces\":[2]}",
                  {"cartographer_triangle_faces"}, 200000), "duplicate provenance keys rejected");
        check(!core::json::parse_object_with_numeric_array_limit(
                  std::string("{\"other\":{\"cartographer_triangle_faces\":") + make_array(4097) + "}}",
                  {"meshes", "primitives", "extras", "cartographer_triangle_faces"}, 200000),
              "same key outside the schema path remains bounded");
        const auto with_metadata = replace(fixture(), "\"indices\":4,\"material\":0}",
                  std::string("\"indices\":4,\"material\":0,\"extras\":") + metadata + "}");
        check(bool(io::import_gltf_scene(with_metadata)), "large provenance scene imports");
        const auto root =
            std::filesystem::temp_directory_path() /
            ("static-gltf-import-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        // A filesystem-dependent .gltf is snapshotted once; subsequent parsing
        // must use the captured bytes even if the sibling changes or disappears.
        {
            const std::string json = R"({"asset":{"version":"2.0"},"images":[{"uri":"albedo.png"}]})";
            const std::vector<std::uint8_t> source(json.begin(), json.end());
            const std::vector<std::uint8_t> png = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,248,207,192,240,31,0,5,0,1,255,137,153,61,29,0,0,0,0,73,69,78,68,174,66,96,130};
            const auto path = root / "external.gltf";
            { std::ofstream out(root / "albedo.png", std::ios::binary); out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size())); }
            const auto captured = take(io::capture_gltf_scene_source(path, source));
            std::filesystem::remove(root / "albedo.png");
            const auto materials = take(io::import_gltf_materials(captured, false));
            check(materials.encoded_images.size() == 1 && materials.encoded_images[0] == png,
                  "external image is captured and imported without filesystem access");
            // Exercise the same captured image through the scene material path.
            { std::ofstream out(root / "albedo.png", std::ios::binary); out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size())); }
            auto scene_source = replace(fixture(), "\"materials\":[", "\"images\":[{\"uri\":\"albedo.png\"}],\"textures\":[{\"source\":0}],\"materials\":[");
            scene_source = replace(scene_source, "\"pbrMetallicRoughness\":{", "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},");
            const auto captured_scene = take(io::capture_gltf_scene_source(path, scene_source));
            std::filesystem::remove(root / "albedo.png");
            const auto scene = take(io::import_gltf_scene(captured_scene));
            auto all_slots_source = replace(scene_source, "\"baseColorTexture\":{\"index\":0},", "\"baseColorTexture\":{\"index\":0},\"metallicRoughnessTexture\":{\"index\":0},");
            all_slots_source = replace(all_slots_source, "\"pbrMetallicRoughness\":{", "\"normalTexture\":{\"index\":0},\"occlusionTexture\":{\"index\":0},\"emissiveTexture\":{\"index\":0},\"pbrMetallicRoughness\":{");
            { std::ofstream out(root / "albedo.png", std::ios::binary); out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size())); }
            const auto all_slots = take(io::import_gltf_scene(take(io::capture_gltf_scene_source(path, all_slots_source))));
            std::filesystem::remove(root / "albedo.png");
            const auto& definition = all_slots.materials.catalog.materials().begin()->second.definition;
            check(definition.normal.normal_convention == render::NormalConvention::opengl_y_plus &&
                  definition.base_color.normal_convention == render::NormalConvention::directx_y_minus &&
                  definition.metallic_roughness.normal_convention == render::NormalConvention::directx_y_minus &&
                  definition.occlusion.normal_convention == render::NormalConvention::directx_y_minus &&
                  definition.emissive.normal_convention == render::NormalConvention::directx_y_minus,
                  "glTF +Y convention applies only to the normal texture slot");
            check(scene.materials.encoded_images.size() == 1 && scene.meshes.size() == 1,
                  "textured external-image scene imports after its sibling is removed");
            check(!io::import_gltf_materials(source, false), "uncaptured external image is refused");
            const auto mismatch = replace(source, "\"uri\":\"albedo.png\"", "\"uri\":\"albedo.png\",\"mimeType\":\"image/jpeg\"");
            { std::ofstream out(root / "albedo.png", std::ios::binary); out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size())); }
            const auto mismatch_package = take(io::capture_gltf_scene_source(path, mismatch));
            check(!io::import_gltf_materials(mismatch_package, false), "conflicting MIME is refused");
            std::filesystem::remove(root / "albedo.png");
            const std::string escape = R"({"asset":{"version":"2.0"},"images":[{"uri":"../albedo.png"}]})";
            check(!io::capture_gltf_scene_source(path, {reinterpret_cast<const std::uint8_t*>(escape.data()), escape.size()}),
                  "external image path traversal is refused");
        }
        for (bool binary : {false, true}) {
            auto bytes = fixture(binary);
            auto parsed = take(io::import_gltf_scene(bytes));
            check(parsed.meshes.size() == 1 && parsed.nodes.size() == 3,
                  "real geometry and full hierarchy decoded");
            check(parsed.nodes[2].local_transform.scale.x == -1 &&
                      parsed.nodes[2].local_transform.scale.y == 2,
                  "mirrored matrix decomposes without losing scale");
            production::RenderMeshCompileOptions options;
            options.uv_set = render::kUv0;
            options.generate_tangents = true;
            options.material_slot_layer = "cartographer.material_slot";
            auto render = take(production::compile_render_mesh_artifact(
                {"import.fixture", core::Revision{1}, parsed.materials.artifact_digest},
                parsed.meshes[0], options));
            check(render.vertices.size() == 3 && render.indices.size() == 3 &&
                      render.submeshes[0].material_slot == 1,
                  "indexed topology and material region reach production stream");
            for (const auto &v : render.vertices)
                check(v.normal.z == 1 && v.tangent && v.tangent->x == 0 && v.tangent->y == 1 &&
                          v.tangent->w == -1,
                      "custom tangent direction and handedness survive compilation");
            auto uv_edit = parsed.meshes[0];
            auto attrs = uv_edit.attributes();
            auto layer = *attrs.find("gltf.uv." + std::to_string(render::kUv0));
            auto p = std::get<core::Vec2d>(layer.values[0]);
            p.x += .125;
            layer.values[0] = p;
            okay(attrs.remove_layer(layer.descriptor.id));
            okay(attrs.add_layer(std::move(layer)));
            okay(uv_edit.set_attribute_payload(std::move(attrs), uv_edit.uv_sets()));
            const auto regenerated = take(production::compile_render_mesh_artifact(
                {"import.fixture", core::Revision{1}, parsed.materials.artifact_digest}, uv_edit,
                options));
            check(regenerated.vertices[0].tangent && regenerated.vertices[0].tangent->x > .9,
                  "UV edits invalidate imported tangent correspondence and "
                  "regenerate the current UV frame");
            application::ApplicationSession session;
            send(session, application::NewProjectAction{"Imported synthetic glTF"});
            const auto path = root / (binary ? "binary.carto" : "json.carto");
            send(session, application::SaveProjectAction{path});
            auto snapshot = session.snapshot();
            const auto before = document(session).serialize();
            const auto input_path = root / (binary ? "import-source.glb" : "import-source.gltf");
            {
                std::ofstream output(input_path, std::ios::binary);
                output.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
                check(bool(output), "scene import fixture written");
            }
            check(!carto::testing::import_gltf_scene_file(session, input_path, false),
                  "rights confirmation is enforced");
            check(document(session).serialize() == before,
                  "rights refusal leaves source unchanged");
            take(carto::testing::import_gltf_scene_file(session, input_path, true));
            auto imported = document(session);
            check(imported.meshes().size() == 1 && imported.scene().size() == 3 &&
                      imported.material_catalog().assignments().size() == 1,
                  "scene and materials admitted atomically");
            check(imported.scene().find({2})->mesh_asset ==
                          imported.scene().find({3})->mesh_asset &&
                      imported.scene().find({3})->parent == scene::ObjectId{1},
                  "shared mesh and parent identity retained");
            check(imported.source_asset_registry().records().size() == 1 &&
                      imported.source_asset_registry().records().begin()->second.document_products.size() == 5 &&
                      imported.source_asset_registry().records().begin()->second.domain_digests.size() == 3,
                  "source identity, domain provenance and product mappings retained locally");
            const auto good_source=imported.serialize();auto corrupt=bytes;corrupt.pop_back();snapshot=session.snapshot();
            const auto corrupt_path = root / (binary ? "corrupt.glb" : "corrupt.gltf");
            {
                std::ofstream output(corrupt_path, std::ios::binary);
                output.write(reinterpret_cast<const char*>(corrupt.data()),
                             static_cast<std::streamsize>(corrupt.size()));
                check(bool(output), "corrupt scene fixture written");
            }
            check(!carto::testing::import_gltf_scene_file(session,corrupt_path,true),"malformed scene refuses through actual application action");
            check(document(session).serialize()==good_source,"failed scene replacement preserves geometry, materials, hierarchy and provenance");
            send(session, application::UndoAction{});
            const auto undone = document(session);
            check(undone.meshes().empty() && undone.scene().size() == 0 &&
                      undone.material_catalog().empty() &&
                      undone.source_asset_registry().records().empty(),
                  "one undo removes complete scene and material admission while the journal "
                  "revision advances");
            send(session, application::RedoAction{});
            auto changed = imported.scene().find({2})->local_transform;
            changed.translation.x = .25;
            send(session, application::SetObjectTransformAction{{2}, changed, {}});
            send(session, application::UndoAction{});
            send(session, application::RedoAction{});
            send(session, application::SaveProjectAction{std::nullopt});
            auto reopened = take(project::ProjectDocument::load(path));
            check(reopened.scene().find({2})->local_transform.translation.x == .25,
                  "native action edit, undo/redo and saved reopen retain imported "
                  "geometry correspondence");
            production::AuthoringViewportRequest request;
            request.width = request.height = 48;
            request.view.position = {.5, .5, 3};
            request.view.rotation = {0, 0, 0, 1};
            request.view.orthographic_height = 2;
            const auto image = take(production::render_authoring_viewport(reopened, request));
            std::size_t covered = 0;
            for (std::size_t i = 0; i < image.objects.size(); ++i)
                if (image.objects[i]) {
                    ++covered;
                    check(image.linear_hdr[i].x >= 1 && image.linear_hdr[i].y >= .2,
                          "authored emission reaches rendered reference frame");
                }
            check(covered > 40, "reference frame covers actual imported mesh");
            const auto output = root / (binary ? "roundtrip.glb" : "roundtrip.gltf");
            io::GltfExportOptions export_options;
            export_options.project_path = path;
            if (binary)
                take(io::export_glb(reopened, output, export_options));
            else
                take(io::export_gltf(reopened, output, export_options));
            const auto encoded = read(output);
            const auto roundtrip = take(io::import_gltf_scene(
                {reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size()}));
            check(roundtrip.nodes.size() == 3 && roundtrip.meshes.size() == 1 &&
                      roundtrip.materials.catalog.materials().size() == 1,
                  "import/edit/save/reopen/export/reimport retains supported "
                  "appearance and sharing");
            check(!io::import_generated_botanical_glb(bytes, "synthetic.source", core::Revision{1},
                                                      assets::sha256(bytes)),
                  "general scene path does not weaken botanical provenance fence");
        }
        // Distinct authored RGBA values, including zero/intermediate alpha,
        // must survive the real application admission and both export formats.
        const std::array<std::array<float, 4>, 3> rgba{{
            {0.125f, 0.25f, 0.5f, 0.0f}, {1.0f, 0.5f, 0.25f, 0.5f},
            {0.0f, 0.75f, 1.0f, 1.0f}}};
        const auto colored_bytes = color_fixture(rgba);
        check_corner_colors(take(io::import_gltf_scene(colored_bytes)).meshes[0], rgba);
        auto rgb_expected = rgba;
        for (auto& c : rgb_expected) c[3] = 1.0f;
        check_corner_colors(take(io::import_gltf_scene(color_fixture(rgba, 3))).meshes[0], rgb_expected);
        const std::array<std::array<float, 4>, 3> byte_colors{{
            {0, 51, 102, 153}, {255, 204, 153, 102}, {51, 0, 255, 255}}};
        const auto normalized = take(io::import_gltf_scene(color_fixture(byte_colors, 4, true)));
        const auto* normalized_layer = normalized.meshes[0].attributes().find("gltf.color0");
        check(normalized_layer && normalized_layer->values.size() == 3, "normalized unsigned colors import");
        for (std::size_t i = 0; i < 3; ++i) {
            const auto c = std::get<core::Vec4d>(normalized_layer->values[i]);
            check(c.x == byte_colors[i][0] / 255.0 && c.y == byte_colors[i][1] / 255.0 &&
                  c.z == byte_colors[i][2] / 255.0 && c.w == byte_colors[i][3] / 255.0,
                  "normalized unsigned RGBA values decode exactly");
        }
        for (bool binary : {false, true}) {
            application::ApplicationSession session;
            send(session, application::NewProjectAction{"Vertex color roundtrip"});
            const auto project_path = root / (binary ? "colors-binary.carto" : "colors-json.carto");
            send(session, application::SaveProjectAction{project_path});
            const auto input = root / (binary ? "colors-input-binary.gltf" : "colors-input-json.gltf");
            { std::ofstream file(input, std::ios::binary);
              file.write(reinterpret_cast<const char*>(colored_bytes.data()), colored_bytes.size());
              check(bool(file), "color input written"); }
            take(carto::testing::import_gltf_scene_file(session, input, true));
            const auto imported = document(session);
            check_corner_colors(imported.meshes().begin()->second, rgba);
            const auto output = root / (binary ? "colors-roundtrip.glb" : "colors-roundtrip.gltf");
            io::GltfExportOptions options;
            options.project_path = project_path;
            if (binary) take(io::export_glb(imported, output, options));
            else take(io::export_gltf(imported, output, options));
            const auto encoded = read(output);
            const auto decoded = take(io::import_gltf_scene({
                reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()}));
            check_corner_colors(decoded.meshes[0], rgba);
        }
        auto invalid_colors = rgba;
        invalid_colors[1][2] = 1.01f;
        reject(color_fixture(invalid_colors), "color above one is rejected rather than clamped");
        invalid_colors[1][2] = -0.01f;
        reject(color_fixture(invalid_colors), "negative color is rejected rather than clamped");
        invalid_colors[1][2] = std::bit_cast<float>(std::uint32_t{0x7fc00000});
        reject(color_fixture(invalid_colors), "NaN color is rejected");
        reject(replace(colored_bytes, "\"type\":\"VEC4\"", "\"type\":\"SCALAR\""),
               "scalar color shape is rejected");
        reject(replace(colored_bytes, "\"normalized\":false,\"count\":3", "\"normalized\":false,\"count\":2"),
               "color accessor shorter than position accessor is rejected");
        reject(replace(color_fixture(byte_colors, 4, true), "\"normalized\":true", "\"normalized\":false"),
               "unnormalized integer colors are rejected");
        const auto json = fixture();
        reject(replace(json, "\"count\":3", "\"count\":600001"), "excessive accessor rejected");
        reject(replace(json, "\"byteOffset\":144", "\"byteOffset\":18446744073709551615"),
               "overflow offset rejected");
        reject(replace(json, "\"byteStride\":48", "\"byteStride\":4"),
               "overlapping short vertex stride rejected");
        reject(replace(json, "\"children\":[1,2]", "\"children\":[0,2]"),
               "hierarchy cycle rejected");
        reject(replace(json, "\"doubleSided\":true", "\"doubleSided\":false"),
               "unsupported culling does not silently change");
        reject(replace(json, "\"asset\":", "\"skins\":[{}],\"asset\":"),
               "skin is not silently dropped");
        const auto animated=replace(json,"\"asset\":","\"animations\":[{}],\"asset\":");
        const auto animation_result=io::import_gltf_scene(animated);
        check(!animation_result&&animation_result.error().message.find("animations require an explicit compatible adapter")!=std::string::npos,"static scene import reports animation loss as refusal, not rig/animation support");
        reject(replace(json, "\"indices\":4", "\"indices\":4,\"targets\":[{}]"),
               "morph targets rejected");
        {
            // COLOR_0 imports as the gltf.color0 corner color4 layer; a
            // malformed (scalar) colour accessor is still refused.
            const auto colored = io::import_gltf_scene(
                replace(json, "\"POSITION\":0", "\"POSITION\":0,\"COLOR_0\":0"));
            check(static_cast<bool>(colored), "VEC3 vertex color imports");
        }
        reject(replace(json, "\"POSITION\":0", "\"POSITION\":0,\"COLOR_0\":4"),
               "scalar vertex color accessor rejected");
        reject(replace(json, "\"componentType\":5121", "\"componentType\":5126"),
               "float indices rejected");
        auto unsafe =
            replace(json, "data:application/octet-stream;base64,", "https://example.invalid/");
        reject(unsafe, "network URI rejected without fetch");
        unsafe = replace(json, "data:application/octet-stream;base64,", "../");
        reject(unsafe, "traversal URI rejected without filesystem reads");
        auto broken = fixture(true);
        broken.back() = 255;
        broken[broken.size() - 4] = 255;
        reject(broken, "out of range index rejected");
        broken = fixture(true);
        broken.pop_back();
        reject(broken, "truncated GLB rejected");
        if (argc == 3 && std::string_view(argv[1]) == "--write-fixtures") {
            std::filesystem::create_directories(argv[2]);
            for (bool binary : {false, true}) {
                const auto bytes = fixture(binary);
                std::ofstream f(std::filesystem::path(argv[2]) /
                                    (binary ? "source.glb" : "source.gltf"),
                                std::ios::binary);
                f.write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            }
        }
        std::cout << "static GLTF/GLB import, authored tangents, hierarchy, atomic "
                     "authoring lifecycle, reference pixels and roundtrip passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
