#include "gltf_test_payload.hpp"
#include "scene_import_test_support.hpp"
#include <carto/io/generated_mesh.hpp>
#include <carto/production/authoring_material.hpp>
#include <carto/production/authoring_material_bake.hpp>
#include <carto/io/gltf_material_import.hpp>
#include <carto/io/gltf_scene_import.hpp>
#include <limits>
#include "application_access.hpp"
namespace {
static_assert(production::authoring_material_bake_fits_budget(4));
static_assert(production::authoring_material_bake_fits_budget(256));
static_assert(production::authoring_material_bake_fits_budget(512, 786432));
static_assert(production::authoring_material_bake_fits_budget(1024));
static_assert(!production::authoring_material_bake_fits_budget(0));
static_assert(!production::authoring_material_bake_fits_budget(3));
static_assert(!production::authoring_material_bake_fits_budget(257));
static_assert(!production::authoring_material_bake_fits_budget(2048));
static_assert(!production::authoring_material_bake_fits_budget(
    std::numeric_limits<std::uint32_t>::max()));
static_assert(!production::authoring_material_bake_fits_budget(1024, 1));
static_assert(!production::authoring_material_bake_fits_budget(512, 786433));
static_assert(!production::authoring_material_bake_fits_budget(
    4, std::numeric_limits<std::uint64_t>::max()));
void near(double a, double b, const char *s) {
    check(std::isfinite(a) && std::abs(a - b) < 2e-6, s);
}
double scalar(std::string_view o, std::string_view key) {
    return std::stod(std::string(member(o, key)));
}
std::vector<std::uint8_t> source_tga(unsigned width = 2, unsigned height = 2) {
    std::vector<std::uint8_t> v(18);
    v[2] = 2;
    v[12] = std::uint8_t(width);
    v[13] = std::uint8_t(width >> 8);
    v[14] = std::uint8_t(height);
    v[15] = std::uint8_t(height >> 8);
    v[16] = 32;
    v[17] = 0x28;
    for (unsigned i = 0; i < width * height; ++i)
        v.insert(v.end(), {std::uint8_t(71 + i % 109), std::uint8_t(103 + i % 67),
                           std::uint8_t(17 + i % 191), std::uint8_t(55 + i % 173)});
    return v;
}
geometry::EditableMesh mesh_fixture(bool material_regions = true) {
    geometry::EditableMesh m;
    okay(m.insert_bulk({{{1}, {0, 0, 0}}, {{2}, {1, 0, 0}}, {{3}, {1, 1, 0}}, {{4}, {0, 1, 0}}},
                       {{{1}, {{1}, {2}, {3}}}, {{2}, {{1}, {3}, {4}}}}));
    attributes::AttributeSet a(
        {{attributes::AttributeDomain::corner, 6}, {attributes::AttributeDomain::face, 2}});
    if (material_regions)
        okay(a.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                           attributes::AttributeType::uint32},
                          {std::uint32_t(1), std::uint32_t(2)}}));
    std::vector<attributes::AttributeValue> uv{core::Vec2d{0, 0}, core::Vec2d{1, 0},
                                               core::Vec2d{1, 1}, core::Vec2d{0, 0},
                                               core::Vec2d{1, 1}, core::Vec2d{0, 1}};
    okay(a.add_layer(
        {{"uv.first", attributes::AttributeDomain::corner, attributes::AttributeType::vec2}, uv}));
    if (material_regions) {
        for (auto &v : uv) {
            auto p = std::get<core::Vec2d>(v);
            v = core::Vec2d{.2 + p.x * .3, .4 + p.y * .2};
        }
        okay(a.add_layer(
            {{"uv.second", attributes::AttributeDomain::corner, attributes::AttributeType::vec2}, uv}));
    }
    attributes::UvSetTable sets;
    okay(sets.add({7, "First", "uv.first", true, true, false, 1}));
    if (material_regions)
        okay(sets.add({8, "Second", "uv.second", false, false, false, 1}));
    okay(m.set_attribute_payload(std::move(a), std::move(sets)));
    return m;
}
project::ProjectDocument material_scene(project::MaterialCatalog catalog,
                                        const std::filesystem::path &path,
                                        bool material_regions = true) {
    auto d =
        take(project::ProjectDocument::create("Independent public-safe material export fixture"));
    journal::Journal j(path.string() + ".journal");
    const auto text = d.serialize();
    static_cast<void>(
        take(j.append({{},
                       d.revision(),
                       "material.fixture.create",
                       {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()},
                       std::nullopt})));
    auto tx = take(project::ProjectTransaction::begin(d, j, "material.export.fixture"));
    const auto id = take(tx.add_mesh(mesh_fixture(material_regions)));
    check(id == 1, "fixture mesh id");
    auto object = take(tx.create_object("Two material regions"));
    okay(tx.attach_mesh(object, id));
    okay(tx.set_material_catalog(std::move(catalog)));
    static_cast<void>(take(tx.commit()));
    static_cast<void>(take(d.save_atomic(path)));
    return d;
}
project::MaterialCatalog catalog(const project::TextureSourceRecord &record) {
    project::MaterialCatalog c;
    okay(c.insert_texture(record));
    project::SamplerIntent sampler;
    sampler.id = 4;
    sampler.name = "Synthetic nearest mirror";
    sampler.address_u = project::SamplerAddressMode::mirrored_repeat;
    sampler.address_v = project::SamplerAddressMode::clamp_to_edge;
    sampler.minification = project::SamplerFilter::nearest;
    sampler.magnification = project::SamplerFilter::nearest;
    sampler.mip = project::SamplerFilter::linear;
    okay(c.insert_sampler(sampler));
    render::StandardMaterial m;
    m.base_color_factor = {.2, .4, .7, .6};
    m.metallic = .8;
    m.roughness = .27;
    m.normal_scale = .6;
    m.occlusion_strength = .3;
    m.emissive_factor = {4, 2, 1};
    m.emissive_strength = 3;
    m.dielectric_f0 = .09;
    m.clearcoat = .35;
    m.clearcoat_roughness = .18;
    m.alpha_mode = render::AlphaMode::masked;
    m.alpha_cutoff = .42;
    render::TextureRef ref;
    ref.asset = record.id;
    ref.uv_set = 7;
    ref.sampler = 4;
    m.base_color = m.metallic_roughness = m.normal = m.occlusion = m.emissive = ref;
    m.base_color.transform = {{.13, -.2}, {.7, 1.3}, .4, {.2, .8}};
    m.metallic_roughness.channels.rgba = {
        render::TextureChannel::constant_one, render::TextureChannel::one_minus_red,
        render::TextureChannel::alpha, render::TextureChannel::constant_one};
    m.occlusion.channels.rgba[0] = render::TextureChannel::blue;
    okay(c.insert_material({10, "All supported channels", m, {}}));
    project::MaterialVariant v;
    v.id = 20;
    v.name = "Second UV variant";
    v.base_material = 10;
    v.overrides.roughness = .66;
    for (auto *binding : {&v.overrides.base_color, &v.overrides.metallic_roughness,
                          &v.overrides.normal, &v.overrides.occlusion, &v.overrides.emissive}) {
        auto r = ref;
        r.uv_set = 8;
        *binding = r;
    }
    okay(c.insert_variant(v));
    okay(c.insert_assignment({{1, 1}, 10, {}}));
    okay(c.insert_assignment({{1, 2}, 10, 20}));
    return c;
}
std::vector<std::uint8_t> pixels(const Payload &p, std::string_view info,
                               unsigned expected_resolution = 0) {
    const auto texture = array(member(p.json, "textures")).at(number(member(info, "index")));
    const auto image = array(member(p.json, "images")).at(number(member(texture, "source")));
    const auto uri = take(core::json::decode_string(member(image, "uri")));
    constexpr std::string_view prefix = "data:image/png;base64,";
    check(uri.starts_with(prefix), "embedded image PNG MIME");
    auto encoded = unbase64(std::string_view(uri).substr(prefix.size()));
    auto decoded = take(assets::decode_png_rgba8(encoded, production::authoring_texture_limits()));
    if (expected_resolution)
        check(decoded.admission.width == expected_resolution &&
                  decoded.admission.height == expected_resolution,
              "baked PNG retains requested dimensions");
    return decoded.pixels;
}
void inspect(const Payload &p, const std::vector<std::uint8_t> &src) {
    auto prim = array(member(array(member(p.json, "meshes"))[0], "primitives"));
    check(prim.size() == 2, "one primitive for each actual visual material slot");
    auto mats = array(member(p.json, "materials"));
    check(mats.size() == 2, "base and variant material emitted");
    for (unsigned i = 0; i < 2; ++i) {
        check(number(member(prim[i], "material")) == i, "primitive resolves matching material");
        check(p.accessor(number(member(prim[i], "indices"))).size() == 3,
              "each triangle exported once");
        auto uv = p.accessor(number(member(member(prim[i], "attributes"), "TEXCOORD_0")));
        const auto primitive_indices=p.accessor(number(member(prim[i],"indices")));
        const auto first=static_cast<std::size_t>(primitive_indices.front())*2;
        check(first+1<uv.size(),"region index selects a valid UV element");
        near(uv[first], i == 0 ? 0 : .2, "region UV identity selected");
        near(uv[first+1], i == 0 ? 0 : .4, "region UV v selected");
        auto mat = mats[i];
        check(member(mat, "doubleSided") == "true", "viewport no-cull visibility preserved");
        check(take(core::json::decode_string(member(
                  member(mat, "extras"), "cartographer_uv_set"))) == std::to_string(i == 0 ? 7 : 8),
              "source UV identity retained");
        auto pbr = member(mat, "pbrMetallicRoughness");
        near(scalar(pbr, "roughnessFactor"), i == 0 ? .27 : .66, "variant roughness");
        near(scalar(pbr, "metallicFactor"), .8, "metallic factor");
        auto color = array(member(pbr, "baseColorFactor"));
        near(std::stod(std::string(color[0])), .2, "linear base factor");
        near(std::stod(std::string(color[3])), .6, "linear alpha factor");
        check(take(core::json::decode_string(member(mat, "alphaMode"))) == "MASK",
              "masked alpha mode");
        near(scalar(mat, "alphaCutoff"), .42, "alpha cutoff");
        near(scalar(member(mat, "normalTexture"), "scale"), .6, "normal scale");
        near(scalar(member(mat, "occlusionTexture"), "strength"), .3, "AO strength");
        auto ext = member(mat, "extensions");
        near(scalar(member(ext, "KHR_materials_ior"), "ior"), 1.3 / .7, "F0 IOR invertible");
        near(scalar(member(ext, "KHR_materials_clearcoat"), "clearcoatFactor"), .35, "clearcoat");
        near(scalar(member(ext, "KHR_materials_clearcoat"), "clearcoatRoughnessFactor"), .18,
             "clearcoat roughness");
        auto emission = array(member(mat, "emissiveFactor"));
        auto strength = scalar(member(ext, "KHR_materials_emissive_strength"), "emissiveStrength");
        near(std::stod(std::string(emission[0])) * strength, 12, "HDR emission red product");
        near(std::stod(std::string(emission[2])) * strength, 3, "HDR emission blue product");
        const auto base = pixels(p, member(pbr, "baseColorTexture"));
        check(base == src, "sRGB color sample bytes and straight alpha preserved");
        check(pixels(p, member(mat, "emissiveTexture")) == src, "emissive sRGB bytes preserved");
        auto normal = src;
        for (std::size_t k = 1; k < normal.size(); k += 4)
            normal[k] = std::uint8_t(255 - normal[k]);
        check(pixels(p, member(mat, "normalTexture")) == normal,
              "DX Y-minus normal converted to glTF Y-plus");
        auto mr = src;
        if (i == 0)
            for (std::size_t k = 0; k < mr.size(); k += 4) {
                mr[k] = 255;
                mr[k + 1] = std::uint8_t(255 - src[k]);
                mr[k + 2] = src[k + 3];
                mr[k + 3] = 255;
            }
        check(pixels(p, member(pbr, "metallicRoughnessTexture")) == mr,
              "linear MR channel projection");
        auto ao = src;
        if (i == 0)
            for (std::size_t k = 0; k < ao.size(); k += 4)
                ao[k] = src[k + 2];
        check(pixels(p, member(mat, "occlusionTexture")) == ao, "linear AO red projection");
    }
    auto sampler = array(member(p.json, "samplers"))[0];
    check(number(member(sampler, "wrapS")) == 33648 && number(member(sampler, "wrapT")) == 33071,
          "sampler address intent");
    check(number(member(sampler, "minFilter")) == 9986 &&
              number(member(sampler, "magFilter")) == 9728,
          "sampler filter intent");
    auto transform = member(
        member(member(member(mats[0], "pbrMetallicRoughness"), "baseColorTexture"), "extensions"),
        "KHR_texture_transform");
    auto offset = array(member(transform, "offset"));
    auto scale = array(member(transform, "scale"));
    auto angle = scalar(transform, "rotation");
    for (const auto uv : std::array<core::Vec2d, 3>{{{0, 0}, {.3, .8}, {1, 1}}}) {
        auto x = (uv.x - .2) * .7, y = (uv.y - .8) * 1.3;
        auto expected_x = std::cos(.4) * x - std::sin(.4) * y + .2 + .13;
        auto expected_y = std::sin(.4) * x + std::cos(.4) * y + .8 - .2;
        const auto actual_x = std::stod(std::string(offset[0])) +
                              std::cos(angle) * std::stod(std::string(scale[0])) * uv.x -
                              std::sin(angle) * std::stod(std::string(scale[1])) * uv.y;
        const auto actual_y = std::stod(std::string(offset[1])) +
                              std::sin(angle) * std::stod(std::string(scale[0])) * uv.x +
                              std::cos(angle) * std::stod(std::string(scale[1])) * uv.y;
        near(actual_x, expected_x, "pivot transform x semantics");
        near(actual_y, expected_y, "pivot transform y semantics");
    }
    auto required = array(member(p.json, "extensionsRequired"));
    check(required.size() == 4, "appearance extensions explicitly required");
    check(p.json.find("relative_locator") == std::string::npos &&
              p.json.find("source-blobs") == std::string::npos,
          "no source locator/path embedded");
}
void rejected(project::MaterialCatalog c, const std::filesystem::path &root, std::string label,
              const io::GltfExportOptions &options, std::string_view reason) {
    auto path = root / (label + ".carto");
    auto d = material_scene(std::move(c), path);
    for (auto extension : {".gltf", ".glb"}) {
        auto out = root / (label + extension);
        {
            std::ofstream f(out, std::ios::binary);
            f << "prior-good-asset";
        }
        auto result = extension == std::string_view(".gltf") ? io::export_gltf(d, out, options)
                                                             : io::export_glb(d, out, options);
        check(!result, "unsupported appearance must fail export");
        check(result.error().message.find(reason) != std::string::npos,
              "specific unsupported-feature diagnostic");
        check(read(out) == "prior-good-asset", "failed export preserves prior good asset");
    }
}
project::MaterialGraph constant_graph() {
    project::MaterialGraph g;
    g.identity = "public.synthetic.constant.material";
    using K = project::MaterialGraphNodeKind;
    using T = project::MaterialGraphValueType;
    for (unsigned i = 0; i < 4; ++i) {
        project::MaterialGraphNode n;
        n.id = i + 1;
        n.kind = i == 0 ? K::constant_color : K::constant_scalar;
        n.value_type = i == 0 ? T::color : T::scalar;
        n.constants = i == 0 ? std::array<double, 4>{.11, .22, .33, 1}
                             : std::array<double, 4>{i == 1   ? .8
                                                     : i == 2 ? .31
                                                              : .72,
                                                     0, 0, 0};
        g.nodes.push_back(n);
    }
    g.outputs.base_color = 1;
    g.outputs.alpha = 2;
    g.outputs.roughness = 3;
    g.outputs.metallic = 4;
    return g;
}
} // namespace
int main(int argc, char **argv) {
    try {
        auto root = std::filesystem::temp_directory_path() /
                    ("gltf-material-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (argc == 3 && (std::string_view(argv[1]) == "--fixture" || std::string_view(argv[1]) == "--bake-fixture"))
            root = argv[2];
        std::filesystem::create_directories(root);
        auto tga = source_tga();
        auto decoded = take(assets::decode_tga_rgba8(tga));
        project::TextureSourceRecord record{1,
                                            "Synthetic RGBA",
                                            "unused/synthetic.tga",
                                            assets::sha256(tga),
                                            assets::TextureSourceFormat::tga,
                                            2,
                                            2,
                                            "Independent nonproprietary fixture",
                                            {}};
        auto library = std::make_shared<production::AuthoringTextureLibrary>();
        okay(library->admit(record, tga));
        auto c = catalog(record);
        auto path = root / "source.carto";
        auto d = material_scene(c, path);
        auto before = d.serialize();
        io::GltfExportOptions options;
        options.textures = library;
        auto a = take(io::export_gltf(d, root / "material.gltf", options));
        auto b = take(io::export_glb(d, root / "material.glb", options));
        check(a.materials == 2 && a.textures >= 4 && a.images >= 4 && a.embedded_image_bytes > 0,
              "material/image accounting");
        check(a.warnings == b.warnings && a.embedded_image_bytes == b.embedded_image_bytes,
              "container reports agree");
        if (argc == 3 && std::string_view(argv[1]) == "--fixture") {
            assets::BlobStore store(root / ".carto-assets" / "source-blobs");
            take(store.put(tga, "image/x-tga"));
            std::cout << "material fixture ready\n";
            return 0;
        }
        Payload gltf(root / "material.gltf"), glb(root / "material.glb");
        inspect(gltf, decoded.pixels);
        inspect(glb, decoded.pixels);
        {
            auto invalid = read(root / "material.gltf");
            const auto begin = invalid.find("\"emissiveFactor\":[");
            check(begin != std::string::npos, "emissive mutation target exists");
            const auto values = begin + std::string_view("\"emissiveFactor\":[").size();
            const auto end = invalid.find(']', values);
            check(end != std::string::npos, "emissive mutation end exists");
            invalid.replace(values, end - values, "1.01,0,0");
            check(!io::import_gltf_materials({reinterpret_cast<const std::uint8_t*>(invalid.data()), invalid.size()}),
                  "out-of-range glTF emissive factors are rejected");
        }
        {
            auto invalid = read(root / "material.gltf");
            check(array(member(gltf.json, "samplers")).size() == 1U, "sampler mutation fixture has one source sampler");
            const auto start = invalid.find("\"textures\":[");
            check(start != std::string::npos, "texture mutation target exists");
            const auto end = invalid.find(']', start);
            check(end != std::string::npos, "texture mutation end exists");
            invalid.insert(end, ",{\"source\":0}");
            const auto sampler = invalid.find("\"sampler\":0", start);
            check(sampler != std::string::npos && sampler < end, "sampler mutation target exists");
            invalid.replace(sampler, std::string_view("\"sampler\":0").size(), "\"sampler\":1");
            check(!io::import_gltf_materials({reinterpret_cast<const std::uint8_t*>(invalid.data()), invalid.size()}),
                  "explicit out-of-range sampler cannot alias synthesized default sampler");
        }
        for(const auto& file:{root/"material.gltf",root/"material.glb"}) {
            const auto encoded=read(file);const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()),encoded.size());
            const auto imported=take(io::import_gltf_materials(bytes));
            const auto scene_import=take(io::import_gltf_scene(bytes));
            check(scene_import.meshes.size()==1&&scene_import.nodes.size()==1&&scene_import.materials.catalog.assignments().size()==2,"textured geometry and both material regions are actually imported");
            auto imported_pixels=std::make_shared<production::AuthoringTextureLibrary>();
            for(const auto& [id,image]:imported.catalog.textures())okay(imported_pixels->admit(image,imported.encoded_images.at(static_cast<std::size_t>(id-1))));
            check(imported.artifact_digest==assets::sha256(bytes)&&imported.catalog.materials().size()==2,"material-only import preserves artifact identity and both resolved materials");
            for(unsigned variant=0;variant<2;++variant) {
                const auto original=take(production::compile_authoring_material(c,10,variant?std::optional<project::MaterialVariantId>{20}:std::nullopt,library.get()));
                const auto restored=take(production::compile_authoring_material(imported.catalog,variant+1,std::nullopt,imported_pixels.get()));
                check(original.uv_set()==restored.uv_set(),"reimport retains explicit UV identity");
                for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
                    production::AuthoringSurfaceSample sample{{(x+.5)/8,(y+.5)/8},{1./32,0},{0,1./32}};
                    const auto reference_sample=take(original.evaluate(sample)),after=take(restored.evaluate(sample));
                    near(reference_sample.material.base_color_factor.x,after.material.base_color_factor.x,"reimport color/sampler/transform semantics");
                    near(reference_sample.material.base_color_factor.w,after.material.base_color_factor.w,"reimport alpha semantics");
                    near(reference_sample.material.metallic,after.material.metallic,"reimport metallic channel");near(reference_sample.material.roughness,after.material.roughness,"reimport roughness channel");
                    near(reference_sample.ambient_occlusion,after.ambient_occlusion,"reimport AO strength");
                    near(reference_sample.material.emissive_factor.x*reference_sample.material.emissive_strength,after.material.emissive_factor.x*after.material.emissive_strength,"reimport HDR emissive strength");
                    check((reference_sample.tangent_normal-after.tangent_normal).length()<.02,"reimport normal Y convention within one-byte filtering precision");
                }
            }
        }
        {
            application::ApplicationSession session;
            take(application::testing::dispatch(session,application::SaveProjectAction{root/"textured-scene-import.carto"}));
            auto snap=session.snapshot();
            take(carto::testing::import_gltf_scene_file(session,root/"material.glb",true));
            take(application::testing::dispatch(session,application::SaveProjectAction{std::nullopt}));
            const auto imported=take(project::ProjectDocument::load(root/"textured-scene-import.carto"));
            check(imported.meshes().size()==1&&imported.material_catalog().assignments().size()==2,"textured import save/reopen retains both material slots");
            io::GltfExportOptions stored;stored.project_path=root/"textured-scene-import.carto";
            take(io::export_glb(imported,root/"textured-scene-roundtrip.glb",stored));
            Payload restored(root/"textured-scene-roundtrip.glb");inspect(restored,decoded.pixels);
            snap=session.snapshot();const auto before_failure=take(session.authoring_viewport_source(snap.project_revision)).serialize();
            check(!carto::testing::import_gltf_scene_file(session,root/"material.glb",true),"duplicate scene import refuses rather than duplicating source identity");
            check(take(session.authoring_viewport_source(snap.project_revision)).serialize()==before_failure,"duplicate scene source leaves good imported project intact");
        }
        {
            application::ApplicationSession session;
            take(application::testing::dispatch(session,application::SaveProjectAction{root/"reimport.carto"}));
            auto snap=session.snapshot();const auto encoded=read(root/"material.glb");std::vector<std::uint8_t> bytes(encoded.begin(),encoded.end());
            check(!application::testing::dispatch(session,application::ImportGltfMaterialLibraryAction{snap.project_generation,snap.project_revision,bytes,false}),"material reuse requires explicit rights confirmation");
            take(application::testing::dispatch(session,application::ImportGltfMaterialLibraryAction{snap.project_generation,snap.project_revision,bytes,true}));
            auto imported=take(session.authoring_viewport_source(session.snapshot().project_revision));
            check(imported.material_catalog().materials().size()==2&&imported.source_asset_registry().records().size()==1,"human import commits materials and digest-bound provenance together");
            take(application::testing::dispatch(session,application::UndoAction{}));check(session.snapshot().materials.empty(),"material library import undoes atomically");
            take(application::testing::dispatch(session,application::RedoAction{}));take(application::testing::dispatch(session,application::SaveProjectAction{std::nullopt}));
            const auto reopened=take(project::ProjectDocument::load(root/"reimport.carto"));check(reopened.material_catalog().serialize()==imported.material_catalog().serialize(),"reimported materials survive save/reopen");
            const auto source_before_failure=take(session.authoring_viewport_source(session.snapshot().project_revision)).serialize();
            auto external=read(root/"material.gltf");const auto first=external.find("data:image/png;base64,");const auto end=external.find('"',first);external.replace(first,end-first,"https://invalid.example/never-fetched.png");
            snap=session.snapshot();std::vector<std::uint8_t> unsafe(external.begin(),external.end());
            check(!application::testing::dispatch(session,application::ImportGltfMaterialLibraryAction{snap.project_generation,snap.project_revision,unsafe,true}),"external texture URI refused without fetching");
            check(take(session.authoring_viewport_source(session.snapshot().project_revision)).serialize()==source_before_failure,"failed material reimport preserves prior catalog and source registry");
        }
        for (auto key : {"materials", "images", "textures", "samplers", "extensionsRequired"})
            check(member(gltf.json, key) == member(glb.json, key),
                  "complete material payload GLB/glTF parity");
        // This is the bounded outward appearance profile, not a new authoring
        // importer. The existing botanical-only importer must reject it rather than
        // drop appearance.
        for (auto ext : {".gltf", ".glb"}) {
            auto file = read(root / (std::string("material") + ext));
            auto bytes =
                std::span(reinterpret_cast<const std::uint8_t *>(file.data()), file.size());
            auto imported =
                ext == std::string_view(".gltf")
                    ? io::import_generated_botanical_gltf(bytes, "unsupported.export.profile", {},
                                                          assets::sha256(bytes))
                    : io::import_generated_botanical_glb(bytes, "unsupported.export.profile", {},
                                                         assets::sha256(bytes));
            check(!imported, "outward material profile rejected by geometry-only "
                             "botanical importer");
        }
        check(d.serialize() == before, "export does not mutate authored material truth");
        auto reopened = take(project::ProjectDocument::load(path));
        check(reopened.serialize() == before, "saved material source round-trip exact");
        take(io::export_gltf(reopened, root / "reopened.gltf", options));
        check(read(root / "reopened.gltf") == read(root / "material.gltf"),
              "reopened material export deterministic");
        assets::BlobStore store(root / ".carto-assets" / "source-blobs");
        take(store.put(tga, "image/x-tga"));
        io::GltfExportOptions stored{path, {}};
        take(io::export_gltf(reopened, root / "store.gltf", stored));
        check(read(root / "store.gltf") == read(root / "material.gltf"),
              "digest-addressed reopen pixel hydration");
        auto changed = [&](auto mutate) {
            auto out = c;
            auto m = out.materials().at(10);
            mutate(m.definition);
            m.revision = m.revision.next();
            okay(out.replace_material(m));
            return out;
        };
        rejected(c, root, "missing-pixels", {}, "pixels unavailable");
        auto stale = record;
        stale.revision = stale.revision.next();
        rejected(catalog(stale), root, "stale-decoded-pixels", options, "stale");
        rejected(changed([](auto &m) { m.base_color_factor.x = 2; }), root, "hdr-base", options,
                 "HDR base-color");
        rejected(changed([](auto &m) { m.dielectric_f0 = 1; }), root, "singular-ior", options,
                 "finite IOR");
        rejected(changed([](auto &m) {
                     m.base_color.interpretation = render::TextureInterpretation::linear;
                 }),
                 root, "linear-color", options, "color-space override");
        rejected(changed([](auto &m) {
                     m.occlusion.interpretation = render::TextureInterpretation::srgb;
                 }),
                 root, "srgb-data", options, "color-space override");
        rejected(changed([](auto &m) {
                     m.base_color.channels.rgba[0] = render::TextureChannel::one_minus_red;
                 }),
                 root, "nonlinear-swizzle", options, "sRGB filtering");
        rejected(changed([](auto &m) { m.normal.transform.rotation = .2; }), root, "rotated-normal",
                 options, "tangent-frame bake");
        rejected(changed([](auto &m) { m.normal.channels.rgba[0] = render::TextureChannel::blue; }),
                 root, "normal-swizzle", options, "tangent-space bake");
        rejected(changed([](auto &m) { m.emissive.uv_set = 8; }), root, "mixed-uv", options,
                 "single UV identity");
        rejected(changed([](auto &m) {
                     for (auto *r : {&m.base_color, &m.metallic_roughness, &m.normal, &m.occlusion,
                                     &m.emissive})
                         r->uv_set = 99;
                 }),
                 root, "missing-uv", options, "UV");
        auto alternate = changed([](auto &m) {
            m.base_color.channels.rgba = {render::TextureChannel::blue, render::TextureChannel::red,
                                          render::TextureChannel::constant_one,
                                          render::TextureChannel::one_minus_alpha};
            m.normal.normal_convention = render::NormalConvention::opengl_y_plus;
            m.alpha_mode = render::AlphaMode::blended;
        });
        auto alternate_doc = material_scene(alternate, root / "alternate.carto");
        const auto alternate_report =
            take(io::export_gltf(alternate_doc, root / "alternate.gltf", options));
        Payload ap(root / "alternate.gltf");
        auto am = array(member(ap.json, "materials"))[0];
        auto expected = decoded.pixels;
        for (std::size_t k = 0; k < expected.size(); k += 4) {
            expected[k] = decoded.pixels[k + 2];
            expected[k + 1] = decoded.pixels[k];
            expected[k + 2] = 255;
            expected[k + 3] = std::uint8_t(255 - decoded.pixels[k + 3]);
        }
        check(pixels(ap, member(member(am, "pbrMetallicRoughness"), "baseColorTexture")) ==
                  expected,
              "sRGB permutation/constants and linear alpha inversion preserve bytes");
        check(pixels(ap, member(am, "normalTexture")) == decoded.pixels, "OpenGL normals retain Y");
        check(take(core::json::decode_string(member(am, "alphaMode"))) == "BLEND",
              "authored BLEND exported");
        check(std::any_of(alternate_report.warnings.begin(), alternate_report.warnings.end(),
                          [](const auto &w) { return w.find("BLEND") != w.npos; }),
              "unsupported native blended appearance diagnosed");
        // Re-admit the exported PNG as source and compare the full derived artifact.
        auto image = array(member(gltf.json, "images"))[0];
        auto uri = take(core::json::decode_string(member(image, "uri")));
        auto pngbytes = unbase64(
            std::string_view(uri).substr(std::string_view("data:image/png;base64,").size()));
        auto pngrecord = record;
        pngrecord.format = assets::TextureSourceFormat::png;
        pngrecord.content_digest = assets::sha256(pngbytes);
        auto pnglibrary = std::make_shared<production::AuthoringTextureLibrary>();
        okay(pnglibrary->admit(pngrecord, pngbytes));
        auto pngdoc = material_scene(catalog(pngrecord), root / "png-source.carto");
        io::GltfExportOptions pngoptions;
        pngoptions.textures = pnglibrary;
        take(io::export_gltf(pngdoc, root / "png-source.gltf", pngoptions));
        Payload pp(root / "png-source.gltf");
        inspect(pp, decoded.pixels);
        // A default material is valid when no authored regions or omitted UV streams exist.
        auto defaultdoc = material_scene({}, root / "default.carto", false);
        const auto default_report = take(io::export_gltf(defaultdoc, root / "default.gltf"));
        Payload dp(root / "default.gltf");
        const auto default_root = take(core::json::parse_object(dp.json));
        const auto default_material = take(core::json::parse_object(array(member(dp.json, "materials"))[0]));
        check(!core::json::find_member(default_root, "extensionsRequired") &&
              !core::json::find_member(default_material, "extensions"),
              "visually neutral material extensions are omitted from the default profile");
        check(std::any_of(default_report.warnings.begin(), default_report.warnings.end(),
                          [](const auto& warning) { return warning.find("Inactive clearcoat roughness") != std::string::npos; }),
              "inactive source parameters omitted from default appearance are reported");
        auto defaultpbr = member(array(member(dp.json, "materials"))[0], "pbrMetallicRoughness");
        near(scalar(defaultpbr, "metallicFactor"), 0,
             "unassigned material retains Cartographer default metallic");
        near(scalar(defaultpbr, "roughnessFactor"), .5,
             "unassigned material retains Cartographer default roughness");
        auto g = constant_graph();
        auto constant = c;
        okay(constant.insert_material_graph(g));
        okay(constant.bind_material_graph(10, g.identity));
        auto graphdoc = material_scene(constant, root / "constant.carto");
        take(io::export_gltf(graphdoc, root / "constant.gltf", options));
        Payload gp(root / "constant.gltf");
        auto graphpbr = member(array(member(gp.json, "materials"))[0], "pbrMetallicRoughness");
        near(scalar(graphpbr, "roughnessFactor"), .31, "constant graph executes roughness output");
        near(std::stod(std::string(array(member(graphpbr, "baseColorFactor"))[0])), .11,
             "constant graph executes color output");
        auto dynamic = g;
        dynamic.nodes[2].kind = project::MaterialGraphNodeKind::parameter_scalar;
        dynamic.nodes[2].parameter_name = "uv.u";
        dynamic.nodes[2].constants = {};
        auto dynamiccat = c;
        okay(dynamiccat.insert_material_graph(dynamic));
        okay(dynamiccat.bind_material_graph(10, dynamic.identity));
        rejected(dynamiccat, root, "dynamic-graph", options, "baking");
        auto baked_options=options;baked_options.graph_bake_resolution=16;
        auto baked_doc=material_scene(dynamiccat,root/"baked.carto");
        const auto source_before_bake=baked_doc.serialize();
        auto baked_report=take(io::export_gltf(baked_doc,root/"baked.gltf",baked_options));
        take(io::export_glb(baked_doc,root/"baked.glb",baked_options));
        check(baked_doc.serialize()==source_before_bake,"graph bake is derived and leaves authoring source intact");
        Payload baked_payload(root/"baked.gltf"),baked_glb(root/"baked.glb");
        for(auto key:{"materials","images","textures","samplers"})
            check(member(baked_payload.json,key)==member(baked_glb.json,key),"baked GLTF/GLB payload agreement");
        check(std::any_of(baked_report.warnings.begin(),baked_report.warnings.end(),[](const auto& w){return w.find("finite-resolution")!=std::string::npos;}),"lossy bake is explicit in export receipt");
        check(std::any_of(baked_report.warnings.begin(),baked_report.warnings.end(),[](const auto& w){return w.find("cannot reimport")!=std::string::npos;}),"baked output import limitation is explicit in export receipt");
        {
            const auto data = read(root/"baked.gltf");
            const auto imported = io::import_gltf_materials({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
            check(!imported, "baked clearcoat and non-mip sampler are rejected instead of silently changed on import");
        }
        const auto linear=[](std::uint8_t v){const double s=double(v)/255;return s<=.04045?s/12.92:std::pow((s+.055)/1.055,2.4);};
        for(unsigned v=0;v<2;++v) {
            auto program=take(production::compile_authoring_material(dynamiccat,10,v?std::optional<project::MaterialVariantId>{20}:std::nullopt,library.get()));
            const auto mat=array(member(baked_payload.json,"materials"))[v];
            const auto pbr=member(mat,"pbrMetallicRoughness");
            const auto color=pixels(baked_payload,member(pbr,"baseColorTexture"));
            const auto mr=pixels(baked_payload,member(pbr,"metallicRoughnessTexture"));
            const auto normal=pixels(baked_payload,member(mat,"normalTexture"));
            const auto ao=pixels(baked_payload,member(mat,"occlusionTexture"));
            const auto emission=pixels(baked_payload,member(mat,"emissiveTexture"));
            const auto coat=pixels(baked_payload,member(member(member(mat,"extensions"),"KHR_materials_clearcoat"),"clearcoatTexture"));
            const auto material_extensions = take(core::json::parse_object(member(mat,"extensions")));
            const auto* strength_extension = core::json::find_member(material_extensions,"KHR_materials_emissive_strength");
            const auto strength = strength_extension ? scalar(strength_extension->raw_value,"emissiveStrength") : 1.0;
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
                const auto sample=take(program.evaluate({{(x+.5)/16,(y+.5)/16},{1./16,0},{0,1./16}}));const auto at=(y*16+x)*4;
                check(std::abs(linear(color[at])-sample.material.base_color_factor.x)<.005,"baked sRGB color agrees with executable graph");
                check(std::abs(double(color[at+3])/255-sample.material.base_color_factor.w)<.002,"baked alpha remains linear");
                check(std::abs(double(mr[at+1])/255-sample.material.roughness)<.002,"UV-varying roughness survives export");
                check(std::abs(double(mr[at+2])/255-sample.material.metallic)<.002,"baked metallic channel placement");
                check(std::abs(double(normal[at+1])/255*2-1-sample.tangent_normal.y)<.004,"baked normal handedness agrees with executable normal");
                check(std::abs(double(ao[at])/255-sample.ambient_occlusion)<.002,"baked AO strength applied once");
                check(std::abs(linear(emission[at])*strength-sample.material.emissive_factor.x*sample.material.emissive_strength)<strength*.005,"baked HDR emission factor applied once");
                check(std::abs(double(coat[at])/255-sample.material.clearcoat)<.002,"baked clearcoat retained");
            }
        }
        auto invalid_bake=options;invalid_bake.graph_bake_resolution=3;
        rejected(dynamiccat,root,"invalid-bake-budget",invalid_bake,"4..1024");
        invalid_bake.graph_bake_resolution=2048;
        rejected(dynamiccat,root,"oversized-bake-budget",invalid_bake,"4..1024");
        // Both regions share one projection. This exercises the maximum bake
        // without charging duplicate uses against the aggregate texel budget.
        auto single_bake_cat=dynamiccat;
        okay(single_bake_cat.replace_assignment({{1,2},10,std::nullopt}));
        auto high_doc=material_scene(single_bake_cat,root/"high-resolution.carto");
        const auto high_source=high_doc.serialize();
        auto high_options=options;
        high_options.graph_bake_resolution=512;
        const auto high_path=root/"high-resolution.gltf";
        take(io::export_gltf(high_doc,high_path,high_options));
        {
            const auto first=read(high_path);
            take(io::export_gltf(high_doc,high_path,high_options));
            check(read(high_path)==first,"512 graph baking is byte deterministic");
            Payload high_payload(high_path);
            const auto mat=array(member(high_payload.json,"materials"))[0];
            const auto mr=pixels(high_payload,
                member(member(mat,"pbrMetallicRoughness"),"metallicRoughnessTexture"),512);
            check(mr.size()==std::size_t(512)*512*4,"512 bake pixel count");
        }
        high_options.graph_bake_resolution=1024;
        const auto high_report=take(io::export_gltf(high_doc,high_path,high_options));
        check(high_report.materials==1,"shared regions reuse the same 1024 graph bake");
        check(std::count_if(high_report.warnings.begin(),high_report.warnings.end(),
                  [](const auto& w){return w.starts_with("Explicit material graph bake:");})==1,
              "one receipt for one unique material bake");
        check(high_report.embedded_image_bytes<=64ULL*1024*1024,
              "higher-resolution graph bake retains embedded-image budget");
        {
            Payload high_payload(high_path);
            const auto mat=array(member(high_payload.json,"materials"))[0];
            const auto mr=pixels(high_payload,
                member(member(mat,"pbrMetallicRoughness"),"metallicRoughnessTexture"),1024);
            check(mr.size()==std::size_t(1024)*1024*4,"1024 bake pixel count");
            auto program=take(production::compile_authoring_material(
                single_bake_cat,10,std::nullopt,library.get()));
            check(!production::bake_authoring_material(program,2048),
                  "direct production baker rejects excessive resolution");
            for(const auto point:std::array<std::array<unsigned,2>,5>{{
                    {0,0},{511,511},{512,512},{1023,1023},{137,701}}}) {
                const auto x=point[0],y=point[1];
                const auto sample=take(program.evaluate(
                    {{(x+.5)/1024,(y+.5)/1024},{1./1024,0},{0,1./1024}}));
                const auto at=(std::size_t(y)*1024+x)*4;
                check(std::abs(double(mr[at+1])/255-sample.material.roughness)<.002,
                      "1024 roughness samples retain executable graph semantics");
                check(std::abs(double(mr[at+2])/255-sample.material.metallic)<.002,
                      "1024 metallic samples retain channel placement");
            }
        }
        check(high_doc.serialize()==high_source,"higher-resolution baking preserves source");
        // A second unique variant cannot consume another 1024 tile. Keep the
        // successful high-resolution artifact when the aggregate budget fails.
        const auto good_high_artifact=read(high_path);
        const auto overflow=io::export_gltf(baked_doc,high_path,high_options);
        check(!overflow&&overflow.error().message.find("total texels")!=std::string::npos,
              "1024 material variants respect aggregate texel budget");
        check(read(high_path)==good_high_artifact,
              "aggregate budget failure preserves the prior good high-resolution export");
        check(baked_doc.serialize()==source_before_bake,
              "aggregate budget failure preserves source document");
        if(argc==3&&std::string_view(argv[1])=="--bake-fixture")return 0;
        auto badbytes = tga;
        badbytes.back() ^= 1;
        okay(library->admit(record, tga));
        check(!library->admit(record, badbytes), "corrupt replacement rejected");
        take(io::export_gltf(d, root / "after-failed-admission.gltf", options));
        check(read(root / "after-failed-admission.gltf") == read(root / "material.gltf"),
              "prior admitted pixels preserved after failed replacement");
        {
            std::ofstream f(store.path_for(record.content_digest),
                            std::ios::binary | std::ios::trunc);
            f << "corrupt";
        }
        auto failed = io::export_gltf(d, root / "store.gltf", stored);
        check(!failed, "corrupt stored blob rejected");
        check(read(root / "store.gltf") == read(root / "material.gltf"),
              "corrupt blob leaves good export unchanged");
        // Exercise multi-block stored DEFLATE with a nonuniform image and
        // independent system PNG decoder.
        auto large = source_tga(129, 129);
        auto lr = record;
        lr.content_digest = assets::sha256(large);
        lr.width = lr.height = 129;
        auto ll = std::make_shared<production::AuthoringTextureLibrary>();
        okay(ll->admit(lr, large));
        auto ld = material_scene(catalog(lr), root / "large.carto");
        io::GltfExportOptions lo;
        lo.textures = ll;
        take(io::export_gltf(ld, root / "large.gltf", lo));
        Payload lp(root / "large.gltf");
        inspect(lp, take(assets::decode_tga_rgba8(large)).pixels);
        std::cout << "PASS material channels, regions/variants/UV, embedded "
                     "PNG/system decode, transforms/samplers, constant graphs, "
                     "reopen and failure-atomic diagnostics\nartifacts="
                  << root.string() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
