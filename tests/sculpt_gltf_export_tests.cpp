#include <carto/application/sculpt_export.hpp>
#include <carto/ui/ui.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/io/gltf_export.hpp>
#include <carto/io/gltf_scene_import.hpp>
#include <carto/project/project.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)
struct OffsetProvider final : carto::application::SculptPreviewProvider {
 std::string_view name() const noexcept override { return "offset"; }
 carto::core::Result<carto::geometry::CompiledMesh> evaluate(const carto::geometry::EditableMesh& mesh,const carto::production::SculptStack&) const override {
  auto result=mesh.compile(); if(result){for(auto& p:result.value().positions) p.x+=0.25; result.value().bounds.minimum.x+=0.25; result.value().bounds.maximum.x+=0.25;} return result;
 }
};
int main(){try {
 carto::application::ApplicationSession session; carto::ui::UiController ui(session);
 CHECK(ui.dispatch(carto::application::NewProjectAction{"Sculpt export"}));
 auto mesh=carto::geometry::make_box({2.,2.,2.}); CHECK(mesh);
 auto topology=mesh.value().topology(); CHECK(topology);
 std::vector<carto::attributes::AttributeValue> uv;
 for(const auto& corner:topology.value().corners) {const auto* vertex=mesh.value().find_vertex(corner.vertex); CHECK(vertex); uv.emplace_back(carto::core::Vec2d{(vertex->position.x+1.)/2.,(vertex->position.y+1.)/2.});}
 carto::attributes::AttributeSet payload({{carto::attributes::AttributeDomain::corner,topology.value().corners.size()}});
 CHECK(payload.add_layer({{"uv0",carto::attributes::AttributeDomain::corner,carto::attributes::AttributeType::vec2},std::move(uv)}));
 carto::attributes::UvSetTable sets; CHECK(sets.add({1U,"UV","uv0",true,true,false,1U}));
 CHECK(mesh.value().set_attribute_payload(std::move(payload),std::move(sets)));
 CHECK(ui.dispatch(carto::application::CreateMeshObjectAction{"Box",std::move(mesh.value())}));
 CHECK(ui.set_workspace(carto::ui::Workspace::sculpt));
 auto snap=ui.snapshot(); auto object=snap.objects.front().object.id;
 CHECK(ui.dispatch(carto::application::DuplicateMeshInstanceAction{object,"Instance",{3.,0.,0.}}));
 CHECK(ui.dispatch(carto::application::SelectObjectAction{object}));
 CHECK(ui.dispatch(carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
 CHECK(ui.dispatch(carto::application::SelectVertexAction{object,carto::geometry::VertexId{1}}));
 snap=ui.snapshot(); CHECK(ui.dispatch(carto::application::AppendSculptStrokeAction{snap.project_revision,carto::production::SculptStrokeMode::add,0.5,0.05,snap.project_generation}));
 auto path=std::filesystem::temp_directory_path()/("carto-sculpt-gltf-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".carto");
 CHECK(ui.dispatch(carto::application::SaveProjectAction{path}));
 auto doc=carto::project::ProjectDocument::load(path); CHECK(doc); auto serialized=doc.value().serialize();
 auto id=*doc.value().scene().find(object)->mesh_asset; const auto& source=doc.value().meshes().at(id);
 auto stack=carto::production::SculptStack::deserialize(doc.value().sculpt_stacks().at(id).bytes); CHECK(stack);
 OffsetProvider provider; auto product=carto::application::materialize_sculpt_for_export(source,stack.value(),provider); CHECK(product);
 auto before=source.vertices_sorted(),after=product.value().vertices_sorted(); carto::geometry::MeshPatch patch; patch.expected_revision=source.revision();
 for(size_t i=0;i<before.size();++i) patch.vertex_changes.push_back({before[i].id,before[i].position,after[i].position});
 carto::io::GltfExportOptions options; options.evaluated_sculpt.emplace(id,carto::io::GltfEvaluatedSculpt{carto::assets::sha256({reinterpret_cast<const std::uint8_t*>(serialized.data()),serialized.size()}),doc.value().sculpt_stacks().at(id).digest,patch});
 auto missing=path; missing += ".missing.gltf"; CHECK(!carto::io::export_gltf(doc.value(),missing)); CHECK(!std::filesystem::exists(missing));
 {std::ofstream sentinel(missing,std::ios::binary); sentinel<<"prior-good-output";}
 auto stale=options; stale.evaluated_sculpt.at(id).positions.vertex_changes.front().before.x+=1.0; CHECK(!carto::io::export_gltf(doc.value(),missing,stale));
 {std::ifstream sentinel(missing,std::ios::binary); std::string text((std::istreambuf_iterator<char>(sentinel)),{}); CHECK(text=="prior-good-output");}
 for(bool glb:{false,true}) {auto out=path; out+=glb?".glb":".gltf";
  auto report=glb?carto::io::export_glb(doc.value(),out,options):carto::io::export_gltf(doc.value(),out,options); CHECK(report);
  std::ifstream file(out,std::ios::binary); std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
  auto imported=carto::io::import_gltf_scene(bytes,{true}); CHECK(imported && imported.value().meshes.size()==1);
  CHECK(imported.value().nodes.size()==2 && imported.value().nodes[0].mesh_asset==imported.value().nodes[1].mesh_asset);
  const auto& loaded_mesh=imported.value().meshes.front();
  const auto* uv_desc=loaded_mesh.uv_sets().active_for_render(); CHECK(uv_desc);
  const auto* uv_layer=loaded_mesh.attributes().find(uv_desc->layer_id); CHECK(uv_layer);
  auto loaded_topology=loaded_mesh.topology(); CHECK(loaded_topology);
  auto corners=loaded_topology.value().corners;
  std::sort(corners.begin(),corners.end(),[](const auto& a,const auto& b){return a.id<b.id;});
  CHECK(uv_layer->values.size()==corners.size());
  for(size_t i=0;i<corners.size();++i) {auto vertex=loaded_mesh.find_vertex(corners[i].vertex); CHECK(vertex); auto loaded_uv=std::get_if<carto::core::Vec2d>(&uv_layer->values[i]); CHECK(loaded_uv); CHECK(std::abs(loaded_uv->x-(vertex->position.x-0.25+1.)/2.)<1e-5 && std::abs(loaded_uv->y-(vertex->position.y+1.)/2.)<1e-5);}
  auto vertices=loaded_mesh.vertices_sorted(); CHECK(vertices.size()==after.size());
  std::vector<bool> matched(vertices.size(),false); for(const auto& expected:after){bool found=false; for(size_t i=0;i<vertices.size();++i){auto a=vertices[i].position,b=expected.position;if(!matched[i] && std::abs(a.x-b.x)<1e-5 && std::abs(a.y-b.y)<1e-5 && std::abs(a.z-b.z)<1e-5){matched[i]=true;found=true;break;}} CHECK(found);}
 }
 CHECK(doc.value().serialize()==serialized);
 auto layer_state=ui.snapshot(); CHECK(layer_state.sculpt_layers.size()==1);
 CHECK(ui.dispatch(carto::application::SetSculptLayerVisibilityAction{layer_state.project_revision,id,layer_state.sculpt_layers.front().identity,false,layer_state.project_generation}));
 auto second_path=path; second_path += ".changed.carto";
 CHECK(ui.dispatch(carto::application::SaveProjectAction{second_path}));
 auto changed=carto::project::ProjectDocument::load(second_path); CHECK(changed);
 CHECK(changed.value().meshes().at(id).revision()==source.revision());
 CHECK(changed.value().sculpt_stacks().at(id).digest!=doc.value().sculpt_stacks().at(id).digest);
 CHECK(!carto::io::export_gltf(changed.value(),missing,options));
 {std::ifstream sentinel(missing,std::ios::binary); std::string text((std::istreambuf_iterator<char>(sentinel)),{}); CHECK(text=="prior-good-output");}
 // A changed surface with authored normals must not silently reuse stale shading.
 carto::application::ApplicationSession normal_session; carto::ui::UiController normal_ui(normal_session);
 CHECK(normal_ui.dispatch(carto::application::NewProjectAction{"Normals"}));
 auto normal_mesh=carto::geometry::make_box({2.,2.,2.}); CHECK(normal_mesh);
 auto normal_topology=normal_mesh.value().topology(); CHECK(normal_topology);
 carto::attributes::AttributeSet normal_payload({{carto::attributes::AttributeDomain::corner,normal_topology.value().corners.size()}});
 std::vector<carto::attributes::AttributeValue> normals(normal_topology.value().corners.size(),carto::core::Vec3d{0.,0.,1.});
 CHECK(normal_payload.add_layer({{"cartographer.normal0",carto::attributes::AttributeDomain::corner,carto::attributes::AttributeType::vec3},std::move(normals)}));
 CHECK(normal_mesh.value().set_attribute_payload(std::move(normal_payload),{}));
 CHECK(normal_ui.dispatch(carto::application::CreateMeshObjectAction{"Normals",std::move(normal_mesh.value())}));
 auto ns=normal_ui.snapshot(); auto no=ns.objects.front().object.id;
 CHECK(normal_ui.set_workspace(carto::ui::Workspace::sculpt));
 CHECK(normal_ui.dispatch(carto::application::SelectObjectAction{no}));
 CHECK(normal_ui.dispatch(carto::application::SetSelectionModeAction{carto::editor::SelectionMode::vertex}));
 CHECK(normal_ui.dispatch(carto::application::SelectVertexAction{no,carto::geometry::VertexId{1}}));
 ns=normal_ui.snapshot(); CHECK(normal_ui.dispatch(carto::application::AppendSculptStrokeAction{ns.project_revision,carto::production::SculptStrokeMode::add,0.5,0.05,ns.project_generation}));
 auto normal_path=path; normal_path += ".normals.carto"; CHECK(normal_ui.dispatch(carto::application::SaveProjectAction{normal_path}));
 auto nd=carto::project::ProjectDocument::load(normal_path); CHECK(nd); auto ni=*nd.value().scene().find(no)->mesh_asset;
 const auto& nm=nd.value().meshes().at(ni); auto nstack=carto::production::SculptStack::deserialize(nd.value().sculpt_stacks().at(ni).bytes); CHECK(nstack);
 auto np=carto::application::materialize_sculpt_for_export(nm,nstack.value(),provider); CHECK(np);
 auto nv=nm.vertices_sorted(), na=np.value().vertices_sorted(); carto::geometry::MeshPatch n_patch; n_patch.expected_revision=nm.revision();
 for(size_t i=0;i<nv.size();++i) n_patch.vertex_changes.push_back({nv[i].id,nv[i].position,na[i].position});
 auto nbytes=nd.value().serialize(); carto::io::GltfExportOptions noptions;
 noptions.evaluated_sculpt.emplace(ni,carto::io::GltfEvaluatedSculpt{carto::assets::sha256({reinterpret_cast<const std::uint8_t*>(nbytes.data()),nbytes.size()}),nd.value().sculpt_stacks().at(ni).digest,n_patch});
 CHECK(!carto::io::export_glb(nd.value(),missing,noptions));
 {std::ifstream sentinel(missing,std::ios::binary); std::string text((std::istreambuf_iterator<char>(sentinel)),{}); CHECK(text=="prior-good-output");}
 std::cout<<"Sculpt glTF export passed\n";
 } catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
