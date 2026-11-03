#include <array>
#include <locale>
#include <bit>
#include <carto/core/json.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <carto/io/gltf_scene_import.hpp>
#include <carto/mesh_attributes/provenance.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/project/transaction.hpp>
#include "../apps/desktop/interchange_export_feedback.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
using namespace carto;
void check(bool v, const char *s) {
    if (!v)
        throw std::runtime_error(s);
}
template <class T> T take(core::Result<T> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
    return std::move(r.value());
}
void okay(core::Result<void> r) {
    if (!r)
        throw std::runtime_error(r.error().message);
}
std::string read(const std::filesystem::path &p) {
    std::ifstream f(p, std::ios::binary);
    check(bool(f), "fixture file readable");
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string_view member(std::string_view text, std::string_view key) {
    auto parsed = take(core::json::parse_object(text));
    auto m = core::json::find_member(parsed, key);
    check(m != nullptr, "JSON member present");
    return m->raw_value;
}
std::uint64_t number(std::string_view s) { return take(core::json::parse_uint(s)); }
std::vector<std::string_view> array(std::string_view s) {
    check(s.front() == '[' && s.back() == ']', "JSON array shape");
    std::vector<std::string_view> result;
    std::size_t start = 1;
    int depth = 0;
    bool quoted = false, escaped = false;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
            --depth;
        else if (c == ',' && depth == 0) {
            result.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (s.substr(start, s.size() - 1 - start).find_first_not_of(" \r\n\t") !=
        std::string_view::npos)
        result.push_back(s.substr(start, s.size() - 1 - start));
    return result;
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
    check(p + 4 <= b.size(), "binary u32 bound");
    return b[p] | (std::uint32_t(b[p + 1]) << 8) | (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}
std::vector<std::uint8_t> unbase64(std::string_view s) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> out;
    std::uint32_t bits = 0;
    unsigned count = 0;
    for (char c : s) {
        if (c == '=')
            break;
        auto n = alphabet.find(c);
        check(n != alphabet.npos, "embedded buffer base64 alphabet");
        bits = (bits << 6) | static_cast<unsigned>(n);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::uint8_t>(bits >> count));
        }
    }
    return out;
}
struct Payload {
    std::string json;
    std::vector<std::uint8_t> bytes;
    explicit Payload(const std::filesystem::path &p) {
        auto file = read(p);
        if (p.extension() == ".glb") {
            auto raw = std::span(reinterpret_cast<const std::uint8_t *>(file.data()), file.size());
            check(u32(raw, 0) == 0x46546c67 && u32(raw, 4) == 2 && u32(raw, 8) == raw.size(),
                  "GLB header and length");
            auto length = u32(raw, 12);
            check(u32(raw, 16) == 0x4e4f534a && 20ULL + length + 8 <= raw.size(), "GLB JSON chunk");
            json = file.substr(20, length);
            auto offset = 20 + length;
            auto bin = u32(raw, offset);
            check(u32(raw, offset + 4) == 0x004e4942 && offset + 8ULL + bin == raw.size(),
                  "GLB BIN chunk");
            bytes.assign(raw.begin() + offset + 8, raw.end());
        } else {
            json = std::move(file);
            auto uri =
                take(core::json::decode_string(member(array(member(json, "buffers"))[0], "uri")));
            constexpr std::string_view prefix = "data:application/octet-stream;base64,";
            check(uri.starts_with(prefix), "single embedded glTF buffer");
            bytes = unbase64(std::string_view(uri).substr(prefix.size()));
        }
        check(bool(core::json::parse_object(json)), "export JSON grammar");
        check(number(member(array(member(json, "buffers"))[0], "byteLength")) <= bytes.size(),
              "buffer length bounded");
    }
    std::vector<double> accessor(std::size_t id) const {
        auto a = array(member(json, "accessors")).at(id);
        auto v = array(member(json, "bufferViews")).at(number(member(a, "bufferView")));
        auto offset = number(member(v, "byteOffset"));
        auto count = number(member(a, "count"));
        auto component = number(member(a, "componentType"));
        auto type = take(core::json::decode_string(member(a, "type")));
        unsigned channels = type == "SCALAR" ? 1
                            : type == "VEC2" ? 2
                            : type == "VEC3" ? 3
                            : type == "VEC4" ? 4
                                             : 0;
        check(channels != 0, "accessor type");
        unsigned size = component == 5123 ? 2 : 4;
        check(component == 5123 || component == 5125 || component == 5126, "accessor component");
        check(count * channels * size <= number(member(v, "byteLength")) &&
                  offset + count * channels * size <= bytes.size(),
              "accessor views bounded");
        std::vector<double> out;
        for (std::size_t i = 0; i < count * channels; ++i) {
            auto pos = offset + i * size;
            if (component == 5126)
                out.push_back(std::bit_cast<float>(u32(bytes, pos)));
            else if (size == 4)
                out.push_back(u32(bytes, pos));
            else
                out.push_back(bytes[pos] | (unsigned(bytes[pos + 1]) << 8));
        }
        return out;
    }
    std::string_view primitive() const {
        return array(member(array(member(json, "meshes"))[0], "primitives"))[0];
    }
    std::vector<double> attribute(std::string_view name) const {
        return accessor(number(member(member(primitive(), "attributes"), name)));
    }
    std::vector<double> indices() const { return accessor(number(member(primitive(), "indices"))); }
};
geometry::EditableMesh fixture(bool uv = true, bool smooth = false) {
    geometry::EditableMesh m;
    okay(m.insert_bulk({{{1}, {0, 0, 0}}, {{2}, {1, 0, 0}}, {{3}, {1, 1, 0}}, {{4}, {0, 1, 0}}},
                       {{{1}, {{1}, {2}, {3}}}, {{2}, {{1}, {3}, {4}}}}));
    attributes::AttributeSet a({{attributes::AttributeDomain::corner, 6},
                                {attributes::AttributeDomain::face, 2}});
    std::vector<attributes::AttributeValue> normals;
    for (unsigned i = 0; i < 6; ++i)
        normals.push_back(smooth  ? core::Vec3d{0, 0, 1}
                          : i < 3 ? core::Vec3d{.6, 0, .8}
                                  : core::Vec3d{0, .8, .6});
    okay(a.add_layer({{"cartographer.normal0", attributes::AttributeDomain::corner,
                       attributes::AttributeType::vec3},
                      normals}));
    attributes::UvSetTable sets;
    if (uv) {
        std::vector<attributes::AttributeValue> coords{core::Vec2d{0, 0},     core::Vec2d{1, 0},
                                                       core::Vec2d{1, 1},     core::Vec2d{.25, .25},
                                                       core::Vec2d{.75, .75}, core::Vec2d{1, .25}};
        if (smooth)
            coords = {core::Vec2d{0, 0}, core::Vec2d{1, 0}, core::Vec2d{1, 1},
                      core::Vec2d{0, 0}, core::Vec2d{1, 1}, core::Vec2d{0, 1}};
        okay(a.add_layer(
            {{"uv.export", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
             coords}));
        okay(sets.add({7, "Render UV", "uv.export", true, true, false, 1}));
    }
    okay(m.set_attribute_payload(std::move(a), std::move(sets)));
    return m;
}
struct Scene {
    project::ProjectDocument doc;
    std::uint64_t mesh;
};
Scene make_scene(const geometry::EditableMesh &mesh, const std::filesystem::path &root,
                 bool mirrored = true) {
    auto doc = take(project::ProjectDocument::create("Independent export geometry fixture"));
    journal::Journal journal(root / "create.log");
    if (doc.revision() != core::Revision{}) {
        const auto checkpoint = doc.serialize();
        static_cast<void>(take(journal.append(
            {{},
             doc.revision(),
             "export.fixture.create",
             {reinterpret_cast<const std::uint8_t *>(checkpoint.data()), checkpoint.size()},
             std::nullopt})));
    }
    auto tx = take(project::ProjectTransaction::begin(doc, journal, "export.geometry.fixture"));
    auto id = take(tx.add_mesh(mesh));
    auto object = take(
        tx.create_object(mirrored ? "Mirrored nonuniform object" : "Positive nonuniform object",
                         {{1, 2, 3}, {0, .5176380902050415, 0, 1.9318516525781366},
                          {mirrored ? -2.0 : 2.0, 3, .5}}));
    okay(tx.attach_mesh(object, id));
    static_cast<void>(take(tx.commit()));
    return {std::move(doc), id};
}
void near(double a, double b, const char *message) {
    check(std::isfinite(a) && std::abs(a - b) < 2e-6, message);
}
Payload exported(const Scene &s, const std::filesystem::path &root, std::string name,
                 double expected_scale_x = -2.0) {
    const auto before = s.doc.serialize();
    const auto gltf = root / (name + ".gltf"), glb = root / (name + ".glb");
    auto a = take(io::export_gltf(s.doc, gltf));
    auto b = take(io::export_glb(s.doc, glb));
    check(s.doc.serialize() == before, "export never changes authoritative source");
    check(a.vertices == b.vertices && a.triangles == b.triangles && a.warnings == b.warnings,
          "glTF/GLB reports agree");
    Payload p(gltf), binary(glb);
    for (const auto key : {"POSITION", "NORMAL"})
        check(p.attribute(key) == binary.attribute(key),
              "GLB/glTF complete geometry payload parity");
    check(p.indices() == binary.indices(), "GLB/glTF complete index parity");
    const auto attrs = take(core::json::parse_object(member(p.primitive(), "attributes")));
    for (const auto key : {"TEXCOORD_0", "TANGENT"})
        if (core::json::find_member(attrs, key))
            check(p.attribute(key) == binary.attribute(key), "GLB/glTF UV/tangent payload parity");
    auto node = array(member(p.json, "nodes"))[0];
    auto q = array(member(node, "rotation"));
    double norm = 0;
    for (auto v : q)
        norm += std::stod(std::string(v)) * std::stod(std::string(v));
    near(norm, 1, "glTF quaternion normalized to authored rotation semantics");
    auto scale = array(member(node, "scale"));
    near(std::stod(std::string(scale[0])), expected_scale_x, "authored scale sign preserved");
    near(std::stod(std::string(scale[1])), 3, "nonuniform scale preserved");
    return p;
}
void normals_and_frames(const Payload &p, bool uv) {
    auto n = p.attribute("NORMAL"), pos = p.attribute("POSITION"), index = p.indices();
    check(index.size() == 6 && n.size() == 18,
          "corner normals split shared positions with and without UV");
    std::vector<double> u, t;
    if (uv) {
        u = p.attribute("TEXCOORD_0");
        t = p.attribute("TANGENT");
    }
    const std::array<core::Vec3d, 6> positions{
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 0}, {1, 1, 0}, {0, 1, 0}}};
    const std::array<core::Vec2d, 6> coords{
        {{0, 0}, {1, 0}, {1, 1}, {.25, .25}, {.75, .75}, {1, .25}}};
    for (unsigned c = 0; c < 6; ++c) {
        auto i = static_cast<std::size_t>(index[c]);
        auto normal = c < 3 ? core::Vec3d{.6, 0, .8} : core::Vec3d{0, .8, .6};
        near(n[i * 3], normal.x, "authored normal x");
        near(n[i * 3 + 1], normal.y, "authored normal y");
        near(n[i * 3 + 2], normal.z, "authored normal z");
        near(pos[i * 3], positions[c].x, "position x");
        near(pos[i * 3 + 1], positions[c].y, "position y");
        if (uv) {
            near(u[i * 2], coords[c].x, "UV seam u");
            near(u[i * 2 + 1], coords[c].y, "UV seam v");
            near(t[i * 4] * normal.x + t[i * 4 + 1] * normal.y + t[i * 4 + 2] * normal.z, 0,
                 "tangent orthogonal to authored normal");
            near(t[i * 4] * t[i * 4] + t[i * 4 + 1] * t[i * 4 + 1] + t[i * 4 + 2] * t[i * 4 + 2], 1,
                 "unit tangent");
            near(t[i * 4 + 3], c < 3 ? 1 : -1, "mirrored UV handedness preserved");
        }
    }
}
void replace(Scene &s, const geometry::EditableMesh &m, const std::filesystem::path &root,
             unsigned serial, std::optional<geometry::TopologyEditReceipt> receipt = {}) {
    journal::Journal journal(root / ("edit-" + std::to_string(serial) + ".log"));
    const auto before = s.doc.serialize();
    static_cast<void>(
        take(journal.append({{},
                             s.doc.revision(),
                             "export.fixture.checkpoint",
                             {reinterpret_cast<const std::uint8_t *>(before.data()), before.size()},
                             std::nullopt})));
    auto tx = take(project::ProjectTransaction::begin(s.doc, journal, "export.geometry.edit"));
    okay(tx.replace_mesh(s.mesh, m, receipt));
    static_cast<void>(take(tx.commit()));
}
void add_material(Scene &s, const std::filesystem::path &root) {
    project::MaterialCatalog catalog;
    render::MaterialAsset material;
    material.id = 71;
    material.name = "Authored material";
    material.revision = core::Revision{1};
    okay(catalog.insert_material(std::move(material)));
    journal::Journal journal(root / "add-material.log");
    const auto before = s.doc.serialize();
    static_cast<void>(take(journal.append(
        {{}, s.doc.revision(), "export.fixture.material-base",
         {reinterpret_cast<const std::uint8_t *>(before.data()), before.size()}, std::nullopt})));
    auto tx = take(project::ProjectTransaction::begin(s.doc, journal, "export.fixture.material"));
    okay(tx.set_material_catalog(std::move(catalog)));
    static_cast<void>(take(tx.commit()));
}

void set_export_catalog(Scene& scene, project::MaterialCatalog catalog,
                        const std::filesystem::path& journal_path) {
    journal::Journal journal(journal_path);
    const auto before = scene.doc.serialize();
    static_cast<void>(take(journal.append(
        {{}, scene.doc.revision(), "export.material.fixture-base",
         {reinterpret_cast<const std::uint8_t*>(before.data()), before.size()}, std::nullopt})));
    auto tx = take(project::ProjectTransaction::begin(scene.doc, journal, "export.material.fixture"));
    okay(tx.set_material_catalog(std::move(catalog)));
    static_cast<void>(take(tx.commit()));
}

project::MaterialCatalog scalar_catalog(std::uint64_t mesh, bool two_regions = false,
                                        double red = 0.25) {
    project::MaterialCatalog catalog;
    render::MaterialAsset material;
    material.id = 71; material.name = "Copper \"fixture\""; material.revision = core::Revision{1};
    material.definition.base_color_factor = {red, 0.5, 0.75, 1.0};
    material.definition.metallic = 0.8; material.definition.roughness = 0.27;
    okay(catalog.insert_material(std::move(material)));
    okay(catalog.insert_assignment({{mesh, 7U}, 71U, std::nullopt}));
    if (two_regions) okay(catalog.insert_assignment({{mesh, 8U}, 71U, std::nullopt}));
    return catalog;
}

geometry::EditableMesh material_mesh(bool two_regions = false, bool condition = false, bool reverse = false) {
    auto mesh = fixture();
    auto attrs = mesh.attributes();
    okay(attrs.add_layer({{condition ? "condition.material_region" : "cartographer.material_slot",
                           attributes::AttributeDomain::face, attributes::AttributeType::uint32},
                          {std::uint32_t{reverse && two_regions ? 8U : 7U},
                           std::uint32_t{two_regions && !reverse ? 8U : 7U}}}));
    okay(mesh.set_attribute_payload(std::move(attrs), mesh.uv_sets()));
    return mesh;
}


Scene region_limit_scene(const std::filesystem::path& root, std::size_t count) {
    geometry::EditableMesh mesh;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    auto layer = *material_mesh().attributes().find("cartographer.material_slot");
    layer.values.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t v = i * 3U + 1U;
        const double x = static_cast<double>(i * 2U);
        vertices.push_back({{v}, {x, 0, 0}});
        vertices.push_back({{v + 1U}, {x + 1, 0, 0}});
        vertices.push_back({{v + 2U}, {x, 1, 0}});
        faces.push_back({{i + 1U}, {{v}, {v + 1U}, {v + 2U}}});
        layer.values.push_back(static_cast<std::uint32_t>(1000U + i));
    }
    okay(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    attributes::AttributeSet attrs({{attributes::AttributeDomain::face, count}});
    okay(attrs.add_layer(std::move(layer)));
    okay(mesh.set_attribute_payload(std::move(attrs), {}));
    auto scene = make_scene(mesh, root);
    project::MaterialCatalog catalog;
    render::MaterialAsset material;
    material.id = 71U; material.name = "Shared bounded material";
    material.revision = core::Revision{1U};
    okay(catalog.insert_material(std::move(material)));
    for (std::size_t i = 0; i < count; ++i)
        okay(catalog.insert_assignment({{scene.mesh, static_cast<std::uint32_t>(1000U + i)},
                                        71U, std::nullopt}));
    set_export_catalog(scene, std::move(catalog), root / "materials.log");
    return scene;
}

void region_limit_pressure(const std::filesystem::path& root) {
    const auto accepted_root = root / "accepted";
    std::filesystem::create_directory(accepted_root);
    auto accepted = region_limit_scene(accepted_root, 128U);
    const auto payload = exported(accepted, accepted_root, "limit");
    const auto primitives = array(member(array(member(payload.json, "meshes")).at(0), "primitives"));
    check(primitives.size() == 128U, "128 assigned regions remain distinct even with one material");
    for (std::size_t i = 0; i < primitives.size(); ++i) {
        const auto extras = member(primitives[i], "extras");
        const auto faces = array(member(extras, "cartographer_triangle_faces"));
        check(number(member(primitives[i], "material")) == 0U &&
                  number(member(extras, "cartographer_material_region")) == 1000U + i &&
                  faces.size() == 1U && number(faces[0]) == i + 1U,
              "region limit preserves exact material, region and source-face correspondence");
    }
    const auto rejected_root = root / "rejected";
    std::filesystem::create_directory(rejected_root);
    auto rejected = region_limit_scene(rejected_root, 129U);
    const auto before_source = rejected.doc.serialize();
    for (const auto ext : {".gltf", ".glb"}) {
        const auto target = accepted_root / (std::string("limit") + ext);
        const auto before = read(target);
        const auto result = std::string_view(ext) == ".gltf" ?
            io::export_gltf(rejected.doc, target) : io::export_glb(rejected.doc, target);
        check(!result && result.error().message.find("128-region") != std::string::npos &&
                  read(target) == before && rejected.doc.serialize() == before_source,
              "129 regions refuse for the bound and preserve prior output and source");
    }
}

void polygon_region_export(const std::filesystem::path& root) {
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk({{{1}, {0, 0, 0}}, {{2}, {1, 0, 0}}, {{3}, {1, 1, 0}},
                           {{4}, {0, 1, 0}}, {{5}, {2, 0, 0}}},
                          {{{1}, {{1}, {2}, {3}, {4}}}, {{2}, {{2}, {5}, {3}}}}));
    attributes::AttributeSet attrs({{attributes::AttributeDomain::face, 2}});
    okay(attrs.add_layer({{"condition.material_region", attributes::AttributeDomain::face,
                           attributes::AttributeType::uint32},
                          {std::uint32_t{8U}, std::uint32_t{7U}}}));
    okay(mesh.set_attribute_payload(std::move(attrs), {}));
    auto scene = make_scene(mesh, root);
    set_export_catalog(scene, scalar_catalog(scene.mesh, true), root / "material.log");
    const auto payload = exported(scene, root, "polygon");
    const auto primitives = array(member(array(member(payload.json, "meshes")).at(0), "primitives"));
    check(primitives.size() == 2U, "same material on different regions remains partitioned");
    const auto first_faces = array(member(member(primitives[0], "extras"), "cartographer_triangle_faces"));
    const auto second_faces = array(member(member(primitives[1], "extras"), "cartographer_triangle_faces"));
    check(first_faces.size() == 1U && number(first_faces[0]) == 2U && second_faces.size() == 2U &&
              number(second_faces[0]) == 1U && number(second_faces[1]) == 1U,
          "triangulation records the actual source face for each output triangle");
    check(payload.accessor(number(member(primitives[0], "indices"))).size() == 3U &&
              payload.accessor(number(member(primitives[1], "indices"))).size() == 6U,
          "region index accessors cover each source triangle exactly once");
}

void constant_material_export_roundtrip(const std::filesystem::path& root) {
    auto edited_mesh = material_mesh();
    auto scene = make_scene(edited_mesh, root);
    set_export_catalog(scene, scalar_catalog(scene.mesh), root / "material.log");
    const auto source_before = scene.doc.serialize();
    const auto payload = exported(scene, root, "material-before");
    const auto primitive = payload.primitive();
    check(number(member(primitive, "material")) == 0U, "primitive binds authored material");
    check(number(member(member(primitive, "extras"), "cartographer_material_region")) == 7U,
          "primitive records its source material region");
    const auto material = array(member(payload.json, "materials")).at(0);
    check(member(material, "doubleSided") == "true", "export preserves double-sided viewport contract");
    check(number(member(member(material, "extras"), "cartographer_material_id")) == 71U,
          "material records its source identity");
    const auto pbr = member(material, "pbrMetallicRoughness");
    const auto color = array(member(pbr, "baseColorFactor"));
    near(std::stod(std::string(color[0])), 0.25, "exported red factor");
    near(std::stod(std::string(color[1])), 0.5, "exported green factor");
    near(std::stod(std::string(color[2])), 0.75, "exported blue factor");
    near(std::stod(std::string(member(pbr, "metallicFactor"))), 0.8, "exported metallic");
    near(std::stod(std::string(member(pbr, "roughnessFactor"))), 0.27, "exported roughness");
    check(!payload.attribute("TEXCOORD_0").empty(), "material export retains authored UVs");
    for (const auto ext : {".gltf", ".glb"}) {
        const auto bytes = read(root / (std::string("material-before") + ext));
        const auto imported = take(io::import_gltf_materials({
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}));
        check(imported.catalog.materials().size() == 1U && imported.encoded_images.empty(),
              "actual glTF material importer admits the emitted texture-free material");
        const auto imported_scene = take(io::import_gltf_scene({
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}));
        check(imported_scene.meshes.size() == 1U && imported_scene.meshes[0].faces_sorted().size() == 2U &&
                  imported_scene.meshes[0].uv_sets().active_for_render() != nullptr &&
                  imported_scene.materials.catalog.assignments().contains({1U, 1U}),
              "actual scene importer retains faces, active UVs and remapped material assignment");
        const auto* imported_slots = imported_scene.meshes[0].attributes().find("cartographer.material_slot");
        check(imported_slots != nullptr && imported_slots->values.size() == 2U &&
                  std::get<std::uint32_t>(imported_slots->values[0]) == 1U &&
                  std::get<std::uint32_t>(imported_slots->values[1]) == 1U,
              "scene importer attaches material to both faces");
        const auto& value = imported.catalog.materials().begin()->second.definition;
        near(value.base_color_factor.x, 0.25, "material importer sees authored factor");
        near(value.metallic, 0.8, "material importer sees metallic factor");
        near(value.roughness, 0.27, "material importer sees roughness factor");
    }
    check(scene.doc.serialize() == source_before, "material export leaves source unchanged");
    const auto saved = root / "material-source.carto";
    static_cast<void>(take(scene.doc.save_atomic(saved)));
    Scene reopened{take(project::ProjectDocument::load(saved)), scene.mesh};
    const auto after = exported(reopened, root, "material-reopened");
    check(after.bytes == payload.bytes, "material source save/reopen preserves geometry and UV bytes");
    check(member(after.primitive(), "material") == member(payload.primitive(), "material") &&
          member(array(member(after.json, "materials")).at(0), "pbrMetallicRoughness") == pbr,
          "material source save/reopen preserves the exported assignment and factors");

    // Existing mesh command undo/redo must not detach the material binding.
    editor::CommandBus bus;
    okay(bus.execute(std::make_unique<editor::SetVertexPositionCommand>(
        edited_mesh, geometry::VertexId{1}, core::Vec3d{0, 0, 0}, core::Vec3d{0, 0, .1})));
    replace(scene, edited_mesh, root, 21);
    check(number(member(exported(scene, root, "material-edited").primitive(), "material")) == 0U,
          "mesh edit retains assigned material");
    okay(bus.undo()); replace(scene, edited_mesh, root, 22);
    const auto undone = exported(scene, root, "material-undo");
    check(undone.bytes == payload.bytes && number(member(undone.primitive(), "material")) == 0U,
          "mesh undo retains material and restores source geometry bytes");
    okay(bus.redo()); replace(scene, edited_mesh, root, 23);
    check(number(member(exported(scene, root, "material-redo").primitive(), "material")) == 0U,
          "mesh redo retains assigned material");

    const auto ordering_root = root / "material-ordering";
    std::filesystem::create_directory(ordering_root);
    auto ordered = make_scene(material_mesh(), ordering_root);
    auto catalog = scalar_catalog(ordered.mesh);
    render::MaterialAsset unused;
    unused.id = 4U; unused.name = "Earlier unassigned material"; unused.revision = core::Revision{1U};
    okay(catalog.insert_material(std::move(unused)));
    set_export_catalog(ordered, std::move(catalog), ordering_root / "material.log");
    const auto ordered_payload = exported(ordered, ordering_root, "ordered");
    check(number(member(ordered_payload.primitive(), "material")) == 1U &&
              number(member(member(array(member(ordered_payload.json, "materials")).at(1), "extras"),
                            "cartographer_material_id")) == 71U,
          "primitive uses the sorted material's actual glTF index");
}
class CommaGroupedNumbers final : public std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

void material_export_locale_pressure(const std::filesystem::path& root) {
    auto scene = make_scene(material_mesh(), root);
    set_export_catalog(scene, scalar_catalog(scene.mesh), root / "material.log");
    const auto before_source = scene.doc.serialize();
    static_cast<void>(exported(scene, root, "locale"));
    const auto gltf = root / "locale.gltf", glb = root / "locale.glb";
    const auto before_gltf = read(gltf), before_glb = read(glb);
    const auto before_catalog = scene.doc.material_catalog().serialize();
    std::string during_source, during_catalog;
    bool project_decoded = false, catalog_decoded = false;

    struct RestoreLocale {
        std::locale previous;
        ~RestoreLocale() { std::locale::global(previous); }
    };
    {
        RestoreLocale restore;
        std::locale::global(std::locale(restore.previous, new CommaGroupedNumbers));
        during_source = scene.doc.serialize();
        during_catalog = scene.doc.material_catalog().serialize();
        project_decoded = static_cast<bool>(project::ProjectDocument::deserialize(before_source));
        catalog_decoded = static_cast<bool>(project::MaterialCatalog::deserialize(before_catalog));

        static_cast<void>(take(io::export_gltf(scene.doc, gltf)));
        static_cast<void>(take(io::export_glb(scene.doc, glb)));
    }
    { std::ofstream out(root / "canonical-before.carto", std::ios::binary); out << before_source; }
    { std::ofstream out(root / "canonical-during.carto", std::ios::binary); out << during_source; }
    { std::ofstream out(root / "catalog-before.carto-material", std::ios::binary); out << before_catalog; }
    { std::ofstream out(root / "catalog-during.carto-material", std::ios::binary); out << during_catalog; }
    std::cerr << "CANONICAL_LOCALE project_equal=" << (during_source == before_source)
              << " catalog_equal=" << (during_catalog == before_catalog)
              << " project_decode=" << project_decoded << " catalog_decode=" << catalog_decoded << '\n';
    check(during_source == before_source && during_catalog == before_catalog,
          "canonical project and material bytes must not depend on numeric locale");
    check(project_decoded && catalog_decoded,
          "canonical project and material bytes must decode under non-default numeric locale");
    check(read(gltf) == before_gltf && read(glb) == before_glb,
          "glTF/GLB bytes must preserve transforms and material factors under comma/grouped numeric locale");
    check(scene.doc.serialize() == before_source, "locale export does not mutate source");
}

} // namespace
int main(int argc, char** argv) {
    try {
        check(argc == 1 || argc == 2, "expected an optional new artifact directory");
        io::GltfExportReport feedback_report;
        feedback_report.path = "feedback.gltf";
        feedback_report.meshes = 1;
        feedback_report.triangles = 2;
        const auto clean_feedback = desktop::interchange_export_feedback(feedback_report);
        check(clean_feedback.find("1 mesh(es), 2 triangle(s)") != std::string::npos &&
                  clean_feedback.find("Export limitations:") == std::string::npos,
              "desktop export feedback retains successful counts");
        feedback_report.warnings = {"authored material omitted", "texture unavailable"};
        const auto warning_feedback = desktop::interchange_export_feedback(feedback_report);
        check(warning_feedback.find("Export limitations:") != std::string::npos &&
                  warning_feedback.find("authored material omitted") != std::string::npos &&
                  warning_feedback.find("texture unavailable") != std::string::npos,
              "desktop export feedback surfaces every loss warning");
        const auto root = argc == 2 ? std::filesystem::path(argv[1]) :
            std::filesystem::temp_directory_path() /
            ("gltf-geometry-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        check(!std::filesystem::exists(root), "fixture directory must be new; prior artifacts are preserved");
        std::filesystem::create_directories(root);
        const auto scalar_root = root / "constant-material";
        std::filesystem::create_directory(scalar_root);
        constant_material_export_roundtrip(scalar_root);
        const auto locale_root = root / "locale-pressure";
        std::filesystem::create_directory(locale_root);
        material_export_locale_pressure(locale_root);
        const auto polygon_root = root / "polygon-regions";
        std::filesystem::create_directory(polygon_root);
        polygon_region_export(polygon_root);
        const auto limit_root = root / "region-limit";
        std::filesystem::create_directory(limit_root);
        region_limit_pressure(limit_root);
        auto mesh = fixture();
        auto s = make_scene(mesh, root);
        auto p = exported(s, root, "baseline");
        normals_and_frames(p, true);
        auto saved = root / "source.carto";
        static_cast<void>(take(s.doc.save_atomic(saved)));
        Scene reopened_scene{take(project::ProjectDocument::load(saved)), s.mesh};
        auto reopened = exported(reopened_scene, root, "reopened");
        check(reopened.bytes == p.bytes, "save/reopen keeps full geometry payload");
        const auto positive_root = root / "positive-placement";
        std::filesystem::create_directory(positive_root);
        const auto positive = make_scene(fixture(), positive_root, false);
        const auto positive_path = positive_root / "source.carto";
        static_cast<void>(take(positive.doc.save_atomic(positive_path)));
        check(take(project::ProjectDocument::load(positive_path)).serialize() == positive.doc.serialize(),
              "separately authored positive-scale fixture survives project save/reopen");
        // Existing standalone command bus provides actual execute/undo/redo; these
        // exports do not claim native UI or application-level undo acceptance.
        editor::CommandBus bus;
        okay(bus.execute(std::make_unique<editor::SetVertexPositionCommand>(
            mesh, geometry::VertexId{1}, core::Vec3d{0, 0, 0}, core::Vec3d{0, 0, .1})));
        replace(s, mesh, root, 1);
        auto edited = exported(s, root, "edited");
        check(edited.bytes != p.bytes, "geometry edits reach exported payload");
        okay(bus.undo());
        replace(s, mesh, root, 2);
        check(exported(s, root, "undo").bytes == p.bytes,
              "mesh command undo restores exact geometry payload");
        okay(bus.redo());
        replace(s, mesh, root, 3);
        check(exported(s, root, "redo").bytes == edited.bytes,
              "mesh command redo restores edited payload");
        auto no_uv_root = root / "no-uv";
        std::filesystem::create_directory(no_uv_root);
        auto bare = make_scene(fixture(false), no_uv_root);
        normals_and_frames(exported(bare, no_uv_root, "no-uv"), false);
        auto smooth_root = root / "smooth";
        std::filesystem::create_directory(smooth_root);
        auto smooth = make_scene(fixture(true, true), smooth_root);
        check(exported(smooth, smooth_root, "smooth").attribute("POSITION").size() == 12,
              "shared continuous UV and normals reuse viewport vertices");
        auto cut_root = root / "cut";
        std::filesystem::create_directory(cut_root);
        auto cutmesh = fixture();
        auto cut = make_scene(cutmesh, cut_root);
        auto receipt = take(mesh_attributes::knife_cut_with_attribute_transfer(
            cutmesh, geometry::FaceId{1}, {{.5, 0, 0}, {.5, .5, 0}}));
        replace(cut, cutmesh, cut_root, 1, receipt);
        auto cut_payload = exported(cut, cut_root, "cut");
        check(cut_payload.indices().size() > 6,
              "supported attribute-transfer topology edit survives export");
        production::RenderMeshCompileOptions options;
        options.uv_set = 7;
        options.generate_tangents = true;
        auto artifact = take(
            production::compile_render_mesh_artifact("export-fixture", cut.doc, cut.mesh, options));
        auto cn = cut_payload.attribute("NORMAL"), cu = cut_payload.attribute("TEXCOORD_0"),
             ct = cut_payload.attribute("TANGENT");
        check(cn.size() == artifact.vertices.size() * 3,
              "transfer output matches viewport vertex stream");
        for (std::size_t i = 0; i < artifact.vertices.size(); ++i) {
            const auto &v = artifact.vertices[i];
            near(cn[i * 3], v.normal.x, "transferred normal matches viewport");
            near(cu[i * 2], v.uv->x, "transferred UV matches viewport");
            near(ct[i * 4 + 3], v.tangent->w, "transferred tangent handedness matches viewport");
        }
        auto cut_saved = cut_root / "cut.carto";
        static_cast<void>(take(cut.doc.save_atomic(cut_saved)));
        Scene reopened_cut{take(project::ProjectDocument::load(cut_saved)), cut.mesh};
        check(exported(reopened_cut, cut_root, "cut-reopened").bytes == cut_payload.bytes,
              "transferred attributes survive save/reopen export");
        auto invalid = fixture();
        auto attributes = invalid.attributes();
        auto *found = attributes.find("cartographer.normal0");
        check(found != nullptr, "normal layer present");
        auto layer = *found;
        layer.values[0] = core::Vec3d{};
        okay(attributes.remove_layer("cartographer.normal0"));
        okay(attributes.add_layer(std::move(layer)));
        okay(invalid.set_attribute_payload(std::move(attributes), invalid.uv_sets()));
        replace(s, invalid, root, 4);
        const auto good = root / "baseline.gltf";
        auto before = read(good);
        check(!io::export_gltf(s.doc, good), "invalid authored normal fails visibly");
        check(read(good) == before, "failed normal export preserves prior good artifact");
        auto good_glb = root / "baseline.glb";
        auto glb_before = read(good_glb);
        check(!io::export_glb(s.doc, good_glb) && read(good_glb) == glb_before,
              "failed normal GLB export preserves prior good artifact");
        geometry::EditableMesh large_mesh;
        okay(large_mesh.insert_bulk(
            {{{1}, {100000000, 0, 0}}, {{2}, {100000001, 0, 0}}, {{3}, {100000000, 1, 0}}},
            {{{1}, {{1}, {2}, {3}}}}));
        const auto large_root = root / "float32-collapse";
        std::filesystem::create_directory(large_root);
        const auto large = make_scene(large_mesh, large_root);
        auto collapsed_gltf = io::export_gltf(large.doc, good);
        check(!collapsed_gltf && collapsed_gltf.error().message.find("float32") != std::string::npos,
              "large-coordinate triangle collapse is explicitly rejected for glTF");
        check(read(good) == before, "collapsed glTF export preserves prior artifact");
        auto collapsed_glb = io::export_glb(large.doc, good_glb);
        check(!collapsed_glb && collapsed_glb.error().message.find("float32") != std::string::npos,
              "large-coordinate triangle collapse is explicitly rejected for GLB");
        check(read(good_glb) == glb_before, "collapsed GLB export preserves prior artifact");
        auto material_root = root / "authored-material";
        std::filesystem::create_directory(material_root);
        auto material_scene = make_scene(fixture(), material_root);
        add_material(material_scene, material_root);
        auto material_export = io::export_gltf(material_scene.doc, good);
        check(!material_export && material_export.error().message.find("material catalog") != std::string::npos &&
                  read(good) == before,
              "authored material loss is refused without replacing prior glTF");
        const auto multi_root = root / "multi-region-material";
        std::filesystem::create_directory(multi_root);
        auto multi = make_scene(material_mesh(true, true, true), multi_root);
        auto multi_catalog = scalar_catalog(multi.mesh);
        render::MaterialAsset second;
        second.id = 72U; second.name = "Second region"; second.revision = core::Revision{1U};
        second.definition.base_color_factor = {0.1, 0.2, 0.3, 1.0};
        okay(multi_catalog.insert_material(std::move(second)));
        okay(multi_catalog.insert_assignment({{multi.mesh, 8U}, 72U, std::nullopt}));
        set_export_catalog(multi, std::move(multi_catalog), multi_root / "material.log");
        const auto multi_source = multi.doc.serialize();
        const auto multi_payload = exported(multi, multi_root, "multi");
        const Payload multi_binary(multi_root / "multi.glb");
        const auto primitives = array(member(array(member(multi_payload.json, "meshes")).at(0), "primitives"));
        const auto binary_primitives = array(member(array(member(multi_binary.json, "meshes")).at(0), "primitives"));
        check(primitives.size() == 2U && binary_primitives.size() == 2U,
              "both containers preserve two material regions as primitives");
        for (std::size_t i = 0; i < primitives.size(); ++i) {
            const auto extras = member(primitives[i], "extras");
            const auto binary_extras = member(binary_primitives[i], "extras");
            check(number(member(primitives[i], "material")) == i &&
                      number(member(extras, "cartographer_material_region")) == i + 7U &&
                      take(core::json::decode_string(member(extras, "cartographer_material_region_layer"))) ==
                          "condition.material_region" &&
                      number(member(extras, "cartographer_mesh_revision")) ==
                          multi.doc.meshes().at(multi.mesh).revision().value() &&
                      number(array(member(extras, "cartographer_triangle_faces")).at(0)) == 2U - i,
                  "primitive retains its material, region and producer triangle-face identity");
            check(member(primitives[i], "material") == member(binary_primitives[i], "material") &&
                      extras == binary_extras &&
                      multi_payload.accessor(number(member(primitives[i], "indices"))) ==
                          multi_binary.accessor(number(member(binary_primitives[i], "indices"))),
                  "GLB and glTF agree on every region and its indices");
        }
        check(array(member(member(member(primitives[0], "extras"), "condition_material_region"), "faces")).size() == 2U,
              "first primitive retains the legacy CONDITION full face table");
        const auto profile = member(member(multi_payload.json, "extras"), "cartographer");
        check(number(member(profile, "material_regions_version")) == 1U &&
                  take(core::json::decode_string(member(profile, "canonical_project_sha256"))) ==
                  assets::sha256({reinterpret_cast<const std::uint8_t*>(multi_source.data()),
                                  multi_source.size()}).hex(),
              "export binds canonical source serialization digest");
        check(number(member(profile, "project_schema")) == multi.doc.schema_version() &&
                  number(member(profile, "project_revision")) == multi.doc.revision().value(),
              "identity metadata describes the actual canonical serialization schema and revision");
        for (const auto ext : {".gltf", ".glb"}) {
            const auto bytes = read(multi_root / (std::string("multi") + ext));
            const auto imported = take(io::import_gltf_scene({
                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}));
            check(imported.meshes.size() == 1U && imported.meshes[0].faces_sorted().size() == 2U &&
                      imported.meshes[0].uv_sets().active_for_render() != nullptr &&
                      imported.materials.catalog.assignments().contains({1U, 1U}) &&
                      imported.materials.catalog.assignments().contains({1U, 2U}),
                  "scene import preserves both remapped assignments and active UVs");
            const auto* slots = imported.meshes[0].attributes().find("cartographer.material_slot");
            check(slots != nullptr && slots->values.size() == 2U &&
                      std::get<std::uint32_t>(slots->values[0]) == 1U &&
                      std::get<std::uint32_t>(slots->values[1]) == 2U,
                  "imported faces keep their distinct material assignments");
        }
        check(multi.doc.serialize() == multi_source, "multi-region export leaves source unchanged");
        const auto saved_multi = multi_root / "multi-source.carto";
        static_cast<void>(take(multi.doc.save_atomic(saved_multi)));
        Scene reopened_multi{take(project::ProjectDocument::load(saved_multi)), multi.mesh};
        const auto multi_reopened = exported(reopened_multi, multi_root, "multi-reopened");
        check(multi_reopened.bytes == multi_payload.bytes && multi_reopened.json == multi_payload.json,
              "multi-region project save/reopen retains derived data and metadata");
        const auto positive_material_root = root / "positive-material-regions";
        std::filesystem::create_directory(positive_material_root);
        auto positive_material = make_scene(material_mesh(true, true, true), positive_material_root, false);
        check(positive_material.mesh == multi.mesh, "independent authored fixture uses matching local mesh keys");
        set_export_catalog(positive_material, multi.doc.material_catalog(), positive_material_root / "material.log");
        const auto positive_material_source = positive_material.doc.serialize();
        const auto positive_material_payload = exported(positive_material, positive_material_root, "positive", 2.0);
        check(array(member(array(member(positive_material_payload.json, "meshes")).at(0), "primitives")).size() == 2U,
              "separately authored positive-scale scene preserves both material regions");
        const auto positive_material_path = positive_material_root / "positive-source.carto";
        static_cast<void>(take(positive_material.doc.save_atomic(positive_material_path)));
        check(take(project::ProjectDocument::load(positive_material_path)).serialize() == positive_material_source,
              "positive-scale material fixture preserves source across save/reopen");
        const auto partial_root = root / "partially-unassigned-material";
        std::filesystem::create_directory(partial_root);
        auto partial = make_scene(material_mesh(true), partial_root);
        set_export_catalog(partial, scalar_catalog(partial.mesh), partial_root / "material.log");
        const auto partial_source = partial.doc.serialize();
        const auto partial_gltf = io::export_gltf(partial.doc, good);
        check(!partial_gltf && read(good) == before && partial.doc.serialize() == partial_source,
              "unassigned face region refuses without changing source or replacing glTF");
        const auto partial_glb = io::export_glb(partial.doc, good_glb);
        check(!partial_glb && read(good_glb) == glb_before && partial.doc.serialize() == partial_source,
              "unassigned face region refuses without replacing GLB");
        const auto hdr_root = root / "out-of-range-material";
        std::filesystem::create_directory(hdr_root);
        auto hdr = make_scene(material_mesh(), hdr_root);
        set_export_catalog(hdr, scalar_catalog(hdr.mesh, false, 1.25), hdr_root / "material.log");
        const auto hdr_source = hdr.doc.serialize();
        check(!io::export_gltf(hdr.doc, good) && read(good) == before && hdr.doc.serialize() == hdr_source,
              "glTF-out-of-range base color refuses without clamping or mutation");
        const auto texture_root = root / "textured-material";
        std::filesystem::create_directory(texture_root);
        auto textured = make_scene(material_mesh(), texture_root);
        auto textured_catalog = scalar_catalog(textured.mesh);
        const std::array<std::uint8_t, 3U> fixture_digest_bytes{1U, 2U, 3U};
        okay(textured_catalog.insert_texture(project::TextureSourceRecord{
            .id = 501U, .name = "Texture identity fixture", .relative_locator = "textures/fixture.png",
            .content_digest = assets::sha256(fixture_digest_bytes),
            .format = assets::TextureSourceFormat::png, .width = 1U, .height = 1U,
            .provenance = "synthetic identity only", .revision = core::Revision{1U}}));
        okay(textured_catalog.insert_sampler(project::SamplerIntent{
            .id = 31U, .name = "Fixture sampler", .revision = core::Revision{1U}}));
        auto textured_material = textured_catalog.materials().at(71U);
        textured_material.revision = core::Revision{2U};
        textured_material.definition.base_color.asset = 501U;
        textured_material.definition.base_color.sampler = 31U;
        textured_material.definition.base_color.uv_set = 7U;
        okay(textured_catalog.replace_material(std::move(textured_material)));
        set_export_catalog(textured, std::move(textured_catalog), texture_root / "material.log");
        const auto texture_source_before = textured.doc.serialize();
        const auto texture_result = io::export_gltf(textured.doc, good);
        check(!texture_result && read(good) == before && textured.doc.serialize() == texture_source_before,
              "textured material is refused without fake pixels, source mutation or file replacement");
        check(!io::export_glb(textured.doc, good_glb) && read(good_glb) == glb_before &&
                  textured.doc.serialize() == texture_source_before,
              "textured GLB is refused without replacing the existing artifact");
        auto slotted = fixture();
        auto slot_attributes = slotted.attributes();
        okay(slot_attributes.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                                        attributes::AttributeType::uint32},
                                       {std::uint32_t{1}, std::uint32_t{2}}}));
        okay(slotted.set_attribute_payload(std::move(slot_attributes), slotted.uv_sets()));
        auto slot_root = root / "authored-slots";
        std::filesystem::create_directory(slot_root);
        const auto slot_scene = make_scene(slotted, slot_root);
        auto slot_export = io::export_gltf(slot_scene.doc, good);
        check(!slot_export && slot_export.error().message.find("material slots") != std::string::npos &&
                  read(good) == before,
              "authored slot loss is refused without replacing prior glTF");
        auto tangent_mesh = fixture();
        auto tangent_attributes = tangent_mesh.attributes();
        okay(tangent_attributes.add_layer({{"cartographer.tangent.7", attributes::AttributeDomain::corner,
                                           attributes::AttributeType::vec4},
                                          std::vector<attributes::AttributeValue>(6, core::Vec4d{1, 0, 0, 1})}));
        okay(tangent_mesh.set_attribute_payload(std::move(tangent_attributes), tangent_mesh.uv_sets()));
        auto tangent_root = root / "authored-tangent";
        std::filesystem::create_directory(tangent_root);
        const auto tangent_scene = make_scene(tangent_mesh, tangent_root);
        auto tangent_export = io::export_glb(tangent_scene.doc, good_glb);
        check(!tangent_export && tangent_export.error().message.find("tangent") != std::string::npos &&
                  read(good_glb) == glb_before,
              "authored tangent replacement is refused without replacing prior GLB");
        auto color_mesh = fixture();
        auto color_attributes = color_mesh.attributes();
        okay(color_attributes.add_layer({{"color.corner", attributes::AttributeDomain::corner,
                                         attributes::AttributeType::color4},
                                        std::vector<attributes::AttributeValue>(6, core::Vec4d{.2, .4, .6, 1})}));
        okay(color_mesh.set_attribute_payload(std::move(color_attributes), color_mesh.uv_sets()));
        auto color_root = root / "authored-color";
        std::filesystem::create_directory(color_root);
        const auto color_scene = make_scene(color_mesh, color_root);
        auto color_export = io::export_gltf(color_scene.doc, good);
        check(!color_export && color_export.error().message.find("color") != std::string::npos &&
                  read(good) == before,
              "authored color loss is refused without replacing prior glTF");
        auto multiple_uv = fixture();
        auto uv_attributes = multiple_uv.attributes();
        auto uv_sets = multiple_uv.uv_sets();
        okay(uv_attributes.add_layer({{"uv.extra", attributes::AttributeDomain::corner,
                                      attributes::AttributeType::vec2},
                                     {core::Vec2d{0, 0}, core::Vec2d{1, 0}, core::Vec2d{1, 1},
                                      core::Vec2d{0, 0}, core::Vec2d{1, 1}, core::Vec2d{0, 1}}}));
        okay(uv_sets.add({8, "Additional UV", "uv.extra", false, false, false, 1}));
        okay(multiple_uv.set_attribute_payload(std::move(uv_attributes), std::move(uv_sets)));
        auto uv_root = root / "multiple-uv";
        std::filesystem::create_directory(uv_root);
        const auto uv_scene = make_scene(multiple_uv, uv_root);
        auto uv_export = io::export_glb(uv_scene.doc, good_glb);
        check(!uv_export && uv_export.error().message.find("UV sets") != std::string::npos &&
                  read(good_glb) == glb_before,
              "additional UV stream loss is refused without replacing prior GLB");
        check(bool(io::export_gltf(reopened_scene.doc, good)) &&
                  bool(io::export_glb(reopened_scene.doc, good_glb)),
              "valid source can export after a rejected attempt");
        std::cout << "CPU export geometry: authored corner normals with/without UV, seam "
                     "splitting, tangent handedness, glTF/GLB payloads, normalized/mirrored TRS, "
                     "mesh command undo/redo, supported attribute transfer and project reopen "
                     "passed. Materials and GPU/native visual acceptance remain open. Artifacts: "
                  << root << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
