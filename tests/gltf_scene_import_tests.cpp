#include "application_access.hpp"
#include "gltf_test_payload.hpp"
#include "scene_import_test_support.hpp"
#include <carto/io/generated_mesh.hpp>
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
std::vector<std::uint8_t> fixture(bool glb = false) {
    // Independently authored synthetic glTF, not produced by Cartographer's
    // exporter: interleaved float vertices, uint8 indices, two mesh instances.
    std::vector<std::uint8_t> binary;
    for (const auto p : std::array<core::Vec2d, 3>{{{0, 0}, {1, 0}, {0, 1}}})
        for (float x : {float(p.x), float(p.y), 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, 0.f, -1.f, float(p.x),
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
        const auto root =
            std::filesystem::temp_directory_path() /
            ("static-gltf-import-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
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
        reject(replace(json, "\"POSITION\":0", "\"POSITION\":0,\"COLOR_0\":0"),
               "unsupported vertex color rejected");
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
