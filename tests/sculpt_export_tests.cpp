#include <carto/application/sculpt_export.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/io/obj.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while(false)
struct TestProvider final : carto::application::SculptPreviewProvider {
 bool break_topology=false;
 bool unchanged=false;
 std::string_view name() const noexcept override { return "test"; }
 carto::core::Result<carto::geometry::CompiledMesh> evaluate(const carto::geometry::EditableMesh& source,const carto::production::SculptStack&) const override {
  auto product=source.compile();
  if (product) {
   if(!unchanged) {for(auto& p:product.value().positions) p.x+=0.25;
   product.value().bounds.minimum.x+=0.25; product.value().bounds.maximum.x+=0.25;}
   if(break_topology && product.value().indices.size()>=2) std::swap(product.value().indices[0],product.value().indices[1]);
  }
  return product;
 }
};
int main() {
 try {
  auto source=carto::geometry::make_box({2.,2.,2.}); CHECK(source);
  auto before=source.value().compile(); CHECK(before);
  carto::production::SculptStack stack;
  TestProvider provider;
  auto product=carto::application::materialize_sculpt_for_export(source.value(),stack,provider); CHECK(product);
  auto actual=product.value().compile(); CHECK(actual);
  CHECK(actual.value().indices==before.value().indices);
  CHECK(actual.value().vertex_ids==before.value().vertex_ids);
  for(size_t i=0;i<actual.value().positions.size();++i) CHECK(actual.value().positions[i].x==before.value().positions[i].x+0.25);
  auto again=source.value().compile(); CHECK(again && again.value().source_revision==before.value().source_revision);
  for(size_t i=0;i<again.value().positions.size();++i) CHECK(again.value().positions[i].x==before.value().positions[i].x);
  auto normal_mesh=carto::geometry::make_box({2.,2.,2.}); CHECK(normal_mesh);
  auto topology=normal_mesh.value().topology(); CHECK(topology);
  carto::attributes::AttributeSet payload({{carto::attributes::AttributeDomain::corner,topology.value().corners.size()}});
  std::vector<carto::attributes::AttributeValue> normals(topology.value().corners.size(),carto::core::Vec3d{0.,0.,1.});
  CHECK(payload.add_layer({{"cartographer.normal0",carto::attributes::AttributeDomain::corner,carto::attributes::AttributeType::vec3},std::move(normals)}));
  CHECK(normal_mesh.value().set_attribute_payload(std::move(payload),{}));
  CHECK(!carto::application::materialize_sculpt_for_export(normal_mesh.value(),stack,provider));
  auto geometry_only=carto::application::materialize_sculpt_for_export(normal_mesh.value(),stack,provider,
      carto::application::SculptExportFramePolicy::geometry_only); CHECK(geometry_only);
  CHECK(!carto::application::materialize_sculpt_for_export(normal_mesh.value(),stack,provider,
      static_cast<carto::application::SculptExportFramePolicy>(255)));
  auto geo_path=std::filesystem::temp_directory_path()/("carto-sculpt-geometry-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto ply=geo_path; ply += ".ply"; auto stl=geo_path; stl += ".stl";
  CHECK(carto::io::export_ply(geometry_only.value(),ply)); CHECK(carto::io::export_stl(geometry_only.value(),stl));
  CHECK(carto::io::import_ply(ply)); CHECK(carto::io::import_stl(stl));
  provider.unchanged=true; CHECK(carto::application::materialize_sculpt_for_export(normal_mesh.value(),stack,provider));
  provider.unchanged=false;
  provider.break_topology=true; CHECK(!carto::application::materialize_sculpt_for_export(source.value(),stack,provider));
  const auto path=std::filesystem::temp_directory_path()/("carto-evaluated-export-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".obj");
  CHECK(carto::io::export_obj(product.value(),path));
  auto loaded=carto::io::import_obj(path); CHECK(loaded);
  auto compiled=loaded.value().mesh.compile(); CHECK(compiled && compiled.value().positions.size()==actual.value().positions.size());
  for(size_t i=0;i<actual.value().positions.size();++i) CHECK(compiled.value().positions[i].x==actual.value().positions[i].x);
  std::cout<<"Sculpt export tests passed\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
