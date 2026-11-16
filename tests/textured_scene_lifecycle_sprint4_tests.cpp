// Synthetic regression-only fixture. No production or real-world asset is represented.
#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "application_access.hpp"
#undef private
#if defined(__clang__)
#    pragma clang diagnostic pop
#endif
#include <carto/assets/blob_store.hpp>
#include <carto/assets/texture_image_decoder.hpp>
#include <carto/editor/command_bus.hpp>
#include <carto/journal/journal.hpp>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <utility>
#include <vector>

namespace {
using namespace carto;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
void chunk(std::vector<std::uint8_t>& bytes, std::string_view name, std::span<const std::uint8_t> data) {
    be32(bytes, static_cast<std::uint32_t>(data.size()));
    const auto start = bytes.size();
    bytes.insert(bytes.end(), name.begin(), name.end());
    bytes.insert(bytes.end(), data.begin(), data.end());
    be32(bytes, crc32(std::span<const std::uint8_t>{bytes}.subspan(start)));
}
std::vector<std::uint8_t> png() {
    std::vector<std::uint8_t> bytes{0x89U,'P','N','G',0x0dU,0x0aU,0x1aU,0x0aU};
    std::array<std::uint8_t, 13U> ihdr{};
    ihdr[3]=1U; ihdr[7]=1U; ihdr[8]=8U; ihdr[9]=6U;
    chunk(bytes, "IHDR", ihdr);
    constexpr std::array<std::uint8_t, 16U> idat{0x78U,0x01U,0x01U,0x05U,0x00U,0xfaU,0xffU,0x00U,
        0xffU,0x00U,0x00U,0xffU,0x05U,0x00U,0x01U,0xffU};
    chunk(bytes, "IDAT", idat); chunk(bytes, "IEND", {});
    return bytes;
}
std::vector<std::uint8_t> jpeg() {
    std::vector<std::uint8_t> bytes{0xffU,0xd8U,0xffU,0xdbU,0U,67U,0U};
    bytes.insert(bytes.end(), 64U, 1U);
    bytes.insert(bytes.end(), {0xffU,0xc0U,0U,11U,8U,0U,1U,0U,1U,1U,1U,0x11U,0U,
        0xffU,0xc4U,0U,38U,
        0U,1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,
        0x10U,1U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,0U,
        0xffU,0xdaU,0U,8U,1U,1U,0U,0U,63U,0U,0x3fU,0xffU,0xd9U});
    return bytes;
}
struct Fixture { std::vector<std::uint8_t> png, jpeg, glb; };
Fixture fixture(std::string_view name, bool external_image = false, bool external_buffer = false) {
    Fixture f{png(), jpeg(), {}};
    std::vector<std::uint8_t> bin;
    for (float v : {0.F,0.F,0.F, 1.F,0.F,0.F, 0.F,1.F,0.F,
                    0.F,1.F,0.F, 0.F,1.F,0.F, 0.F,1.F,0.F,
                    0.F,0.F, 1.F,0.F, 0.F,1.F}) u32(bin, std::bit_cast<std::uint32_t>(v));
    const auto png_offset = bin.size();
    bin.insert(bin.end(), f.png.begin(), f.png.end());
    while (bin.size() % 4U) bin.push_back(0U);
    const auto jpeg_offset = bin.size();
    bin.insert(bin.end(), f.jpeg.begin(), f.jpeg.end());
    while (bin.size() % 4U) bin.push_back(0U);
    const std::string first_image = external_image ? R"({"uri":"outside.png"})" :
        R"({"bufferView":3,"mimeType":"image/png"})";
    std::string json = std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":")") + std::string(name) +
        R"(","mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"material":0}]}],)"
        R"("materials":[{"name":"synthetic","doubleSided":true,"pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,1],"metallicFactor":0.125,"roughnessFactor":0.625,"baseColorTexture":{"index":0}},"normalTexture":{"index":1,"scale":0.5}}],)"
        R"("samplers":[{"wrapS":33648,"wrapT":33071,"minFilter":9984,"magFilter":9728},{"wrapS":10497,"wrapT":33071,"minFilter":9987,"magFilter":9729}],)"
        R"("textures":[{"source":0,"sampler":0},{"source":1,"sampler":1}],"images":[)" + first_image +
        R"(,{"bufferView":4,"mimeType":"image/jpeg"}],"buffers":[{"byteLength":)" + std::to_string(bin.size()) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":)" + std::to_string(png_offset) +
        R"(,"byteLength":)" + std::to_string(f.png.size()) + R"(},{"buffer":0,"byteOffset":)" + std::to_string(jpeg_offset) +
        R"(,"byteLength":)" + std::to_string(f.jpeg.size()) +
        R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"}]})";
    if (external_buffer) {
        const std::string marker = "\"buffers\":[{\"byteLength\":";
        const auto pos = json.find(marker);
        require(pos != std::string::npos, "buffer fixture marker");
        json.replace(pos, marker.size(), "\"buffers\":[{\"uri\":\"outside.bin\",\"byteLength\":");
    }
    while (json.size() % 4U) json.push_back(' ');
    u32(f.glb,0x46546c67U); u32(f.glb,2U);
    u32(f.glb,static_cast<std::uint32_t>(12U+8U+json.size()+8U+bin.size()));
    u32(f.glb,static_cast<std::uint32_t>(json.size())); u32(f.glb,0x4e4f534aU);
    f.glb.insert(f.glb.end(),json.begin(),json.end());
    u32(f.glb,static_cast<std::uint32_t>(bin.size())); u32(f.glb,0x004e4942U);
    f.glb.insert(f.glb.end(),bin.begin(),bin.end());
    return f;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "journal file exists");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
using application::ApplicationAction;
auto human(application::ApplicationSession& s, const ApplicationAction& a) {
    return application::HumanApplicationAccess::dispatch(s,a);
}
application::ImportGltfSceneAction action_for(application::ApplicationSession& s,
        std::string identity, core::Revision generation, assets::Sha256Digest digest) {
    const auto snap=s.snapshot();
    return {snap.project_generation,snap.project_revision,snap.source_namespace,std::move(identity),
        generation,digest,true,"Synthetic fixture rights confirmed"};
}
void same_state(application::ApplicationSession& s, const std::string& document,
                std::size_t undo, std::size_t redo, const std::string& journal,
                const std::filesystem::path& journal_path) {
    const auto snap=s.snapshot();
    require(s.document_.serialize()==document && snap.undo_count==undo && snap.redo_count==redo &&
            bytes(journal_path)==journal, "refusal preserves document, history and journal bytes");
}
void verify_scene(application::ApplicationSession& s, const Fixture& f, core::Revision generation,
                  const assets::BlobStore& store) {
    using K=assets::SourceDocumentProductKind;
    const auto snapshot=s.snapshot();
    const auto source_bytes=store.read(assets::sha256(f.glb));
    require(source_bytes && source_bytes.value()==f.glb, "exact glTF source bytes retained");
    require(snapshot.objects.size()==1U && snapshot.source_assets.size()==1U &&
            snapshot.source_assets[0].source_generation==generation &&
            snapshot.source_assets[0].source_digest==assets::sha256(f.glb), "exact source and generation retained");
    const std::vector<assets::SourceDocumentProduct> products{{K::mesh,1U,1U},{K::node,1U,1U},
        {K::material,1U,1U},{K::texture,1U,1U},{K::texture,2U,2U},{K::sampler,1U,1U},{K::sampler,2U,2U}};
    require(snapshot.source_assets[0].document_products==products &&
            snapshot.source_assets[0].artifact_digest==assets::source_document_product_digest(assets::sha256(f.glb),products),
        "typed source product IDs and digest are exact");
    const auto& doc=s.document_;
    const auto* node=doc.scene().find(scene::ObjectId{1U});
    require(node && node->mesh_asset==1U, "source node binds expected mesh");
    const auto& mesh=doc.meshes().at(1U);
    require(mesh.vertex_count()==3U && mesh.face_count()==1U, "source geometry retained");
    const auto* normals=mesh.attributes().find("cartographer.normal0");
    const auto* uv=mesh.attributes().find("gltf.uv.1");
    const auto* slots=mesh.attributes().find("cartographer.material_slot");
    require(normals && uv && slots && normals->values.size()==3U && uv->values.size()==3U && slots->values.size()==1U,
        "normal, UV and material-slot payloads exist");
    const std::array<core::Vec2d,3U> expected_uv{{{0,0},{1,0},{0,1}}};
    for (std::size_t i=0;i<3U;++i) {
        const auto n=std::get<core::Vec3d>(normals->values[i]);
        const auto t=std::get<core::Vec2d>(uv->values[i]);
        require(n.x==0.0 && n.y==1.0 && n.z==0.0 &&
                t.x==expected_uv[i].x && t.y==expected_uv[i].y, "authored normals and UV coordinates retained");
    }
    require(std::get<std::uint32_t>(slots->values[0])==1U && mesh.uv_sets().find(1U),
        "material slot and UV identity retained");
    const auto& catalog=doc.material_catalog();
    const auto& mat=catalog.materials().at(1U).definition;
    require(mat.base_color_factor.x==0.25 && mat.base_color_factor.y==0.5 &&
            mat.base_color_factor.z==0.75 && mat.metallic==0.125 && mat.roughness==0.625 &&
            mat.base_color.asset==1U && mat.base_color.sampler==1U && mat.base_color.uv_set==1U &&
            mat.normal.asset==2U && mat.normal.sampler==2U && mat.normal.uv_set==1U && mat.normal_scale==0.5 &&
            mat.normal.normal_convention==render::NormalConvention::opengl_y_plus,
        "authored material and texture/sampler/UV bindings retained");
    require(catalog.assignments().at(project::MaterialRegionKey{1U,1U}).material==1U,
        "mesh region maps to source material");
    const auto& samplers=catalog.samplers();
    require(samplers.at(1U).address_u==project::SamplerAddressMode::mirrored_repeat &&
            samplers.at(1U).address_v==project::SamplerAddressMode::clamp_to_edge &&
            samplers.at(1U).minification==project::SamplerFilter::nearest &&
            samplers.at(1U).magnification==project::SamplerFilter::nearest &&
            samplers.at(2U).address_u==project::SamplerAddressMode::repeat &&
            samplers.at(2U).mip==project::SamplerFilter::linear, "sampler intent retained");
    for (const auto& pair : {std::pair<std::uint64_t,const std::vector<std::uint8_t>*>{1U,&f.png}, {2U,&f.jpeg}}) {
        const auto& texture=catalog.textures().at(pair.first);
        const auto expected=assets::sha256(*pair.second);
        const auto read=store.read(expected);
        const auto expected_format=pair.first==1U ? assets::TextureSourceFormat::png : assets::TextureSourceFormat::jpeg;
        const auto project_root=store.root().parent_path().parent_path();
        require(texture.content_digest==expected && texture.width==1U && texture.height==1U &&
                texture.format==expected_format &&
                texture.relative_locator==store.path_for(expected).lexically_relative(project_root).generic_string() &&
                read && read.value()==*pair.second, "staged texture bytes, format, locator and digest preserved");
        const auto decoded=assets::decode_texture_image_rgba8(read.value(),expected,1U,1U);
        const std::vector<std::uint8_t> pixel=pair.first==1U ? std::vector<std::uint8_t>{255U,0U,0U,255U}
                                                              : std::vector<std::uint8_t>{128U,128U,128U,255U};
        require(decoded && decoded.value().pixels==pixel, "fixture image decodes to exact authored pixel");
    }
}

void lifecycle(const std::filesystem::path& root) {
    const auto project_path=root/"textured-regression.carto";
    const auto journal_path=std::filesystem::path(project_path.string()+".journal");
    application::ApplicationSession s;
    require(static_cast<bool>(human(s,application::NewProjectAction{"Synthetic textured fixture"})), "new project");
    require(static_cast<bool>(human(s,application::SaveProjectAction{project_path})), "save establishes journal");
    assets::BlobStore store(root/".carto-assets"/"source-blobs");
    const Fixture f=fixture("synthetic-regression");
    const auto source=store.put(f.glb,"model/gltf-binary");
    const auto color=store.put(f.png,"image/png");
    require(source && color, "stage exact source and PNG");
    auto action=action_for(s,"synthetic-scene",core::Revision{},source.value().digest);
    auto before=s.document_.serialize(); auto snap=s.snapshot(); auto journal=bytes(journal_path);
    require(!human(s,action), "missing staged JPEG refuses");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    const auto normal=store.put(f.jpeg,"image/jpeg");
    require(static_cast<bool>(normal), "stage exact JPEG");

    auto ai=s.dispatch(application::HumanActionAdmission{}, action,
        application::OperationSource::ai_proposal,"synthetic-adversarial-proposal");
    require(!ai && ai.error().code==core::ErrorCode::unsupported, "AI source cannot authorize import");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    const Fixture external=fixture("external-resource",true);
    const auto external_source=store.put(external.glb,"model/gltf-binary");
    require(static_cast<bool>(external_source), "stage external-resource fixture source");
    auto external_action=action_for(s,"external-scene",core::Revision{},external_source.value().digest);
    require(!human(s,external_action), "external image URI refuses without fetching");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    const Fixture external_bin=fixture("external-buffer",false,true);
    const auto external_bin_source=store.put(external_bin.glb,"model/gltf-binary");
    require(static_cast<bool>(external_bin_source), "stage external-buffer source");
    auto external_bin_action=action_for(s,"external-buffer",core::Revision{},external_bin_source.value().digest);
    require(!human(s,external_bin_action), "external buffer URI refuses without fetching");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    auto no_rights=action; no_rights.rights_confirmed=false;
    require(!human(s,no_rights), "unconfirmed rights refuse");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    auto no_justification=action; no_justification.justification=" ";
    require(!human(s,no_justification), "missing justification refuses");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);

    require(static_cast<bool>(human(s,action)), "human textured scene import");
    verify_scene(s,f,core::Revision{1U},store);

    // Adversarial internal fixture: the public adapter allocates fresh IDs, so
    // construct the real command with forged source-product claims to reach
    // its post-insertion registry conflict and rollback branch.
    const Fixture other=fixture("different-source");
    const auto other_blob=store.put(other.glb,"model/gltf-binary");
    require(other_blob && other_blob.value().digest!=source.value().digest, "second exact source staged");
    auto forged=s.document_.source_asset_registry().records().at("synthetic-scene");
    forged.identity="collision-scene";
    forged.relative_source_path=store.path_for(other_blob.value().digest).lexically_relative(root).generic_string();
    forged.source_digest=other_blob.value().digest;
    forged.artifact_identity="gltf.scene.collision-scene";
    forged.artifact_digest=assets::source_document_product_digest(forged.source_digest,forged.document_products);
    require(static_cast<bool>(forged.validate()), "forged typed mapping is structurally valid");
    std::vector<std::pair<std::uint64_t,geometry::EditableMesh>> staged_meshes;
    staged_meshes.emplace_back(900001U,s.document_.meshes().at(1U));
    scene::SceneObject staged_node;
    staged_node.id=scene::ObjectId{900001U};
    staged_node.name="adversarial-staged-node";
    staged_node.mesh_asset=900001U;
    before=s.document_.serialize(); snap=s.snapshot(); journal=bytes(journal_path);
    const auto conflict=s.execute_command(std::make_unique<editor::ImportGltfSceneCommand>(
        editor::ProjectCommandAdmission{},s.document_,std::move(staged_meshes),
        std::vector<scene::SceneObject>{staged_node},s.document_.material_catalog(),forged,store.root()),
        "Adversarial typed product collision");
    require(!conflict && conflict.error().message.find("conflicting document products")!=std::string::npos,
        "late typed product collision reaches real command refusal");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    require(!s.document_.meshes().contains(900001U) && !s.document_.scene().find(scene::ObjectId{900001U}),
        "late command rollback removes its staged mesh and node");
    verify_scene(s,f,core::Revision{1U},store);

    require(static_cast<bool>(human(s,application::UndoAction{})), "undo import");
    snap=s.snapshot();
    require(snap.objects.empty() && snap.source_assets.empty() && snap.source_asset_tombstone_count==1U,
        "undo removes products and retains source tombstone");
    const auto generation=s.source_asset_generation("synthetic-scene",snap.source_namespace,
        snap.project_generation,snap.project_revision);
    require(generation && generation.value()==core::Revision{1U}, "undo retains generation one");
    require(static_cast<bool>(human(s,application::SaveProjectAction{})), "save tombstoned source");
    application::ApplicationSession cold_undone;
    require(static_cast<bool>(human(cold_undone,application::OpenProjectAction{project_path})), "cold reopen tombstone");
    const auto cold_before=cold_undone.snapshot();
    const auto cold_generation=cold_undone.source_asset_generation("synthetic-scene",cold_before.source_namespace,
        cold_before.project_generation,cold_before.project_revision);
    require(cold_before.source_assets.empty() && cold_before.source_asset_tombstone_count==1U &&
            cold_generation && cold_generation.value()==core::Revision{1U}, "tombstone generation survives reopen");
    auto stale=action_for(s,"synthetic-scene",core::Revision{},source.value().digest);
    before=s.document_.serialize(); snap=s.snapshot(); journal=bytes(journal_path);
    require(!human(s,stale), "stale tombstone generation cannot be reused");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    const auto image_path=store.path_for(normal.value().digest);
    const auto parked=root/"parked-fixture-jpeg";
    std::filesystem::rename(image_path,parked);
    require(!human(s,application::RedoAction{}), "redo refuses missing staged image");
    same_state(s,before,snap.undo_count,snap.redo_count,journal,journal_path);
    std::filesystem::rename(parked,image_path);
    require(static_cast<bool>(human(s,application::RedoAction{})), "redo import");
    verify_scene(s,f,core::Revision{2U},store);
    require(static_cast<bool>(human(s,application::SaveProjectAction{})), "save restored import");
    application::ApplicationSession cold;
    require(static_cast<bool>(human(cold,application::OpenProjectAction{project_path})), "cold reopen textured import");
    verify_scene(cold,f,core::Revision{2U},store);
    journal::Journal verified(journal_path);
    require(static_cast<bool>(verified.verify()), "journal chain verifies");
}
} // namespace
int main() {
#ifndef _WIN32
    std::cout << "SKIP: textured glTF fixture needs the supported Windows image decoder\n";
    return 0;
#else
    auto root=std::filesystem::temp_directory_path()/
        ("carto-textured-lifecycle-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    // Native source blobs include SHA-256 fanout; sandbox temp roots can make
    // those paths exceed MAX_PATH even though import itself uses long paths.
    if (root.native().rfind(L"\\\\?\\", 0) != 0)
        root = std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(root).native());
    try {
        std::filesystem::create_directories(root);
        lifecycle(root);
        std::filesystem::remove_all(root);
        std::cout << "textured scene lifecycle regression passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove_all(root,ignored);
        return 1;
    }
#endif
}
