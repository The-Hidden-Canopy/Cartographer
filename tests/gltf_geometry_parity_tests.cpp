#include "gltf_test_payload.hpp"
#include <algorithm>
#include <array>
#include <locale>
#include <limits>
#include <carto/io/gltf_material_import.hpp>
#include <carto/io/gltf_scene_import.hpp>
#include "../apps/desktop/interchange_export_feedback.hpp"

namespace {
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
    const auto extras = member(node, "extras");
    check(member(extras, "vanta_object_id") == member(extras, "cartographer_object_id") &&
          member(extras, "vanta_visible") == member(extras, "visible") &&
          member(extras, "vanta_locked") == member(extras, "locked"),
          "VANTA scene metadata matches Cartographer identity and flags");
    const auto binary_extras = member(array(member(binary.json, "nodes"))[0], "extras");
    check(binary_extras == extras, "GLB and glTF preserve the same scene metadata");
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
void vanta_metadata_export(const std::filesystem::path& root) {
    auto doc = take(project::ProjectDocument::create("VANTA metadata"));
    journal::Journal journal(root / "vanta-metadata.log");
    if (doc.revision() != core::Revision{}) {
        const auto checkpoint = doc.serialize();
        static_cast<void>(take(journal.append(
            {{}, doc.revision(), "export.fixture.create",
             {reinterpret_cast<const std::uint8_t*>(checkpoint.data()), checkpoint.size()}, std::nullopt})));
    }
    auto tx = take(project::ProjectTransaction::begin(doc, journal, "export.vanta.metadata"));
    const auto mesh = take(tx.add_mesh(fixture()));
    scene::SceneObject node;
    node.id = scene::ObjectId{42};
    node.name = "Hidden locked object";
    node.mesh_asset = mesh;
    node.visible = false;
    node.locked = true;
    okay(tx.insert_object(node));
    static_cast<void>(take(tx.commit()));
    for (const auto* extension : {".gltf", ".glb"}) {
        const auto path = root / (std::string("vanta-metadata") + extension);
        if (std::string_view(extension) == ".glb")
            static_cast<void>(take(io::export_glb(doc, path)));
        else
            static_cast<void>(take(io::export_gltf(doc, path)));
        const Payload payload(path);
        const auto extras = member(array(member(payload.json, "nodes"))[0], "extras");
        check(number(member(extras, "vanta_object_id")) == 42U &&
              member(extras, "vanta_visible") == "false" &&
              member(extras, "vanta_locked") == "true",
              "VANTA metadata preserves a hidden locked object and source identity");
    }
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
    check(take(core::json::decode_string(member(material, "name"))) == "Copper \"fixture\"",
          "material export preserves and escapes its authored name");
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

    // An extended catalog can contain an unrelated material that this glTF
    // profile cannot represent. The used material retains its source identity
    // without exporting a fabricated placeholder or failing on unused data.
    const auto extended_root = root / "unused-unsupported-material";
    std::filesystem::create_directory(extended_root);
    auto extended = make_scene(material_mesh(), extended_root);
    auto extended_catalog = scalar_catalog(extended.mesh);
    render::MaterialAsset unsupported;
    unsupported.id = 4U; unsupported.name = "Unused HDR material";
    unsupported.revision = core::Revision{1U};
    unsupported.definition.base_color_factor.x = 2.0;
    okay(extended_catalog.insert_material(std::move(unsupported)));
    set_export_catalog(extended, std::move(extended_catalog), extended_root / "material.log");
    const auto mapped = exported(extended, extended_root, "mapped");
    const auto mapped_materials = array(member(mapped.json, "materials"));
    const auto mapped_index = number(member(mapped.primitive(), "material"));
    check(mapped_materials.size() == 1U && mapped_index == 0U &&
              number(member(member(mapped_materials.at(mapped_index), "extras"),
                            "cartographer_material_id")) == 71U,
          "unused unsupported catalog entry is omitted and primitive retains source material identity");
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

using UvTriangle = std::array<core::Vec2d, 3U>;

geometry::EditableMesh uv_projection_fixture(const UvTriangle& coordinates, bool authored_frames) {
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk(
        {{{1}, {0, 0, 0}}, {{2}, {1, 0, 0}}, {{3}, {0, 1, 0}}},
        {{{1}, {{1}, {2}, {3}}}}));
    attributes::AttributeSet payload({{attributes::AttributeDomain::corner, 3U}});
    std::vector<attributes::AttributeValue> values;
    for (const auto coordinate : coordinates) values.emplace_back(coordinate);
    okay(payload.add_layer({{"uv.export", attributes::AttributeDomain::corner,
        attributes::AttributeType::vec2}, values}));
    if (authored_frames) {
        okay(payload.add_layer({{"cartographer.tangent.7", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec4},
            std::vector<attributes::AttributeValue>(3U, core::Vec4d{1.0, 0.0, 0.0, 1.0})}));
        okay(payload.add_layer({{"cartographer.tangent_uv.7", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec2}, values}));
    }
    attributes::UvSetTable sets;
    okay(sets.add({7U, "Render UV", "uv.export", true, true, false, 1U}));
    okay(mesh.set_attribute_payload(std::move(payload), std::move(sets)));
    return mesh;
}

void gltf_float32_uv_projection_preserves_fidelity(const std::filesystem::path& root) {
    const UvTriangle unit_uvs{{{0, 0}, {1, 0}, {0, 1}}};
    okay(production::validate_uv_triangle_projection(unit_uvs, unit_uvs));
    auto nonfinite = unit_uvs;
    nonfinite[0].x = std::numeric_limits<double>::quiet_NaN();
    check(!production::validate_uv_triangle_projection(nonfinite, unit_uvs) &&
          !production::validate_uv_triangle_projection(unit_uvs, nonfinite),
        "shared UV projection gate refuses non-finite source or projected coordinates");
    const double maximum = std::numeric_limits<double>::max();
    const UvTriangle overflowing_edges{{{-maximum, 0}, {maximum, 0}, {0, 1}}};
    check(!production::validate_uv_triangle_projection(overflowing_edges, unit_uvs),
        "shared UV projection gate refuses non-finite edge differences");

    const auto baseline_root = root / "baseline";
    std::filesystem::create_directory(baseline_root);
    const auto baseline = make_scene(uv_projection_fixture(unit_uvs, false), baseline_root);
    const auto good_gltf = root / "baseline.gltf";
    const auto good_glb = root / "baseline.glb";
    static_cast<void>(take(io::export_gltf(baseline.doc, good_gltf)));
    static_cast<void>(take(io::export_glb(baseline.doc, good_glb)));
    const auto gltf_before = read(good_gltf);
    const auto glb_before = read(good_glb);
    const double unit = std::ldexp(1.0, -23);
    struct Case { std::string name; UvTriangle coordinates; };
    const std::vector<Case> rejected{
        {"collapsed", {{{1e8, 0}, {1e8 + 1, 0}, {1e8, 1}}}},
        {"reversed", {{{0, 0}, {1 + 0.6 * unit, 1 + 0.4 * unit},
            {2 + 0.98 * unit, 2 - 0.98 * unit}}}},
        {"underflow", {{{0, 0}, {1e-170, 0}, {0, 1e-170}}}},
        {"anisotropic", {{{0, 0}, {1e-10, 1e-320}, {2e-10, 1e-320}}}},
        {"subtraction-loss", {{{1, 1}, {1e-170, 0}, {0, 1e-170}}}},
        {"invented-area", {{{0, 0}, {1 + unit, 1 + 2 * unit},
            {3 + 3 * unit, 3 + 6 * unit}}}},
    };
    for (const auto& item : rejected) {
        for (const bool authored_frames : {false, true}) {
            const auto case_root = root / (item.name + (authored_frames ? "-authored" : "-derived"));
            std::filesystem::create_directory(case_root);
            const auto scene = make_scene(
                uv_projection_fixture(item.coordinates, authored_frames), case_root);
            const auto before_source = scene.doc.serialize();
            const auto gltf = io::export_gltf(scene.doc, good_gltf);
            const auto glb = io::export_glb(scene.doc, good_glb);
            check(!gltf && !glb &&
                  gltf.error().message.find("glTF float32 UV narrowing") != std::string::npos &&
                  glb.error().message.find("glTF float32 UV narrowing") != std::string::npos,
                "both formats reject UV fidelity loss with or without authored tangent frames");
            check(read(good_gltf) == gltf_before && read(good_glb) == glb_before &&
                  scene.doc.serialize() == before_source,
                "refused UV narrowing preserves both prior artifacts and the complete source document");
            const auto new_gltf = case_root / "refused.gltf";
            const auto new_glb = case_root / "refused.glb";
            check(!io::export_gltf(scene.doc, new_gltf) && !std::filesystem::exists(new_gltf) &&
                  !io::export_glb(scene.doc, new_glb) && !std::filesystem::exists(new_glb),
                "refused UV projection never publishes a new partial artifact");
        }
    }
    const std::vector<Case> accepted{
        {"ordinary", unit_uvs},
        {"mirrored", {{{0, 0}, {-1, 0}, {0, 1}}}},
        {"tiny-valid", {{{0, 0}, {1e-8, 0}, {0, 1e-8}}}},
        {"constant", {{{0.25, 0.25}, {0.25, 0.25}, {0.25, 0.25}}}},
    };
    for (const auto& item : accepted) {
        for (const bool authored_frames : {false, true}) {
            const auto case_root = root / (item.name + (authored_frames ? "-authored" : "-derived"));
            std::filesystem::create_directory(case_root);
            const auto scene = make_scene(
                uv_projection_fixture(item.coordinates, authored_frames), case_root);
            const auto payload = exported(scene, case_root, "preserved");
            const auto coordinates = payload.attribute("TEXCOORD_0");
            check(coordinates.size() == 6U, "accepted projection retains all three UV corners");
            for (std::size_t i = 0U; i < 3U; ++i) {
                check(coordinates[i * 2U] == static_cast<float>(item.coordinates[i].x) &&
                      coordinates[i * 2U + 1U] == static_cast<float>(item.coordinates[i].y),
                    "accepted export preserves the exact requested float32 UV projection");
            }
        }
    }
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
        const auto uv_projection_root = root / "uv-float32-projection";
        std::filesystem::create_directory(uv_projection_root);
        gltf_float32_uv_projection_preserves_fidelity(uv_projection_root);
        vanta_metadata_export(root);
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
        auto source_root = root / "source-aware";
        std::filesystem::create_directory(source_root);
        auto sourced = make_scene(fixture(), source_root);
        const auto without_source = take(io::export_gltf(sourced.doc, source_root / "plain.gltf"));
        check(std::none_of(without_source.warnings.begin(), without_source.warnings.end(),
                           [](const auto& warning) {
                               return warning.find("reimport links are not included") != std::string::npos;
                           }),
              "source handoff warning is omitted when no imported sources are registered");
        const std::string source_bytes = "private source fixture";
        const auto source_digest = assets::sha256({
            reinterpret_cast<const std::uint8_t*>(source_bytes.data()), source_bytes.size()});
        assets::SourceImportRecord record;
        record.identity = "source.fixture";
        record.relative_source_path = "private/original.glb";
        record.source_generation = core::Revision{1U};
        record.kind = assets::SourceAssetKind::mesh;
        record.source_digest = source_digest;
        record.importer_identity = "carto.import.gltf";
        record.artifact_identity = "mesh.fixture";
        record.artifact_digest = source_digest;
        auto plan = take(assets::plan_source_reimport(sourced.doc.source_asset_registry(), {record}));
        journal::Journal source_journal(source_root / "create.log");
        auto source_tx = take(project::ProjectTransaction::begin(sourced.doc, source_journal,
                                                                  "source.fixture"));
        okay(source_tx.apply_source_reimport_plan(plan));
        static_cast<void>(take(source_tx.commit("source.fixture.import")));
        const auto sourced_gltf = take(io::export_gltf(sourced.doc, source_root / "source.gltf"));
        const auto sourced_glb = take(io::export_glb(sourced.doc, source_root / "source.glb"));
        const auto warns_source_handoff = [](const auto& report) {
            return std::any_of(report.warnings.begin(), report.warnings.end(), [](const auto& warning) {
                return warning.find("reimport links are not included") != std::string::npos &&
                       warning.find("original source files") != std::string::npos &&
                       warning.find("private/original.glb") == std::string::npos;
            });
        };
        check(warns_source_handoff(sourced_gltf) && warns_source_handoff(sourced_glb) &&
                  read(source_root / "source.gltf").find("private/original.glb") == std::string::npos &&
                  read(source_root / "source.glb").find("private/original.glb") == std::string::npos &&
                  read(source_root / "source.gltf").find(source_bytes) == std::string::npos &&
                  read(source_root / "source.glb").find(source_bytes) == std::string::npos,
              "source-aware glTF and GLB warn that originals and reimport links are absent");
        const auto source_feedback = desktop::interchange_export_feedback(sourced_glb);
        check(source_feedback.find("reimport links are not included") != std::string::npos &&
                  source_feedback.find("private/original.glb") == std::string::npos,
              "actual source-handoff warning reaches desktop feedback without source locators");
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
            check(std::any_of(imported.warnings.begin(), imported.warnings.end(), [](const auto& warning) {
                      return warning.find("source face IDs and material-region identity") != std::string::npos;
                  }), "import explicitly reports source region and face identity loss");
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
        auto preserved_tangent_mesh = fixture(true, true);
        auto preserved_attributes = preserved_tangent_mesh.attributes();
        const auto preserved_uv = *preserved_attributes.find("uv.export");
        okay(preserved_attributes.add_layer({{"cartographer.tangent.7", attributes::AttributeDomain::corner,
                                             attributes::AttributeType::vec4},
                                            std::vector<attributes::AttributeValue>(6, core::Vec4d{1, 0, 0, 1})}));
        okay(preserved_attributes.add_layer({{"cartographer.tangent_uv.7", attributes::AttributeDomain::corner,
                                             attributes::AttributeType::vec2}, preserved_uv.values}));
        okay(preserved_tangent_mesh.set_attribute_payload(
            std::move(preserved_attributes), preserved_tangent_mesh.uv_sets()));
        const auto preserved_root = root / "preserved-authored-tangents";
        std::filesystem::create_directory(preserved_root);
        auto preserved_scene = make_scene(preserved_tangent_mesh, preserved_root);
        const auto preserved_source = preserved_scene.doc.serialize();
        const auto preserved_path = preserved_root / "source.carto";
        static_cast<void>(take(preserved_scene.doc.save_atomic(preserved_path)));
        const auto preserved_reopened = take(project::ProjectDocument::load(preserved_path));
        for (const auto* extension : {".gltf", ".glb"}) {
            const auto path = preserved_root / (std::string("tangent") + extension);
            if (std::string_view(extension) == ".glb")
                static_cast<void>(take(io::export_glb(preserved_reopened, path)));
            else
                static_cast<void>(take(io::export_gltf(preserved_reopened, path)));
            const auto tangents = Payload(path).attribute("TANGENT");
            check(!tangents.empty() && tangents.size() % 4U == 0U,
                "validated authored tangent pairs remain exportable after reopen");
            for (std::size_t i = 0; i < tangents.size(); i += 4U) {
                near(tangents[i], 1.0, "authored tangent x retained");
                near(tangents[i + 1U], 0.0, "authored tangent y retained");
                near(tangents[i + 2U], 0.0, "authored tangent z retained");
                near(tangents[i + 3U], 1.0, "authored tangent handedness retained");
            }
        }
        check(preserved_scene.doc.serialize() == preserved_source &&
                  preserved_reopened.serialize() == preserved_source,
              "tangent export does not mutate authored attributes or reopened source");
        auto stale_tangent_mesh = fixture(true, true);
        auto stale_attributes = stale_tangent_mesh.attributes();
        auto uv_layer = *stale_attributes.find("uv.export");
        okay(stale_attributes.add_layer({{"cartographer.tangent.7", attributes::AttributeDomain::corner,
                                         attributes::AttributeType::vec4},
                                        std::vector<attributes::AttributeValue>(6, core::Vec4d{1, 0, 0, 1})}));
        okay(stale_attributes.add_layer({{"cartographer.tangent_uv.7", attributes::AttributeDomain::corner,
                                         attributes::AttributeType::vec2}, uv_layer.values}));
        uv_layer.values.front() = core::Vec2d{.125, 0};
        okay(stale_attributes.remove_layer("uv.export"));
        okay(stale_attributes.add_layer(std::move(uv_layer)));
        okay(stale_tangent_mesh.set_attribute_payload(std::move(stale_attributes), stale_tangent_mesh.uv_sets()));
        auto stale_tangent_root = root / "stale-authored-tangent";
        std::filesystem::create_directory(stale_tangent_root);
        const auto stale_tangent_scene = make_scene(stale_tangent_mesh, stale_tangent_root);
        const auto stale_tangent_export = io::export_glb(stale_tangent_scene.doc, good_glb);
        check(!stale_tangent_export &&
                  stale_tangent_export.error().message.find("tangent UV correspondence changed") != std::string::npos &&
                  read(good_glb) == glb_before,
              "changed UVs refuse stale authored tangents without replacing prior GLB");
        auto invalid_frame_mesh = fixture(true, true);
        auto invalid_frame_attributes = invalid_frame_mesh.attributes();
        const auto current_uv = *invalid_frame_attributes.find("uv.export");
        okay(invalid_frame_attributes.add_layer({{"cartographer.tangent.7", attributes::AttributeDomain::corner,
                                                 attributes::AttributeType::vec4},
                                                std::vector<attributes::AttributeValue>(6, core::Vec4d{0, 0, 1, 1})}));
        okay(invalid_frame_attributes.add_layer({{"cartographer.tangent_uv.7", attributes::AttributeDomain::corner,
                                                 attributes::AttributeType::vec2}, current_uv.values}));
        okay(invalid_frame_mesh.set_attribute_payload(std::move(invalid_frame_attributes), invalid_frame_mesh.uv_sets()));
        auto invalid_frame_root = root / "invalid-tangent-frame";
        std::filesystem::create_directory(invalid_frame_root);
        const auto invalid_frame_scene = make_scene(invalid_frame_mesh, invalid_frame_root);
        const auto invalid_frame_export = io::export_glb(invalid_frame_scene.doc, good_glb);
        check(!invalid_frame_export &&
                  invalid_frame_export.error().message.find("tangent") != std::string::npos &&
                  read(good_glb) == glb_before,
              "non-orthogonal authored tangents refuse without replacing prior GLB");
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
        auto custom_mesh = fixture();
        auto custom_attributes = custom_mesh.attributes();
        okay(custom_attributes.add_layer({{"author.roughness", attributes::AttributeDomain::face,
                                          attributes::AttributeType::float32},
                                         {0.2F, 0.8F}}));
        okay(custom_mesh.set_attribute_payload(std::move(custom_attributes), custom_mesh.uv_sets()));
        auto custom_root = root / "authored-custom-layer";
        std::filesystem::create_directory(custom_root);
        const auto custom_scene = make_scene(custom_mesh, custom_root);
        const auto custom_source = custom_scene.doc.serialize();
        const auto custom_gltf = io::export_gltf(custom_scene.doc, good);
        const auto custom_glb = io::export_glb(custom_scene.doc, good_glb);
        check(!custom_gltf && !custom_glb &&
                  custom_gltf.error().message.find("author.roughness") != std::string::npos &&
                  custom_glb.error().message.find("author.roughness") != std::string::npos &&
                  custom_gltf.error().message.find(std::to_string(custom_scene.mesh)) != std::string::npos &&
                  read(good) == before && read(good_glb) == glb_before &&
                  custom_scene.doc.serialize() == custom_source,
              "unsupported custom attribute loss is diagnosed before glTF or GLB replacement");
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
                     "passed. Additional material data is checked by its separate suite; GPU/native visual acceptance remains open. Artifacts: "
                  << root << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
