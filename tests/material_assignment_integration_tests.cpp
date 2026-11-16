#include "application_access.hpp"
#include "gltf_test_payload.hpp"
#include "../apps/desktop/studio_material_draft.hpp"
#include <carto/production/authoring_viewport.hpp>
#include <carto/production/material_binding.hpp>
namespace {
using namespace carto;
void send(application::ApplicationSession& s,const application::ApplicationAction& a){take(application::testing::dispatch(s,a));}
geometry::EditableMesh quad(bool legacy=false) {
    geometry::EditableMesh m;okay(m.insert_bulk({{{1},{0,0,0}},{{2},{1,0,0}},{{3},{1,1,0}},{{4},{0,1,0}}},
        {{{1},{{1},{2},{3}}},{{2},{{1},{3},{4}}}}));
    if(legacy) {
        attributes::AttributeSet attrs({{attributes::AttributeDomain::face,2}});
        okay(attrs.add_layer({{"condition.material_region",attributes::AttributeDomain::face,attributes::AttributeType::uint32},{std::uint32_t(7),std::uint32_t(7)}}));
        okay(m.set_attribute_payload(std::move(attrs),{}));
    }return m;
}
production::AuthoringViewportImage image(application::ApplicationSession& s) {
    const auto snap=s.snapshot();auto d=take(s.authoring_viewport_source(snap.project_revision));
    production::AuthoringViewportRequest r;r.width=r.height=64;r.view.position={.5,.5,3};r.view.rotation={0,0,0,1};r.view.orthographic_height=1.5;
    return take(production::render_authoring_viewport(d,r));
}
void colors(const production::AuthoringViewportImage& p,bool green,bool blue=false) {
    unsigned red_count=0,green_count=0,blue_count=0;
    for(std::size_t i=0;i<p.objects.size();++i)if(p.objects[i]) {
        const auto c=p.linear_hdr[i];
        if(std::abs(c.x-2)<1e-6&&c.y==0&&c.z==0)++red_count;
        else if(std::abs(c.y-3)<1e-6&&c.x==0&&c.z==0)++green_count;
        else if(std::abs(c.z-4)<1e-6&&c.x==0&&c.y==0)++blue_count;
        else check(false,"every covered pixel uses authored material rather than a default");
    }
    if(blue)check(blue_count>100&&red_count==0&&green_count==0,"legacy assigned material reaches visible frame");
    else check(red_count>100&&(green?green_count>100:green_count==0),"whole/face assignments reach correct visible regions");
}
}
int main(){try {
    const auto root=std::filesystem::temp_directory_path()/("material-binding-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
    application::ApplicationSession s;auto path=root/"source.carto";send(s,application::SaveProjectAction{path});
    send(s,application::CreateMeshObjectAction{"Unassigned primitive",quad()});
    auto snap=s.snapshot();const auto object=snap.objects.front().object.id;const auto mesh=*snap.objects.front().object.mesh_asset;
    render::StandardMaterial red;red.emissive_factor={1,0,0};red.emissive_strength=2;
    send(s,application::CreateMaterialAction{snap.project_revision,"Red",red});
    snap=s.snapshot();const auto redid=snap.materials.back().id,redrev=snap.materials.back().revision.value();
    auto green=red;green.emissive_factor={0,1,0};green.emissive_strength=3;
    send(s,application::CreateMaterialAction{snap.project_revision,"Green",green});
    snap=s.snapshot();const auto greenid=snap.materials.back().id;const auto greenrev=snap.materials.back().revision;
    {
        using studio::MaterialDraftIdentity;
        using studio::MaterialDraftState;
        const auto first=studio::find_material_source(snap.materials,redid);
        const auto selected=studio::find_material_source(snap.materials,greenid);
        check(first!=snap.materials.end() && selected!=snap.materials.end() &&
              first->revision==selected->revision && first->id!=selected->id,
              "fixture has distinct material sources at the same revision");
        const MaterialDraftIdentity draft{snap.project_generation,first->id,first->revision};
        const MaterialDraftIdentity current{snap.project_generation,selected->id,selected->revision};
        check(selected->definition.emissive_factor.y==1 &&
              studio::material_draft_state(true,false,draft,current)==MaterialDraftState::reload,
              "new selection resolves the new source and reloads an equal-revision clean draft");
        check(studio::material_draft_state(true,true,draft,current)==MaterialDraftState::stale,
              "dirty draft cannot be applied to a different equal-revision material");
        const MaterialDraftIdentity other_project{snap.project_generation+1,first->id,first->revision};
        check(studio::material_draft_state(true,false,draft,other_project)==MaterialDraftState::reload &&
              studio::material_draft_state(true,true,draft,other_project)==MaterialDraftState::stale,
              "project generation change reloads a clean draft but retains a dirty one as stale");
        const MaterialDraftIdentity newer{draft.project_generation,draft.material,draft.revision.next()};
        check(studio::material_draft_state(true,true,draft,newer)==MaterialDraftState::stale &&
              studio::material_draft_state(true,false,draft,newer)==MaterialDraftState::reload &&
              studio::material_draft_state(true,true,draft,draft)==MaterialDraftState::current &&
              studio::material_draft_state(false,true,draft,current)==MaterialDraftState::stale,
              "revision changes and missing loaded state fail closed for dirty drafts");
        check(studio::material_source_widget_id(1U,1U) !=
                  studio::material_source_widget_id(1U,1ULL + (1ULL << 31U)) &&
              studio::material_source_widget_id(1U,1U) !=
                  studio::material_source_widget_id(2U,1U) &&
              studio::material_source_widget_id(12U,3U) !=
                  studio::material_source_widget_id(1U,23U),
              "material widget scopes retain full source identity and project generation");
        // All standard slots use the same non-destructive source selector.
        render::StandardMaterial maps;
        for (auto* slot : {&maps.metallic_roughness, &maps.normal, &maps.occlusion, &maps.emissive}) {
            slot->asset = 4; slot->uv_set = 7; slot->sampler = 3;
            slot->transform.offset = {.2, .3};
            auto preserved = *slot; preserved.asset = 5;
            studio::set_material_texture_source(*slot, 5);
            check(*slot == preserved, "slot replacement retains UV, sampler and mapping");
            studio::set_material_texture_source(*slot, 0);
            check(*slot == render::TextureRef{}, "slot clear resets its complete binding");
        }
        render::TextureRef binding;
        binding.asset=10;binding.uv_set=7;binding.sampler=3;
        binding.transform.offset={.2,.3};binding.transform.scale={2,3};
        binding.transform.rotation=.5;binding.transform.pivot={.1,.9};
        binding.channels.rgba[0]=render::TextureChannel::one_minus_green;
        binding.interpretation=render::TextureInterpretation::linear;
        binding.normal_convention=render::NormalConvention::opengl_y_plus;
        auto expected=binding;expected.asset=11;
        studio::set_base_color_texture_source(binding,11);
        check(binding==expected,"replacing base-color texture preserves all authored binding metadata");
        studio::set_base_color_texture_source(binding,0);
        check(binding==render::TextureRef{},"clearing base-color texture restores an unbound default");
        studio::set_base_color_texture_source(binding,12);
        render::TextureRef rebound;rebound.asset=12;
        check(binding==rebound,"binding from None uses default sampling metadata");
    }
    // The same edit path used by the desktop persists all five slots through
    // revisioned update, undo/redo, and a saved project reopen.
    {
        application::ApplicationSession maps_session;
        const auto maps_path = root / "maps.carto";
        send(maps_session, application::SaveProjectAction{maps_path});
        const std::vector<std::uint8_t> png = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,6,0,0,0,31,21,196,137,0,0,0,13,73,68,65,84,120,156,99,248,207,192,240,31,0,5,0,1,255,137,153,61,29,0,0,0,0,73,69,78,68,174,66,96,130};
        assets::BlobStore blobs(root / ".carto-assets" / "source-blobs");
        const auto stored = take(blobs.put(png, "image/png"));
        auto state = maps_session.snapshot();
        send(maps_session, application::ImportMaterialTextureSourceAction{
            state.project_revision, stored.digest, assets::TextureSourceFormat::png, 1U, 1U});
        state = maps_session.snapshot();
        send(maps_session, application::CreateMaterialAction{state.project_revision, "Maps", {}});
        state = maps_session.snapshot();
        auto edited = state.materials.front().definition;
        const auto texture = state.texture_sources.front().id;
        for (auto* slot : {&edited.base_color, &edited.metallic_roughness, &edited.normal,
                           &edited.occlusion, &edited.emissive})
            studio::set_material_texture_source(*slot, texture);
        edited.normal.normal_convention = render::NormalConvention::opengl_y_plus;
        edited.normal_scale = .7; edited.occlusion_strength = .6;
        send(maps_session, application::UpdateMaterialAction{state.project_revision,
            state.materials.front().id, state.materials.front().revision, edited});
        send(maps_session, application::UndoAction{});
        check(!maps_session.snapshot().materials.front().definition.normal,
              "undo removes new map bindings");
        send(maps_session, application::RedoAction{});
        send(maps_session, application::SaveProjectAction{std::nullopt});
        const auto reopened_maps = take(project::ProjectDocument::load(maps_path));
        const auto& saved = reopened_maps.material_catalog().materials().begin()->second.definition;
        check(saved.base_color.asset == texture && saved.metallic_roughness.asset == texture &&
              saved.normal.asset == texture && saved.occlusion.asset == texture &&
              saved.emissive.asset == texture && saved.normal.normal_convention ==
                  render::NormalConvention::opengl_y_plus && saved.normal_scale == .7 &&
              saved.occlusion_strength == .6, "all map bindings survive undo/redo and reopen");
        // Put the bindings on a UV-mapped mesh and exercise the real glTF
        // handoff from the saved project-owned source blobs.
        auto mapped_mesh = quad();
        attributes::AttributeSet mapped_attributes({{attributes::AttributeDomain::corner, 6U}});
        okay(mapped_attributes.add_layer({{"uv0.corners", attributes::AttributeDomain::corner,
            attributes::AttributeType::vec2}, {core::Vec2d{0,0}, core::Vec2d{1,0}, core::Vec2d{1,1},
            core::Vec2d{0,0}, core::Vec2d{1,1}, core::Vec2d{0,1}}}));
        attributes::UvSetTable mapped_uvs;
        okay(mapped_uvs.add({render::kUv0, "UV0", "uv0.corners", true, true, false, 1U}));
        okay(mapped_mesh.set_attribute_payload(std::move(mapped_attributes), std::move(mapped_uvs)));
        state = maps_session.snapshot();
        send(maps_session, application::CreateMeshObjectAction{"Mapped quad", std::move(mapped_mesh)});
        state = maps_session.snapshot();
        const auto mapped_object = state.objects.front().object.id;
        send(maps_session, application::AssignMaterialToMeshAction{state.project_generation,
            state.project_revision, mapped_object, state.materials.front().id,
            state.materials.front().revision, {}, {}});
        send(maps_session, application::SaveProjectAction{std::nullopt});
        const auto export_source = take(project::ProjectDocument::load(maps_path));
        take(io::export_glb(export_source, root / "maps.glb", io::GltfExportOptions{maps_path}));
        Payload maps_payload(root / "maps.glb");
        const auto material = array(member(maps_payload.json, "materials"))[0];
        const auto pbr = member(material, "pbrMetallicRoughness");
        check(!member(pbr, "baseColorTexture").empty() && !member(pbr, "metallicRoughnessTexture").empty() &&
              !member(material, "normalTexture").empty() && !member(material, "occlusionTexture").empty() &&
              !member(material, "emissiveTexture").empty(), "all five map slots reach exported GLB");
    }
    send(s,application::AssignMaterialToMeshAction{snap.project_generation,snap.project_revision,object,redid,core::Revision{redrev},{},{}});colors(image(s),false);
    snap=s.snapshot();auto whole=take(s.authoring_viewport_source(snap.project_revision));check(whole.material_catalog().assignments().size()==1,"new primitive gains one authored slot/binding");
    send(s,application::AssignMaterialToMeshAction{snap.project_generation,snap.project_revision,object,greenid,greenrev,{{1}},{}});colors(image(s),true);
    snap=s.snapshot();auto partial=take(s.authoring_viewport_source(snap.project_revision));check(partial.material_catalog().assignments().size()==2,"partial face assignment preserves other material region");
    const auto before=partial.serialize();
    check(!application::testing::dispatch(s,application::AssignMaterialToMeshAction{snap.project_generation,snap.project_revision,object,redid,core::Revision{redrev},{{999}},{}}),"invalid face rejected");
    check(!application::testing::dispatch(s,application::AssignMaterialToMeshAction{snap.project_generation,snap.project_revision,object,redid,core::Revision{redrev},{{1},{1}},{}}),"duplicate face rejected");
    check(!application::testing::dispatch(s,application::AssignMaterialToMeshAction{snap.project_generation,snap.project_revision,object,redid,core::Revision{999},{},{}}),"stale material rejected");
    check(take(s.authoring_viewport_source(s.snapshot().project_revision)).serialize()==before,"failed assignment leaves all source unchanged");
    send(s,application::UndoAction{});colors(image(s),false);send(s,application::RedoAction{});colors(image(s),true);
    send(s,application::SaveProjectAction{std::nullopt});auto reopened=take(project::ProjectDocument::load(path));
    check(reopened.meshes().at(mesh).attributes().find("cartographer.material_slot"),"face slots survive reopen");
    take(io::export_gltf(reopened,root/"partial.gltf"));take(io::export_glb(reopened,root/"partial.glb"));
    Payload p(root/"partial.gltf"),b(root/"partial.glb");check(array(member(array(member(p.json,"meshes"))[0],"primitives")).size()==2,"assigned faces export as two material primitives");
    check(member(p.json,"materials")==member(b.json,"materials"),"integrated GLTF/GLB material agreement");
    snap=s.snapshot();send(s,application::AssignMaterialToMeshAction{snap.project_generation,snap.project_revision,object,greenid,greenrev,{},{}});
    auto replaced=take(s.authoring_viewport_source(s.snapshot().project_revision));check(replaced.material_catalog().assignments().size()==1,"whole replacement retires obsolete region bindings atomically");
    send(s,application::UndoAction{});colors(image(s),true);
    snap=s.snapshot();const auto before_uv=take(s.authoring_viewport_source(snap.project_revision));
    send(s,application::GenerateUniqueLightmapUvAction{snap.project_revision,object,2U,1024U,4U,1U});
    auto uv_document=take(s.authoring_viewport_source(s.snapshot().project_revision));
    check(uv_document.meshes().at(mesh).uv_sets().find(2U)!=nullptr,"native lightmap action creates new UV identity");
    colors(image(s),true);
    const auto uv_source=uv_document.serialize();snap=s.snapshot();
    check(!application::testing::dispatch(s,application::GenerateUniqueLightmapUvAction{snap.project_revision,object,2U,1024U,4U,1U}),"occupied UV identity is not overwritten");
    check(take(s.authoring_viewport_source(s.snapshot().project_revision)).serialize()==uv_source,"failed UV replacement preserves materials and source");
    send(s,application::UndoAction{});
    const auto undone=take(s.authoring_viewport_source(s.snapshot().project_revision));
    check(undone.meshes().at(mesh).uv_sets().find(2U)==nullptr &&
        undone.meshes().at(mesh).attributes().layer_count()==before_uv.meshes().at(mesh).attributes().layer_count() &&
        undone.material_catalog().serialize()==before_uv.material_catalog().serialize(),"UV undo restores prior attributes and bindings while document revision advances");
    colors(image(s),true);
    send(s,application::RedoAction{});colors(image(s),true);send(s,application::SaveProjectAction{std::nullopt});
    check(take(project::ProjectDocument::load(path)).meshes().at(mesh).uv_sets().find(2U)!=nullptr,"generated UV and material regions survive reopen together");
    application::ApplicationSession legacy;send(legacy,application::SaveProjectAction{root/"legacy.carto"});send(legacy,application::CreateMeshObjectAction{"Legacy region mesh",quad(true)});
    auto ls=legacy.snapshot();auto blue=red;blue.emissive_factor={0,0,1};blue.emissive_strength=4;send(legacy,application::CreateMaterialAction{ls.project_revision,"Blue",blue});ls=legacy.snapshot();
    send(legacy,application::SetMaterialRegionAssignmentAction{ls.project_revision,{*ls.objects.front().object.mesh_asset,7},ls.materials.front().id,{}});colors(image(legacy),false,true);
    auto ld=take(legacy.authoring_viewport_source(legacy.snapshot().project_revision));take(io::export_gltf(ld,root/"legacy.gltf"));Payload lp(root/"legacy.gltf");
    auto mat=array(member(lp.json,"materials"))[0];auto emission=array(member(mat,"emissiveFactor"));check(std::stod(std::string(emission[2]))==1,"legacy material is exported rather than silently defaulted");
    std::cout<<"PASS human material creation -> whole mesh/selected faces -> visible HDR regions -> atomic undo/redo -> save/reopen -> GLTF/GLB; legacy region rendering/export\nartifacts="<<root<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
