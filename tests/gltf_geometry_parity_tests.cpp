#include <bit>
#include <carto/core/json.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/io/gltf_export.hpp>
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
Scene make_scene(const geometry::EditableMesh &mesh, const std::filesystem::path &root) {
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
        tx.create_object("Mirrored nonuniform object",
                         {{1, 2, 3}, {0, .5176380902050415, 0, 1.9318516525781366}, {-2, 3, .5}}));
    okay(tx.attach_mesh(object, id));
    static_cast<void>(take(tx.commit()));
    return {std::move(doc), id};
}
void near(double a, double b, const char *message) {
    check(std::isfinite(a) && std::abs(a - b) < 2e-6, message);
}
Payload exported(const Scene &s, const std::filesystem::path &root, std::string name) {
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
    near(std::stod(std::string(scale[0])), -2, "mirrored scale preserved");
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
} // namespace
int main() {
    try {
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
        const auto root =
            std::filesystem::temp_directory_path() /
            ("gltf-geometry-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        auto mesh = fixture();
        auto s = make_scene(mesh, root);
        auto p = exported(s, root, "baseline");
        normals_and_frames(p, true);
        auto saved = root / "source.carto";
        static_cast<void>(take(s.doc.save_atomic(saved)));
        Scene reopened_scene{take(project::ProjectDocument::load(saved)), s.mesh};
        auto reopened = exported(reopened_scene, root, "reopened");
        check(reopened.bytes == p.bytes, "save/reopen keeps full geometry payload");
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
