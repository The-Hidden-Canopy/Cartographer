#include "application_access.hpp"
#include "gltf_test_payload.hpp"
#include <carto/character/rig_module.hpp>
namespace {
void send(application::ApplicationSession& s,const application::ApplicationAction& a){take(application::testing::dispatch(s,a));}
project::ProjectDocument source(application::ApplicationSession& s){return take(s.authoring_viewport_source(s.snapshot().project_revision));}
character::RigModuleTemplate module(bool skin=false) {
    character::RigModuleTemplate m;m.name=skin?"public.synthetic.skinned":"public.synthetic.rigid";
    kinematic::KinematicNode root;root.id={1};root.semantic_type="frame";
    auto part=root;part.id={2};part.parent=root.id;part.kind=kinematic::JointKind::hinge;part.semantic_type="panel";part.rest.translation={0,3,0};
    if(skin)part.node_class=kinematic::NodeClass::skinned;m.joints={root,part};return m;
}
project::RigTemplateBinding binding(application::ApplicationSession& s,std::uint64_t root,std::uint64_t part,double scale=1,std::string name="public.synthetic.rigid") {
    auto d=source(s);const auto& entry=d.rig_module_templates().at(name);
    return {{root},name,entry.revision,entry.digest,"Independently authored public-safe local fixture",scale,{{1,{root}},{2,{part}}}};
}
void apply(application::ApplicationSession& s,project::RigTemplateBinding b) {
    auto snap=s.snapshot();send(s,application::ApplySharedRigAction{snap.project_generation,snap.project_revision,std::move(b),true,true});
}
}
int main(){try {
    const auto root=std::filesystem::temp_directory_path()/("shared-rig-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
    const auto path=root/"assets.carto";auto d=take(project::ProjectDocument::create("Two independent rigid assets"));
    journal::Journal journal(root/"fixture.journal");const auto initial=d.serialize();
    take(journal.append({{},d.revision(),"rig.fixture",{reinterpret_cast<const std::uint8_t*>(initial.data()),initial.size()},std::nullopt}));
    auto tx=take(project::ProjectTransaction::begin(d,journal,"rig.fixture"));
    geometry::EditableMesh mesh;okay(mesh.insert_bulk({{{1},{0,0,0}},{{2},{1,0,0}},{{3},{0,1,0}}},{{{1},{{1},{2},{3}}}}));
    const auto mesh_id=take(tx.add_mesh(std::move(mesh)));
    okay(tx.insert_object({{1},"Asset A",{{10,0,0}},std::nullopt,mesh_id}));
    okay(tx.insert_object({{2},"A panel",{{0,8,0},{},{2,3,4}},std::nullopt,mesh_id}));
    okay(tx.insert_object({{3},"Asset B",{{-10,0,0}},std::nullopt,mesh_id}));
    okay(tx.insert_object({{4},"B panel",{{0,9,0}},std::nullopt,mesh_id}));
    okay(tx.insert_object({{5},"Asset C",{{0,0,10}},std::nullopt,mesh_id}));
    okay(tx.insert_object({{6},"C panel",{{0,2,0}},scene::ObjectId{5},mesh_id}));
    okay(tx.insert_object({{7},"A tip",{{0,4,0},{},{.5,1,.5}},std::nullopt,mesh_id}));
    okay(tx.insert_object({{8},"C tip",{{0,4,10}},std::nullopt,mesh_id}));
    take(tx.commit());take(d.save_atomic(path));
    application::ApplicationSession s;send(s,application::OpenProjectAction{path});
    auto snap=s.snapshot();const auto bytes=take(module().serialize());
    check(!application::testing::dispatch(s,application::ImportRigModuleAction{snap.project_generation,snap.project_revision,bytes,false}),"reuse rights require explicit human confirmation");
    send(s,application::ImportRigModuleAction{snap.project_generation,snap.project_revision,bytes,true});
    const auto library_digest=source(s).rig_module_templates().begin()->second.digest;
    auto a=binding(s,1,2);snap=s.snapshot();
    check(!application::testing::dispatch(s,application::ApplySharedRigAction{snap.project_generation,snap.project_revision,a,false,true}),"ambiguous correspondence cannot auto-apply");
    apply(s,a);auto applied=source(s);
    check(applied.scene().find({2})->parent==scene::ObjectId{1}&&applied.scene().find({2})->local_transform.translation.y==3,"explicit reusable structure reparents and places target part");
    check(applied.scene().find({2})->local_transform.scale.x==2&&applied.scene().find({2})->mesh_asset==mesh_id,"target shape scale and mesh identity retained");
    apply(s,binding(s,3,4,2));const auto second=source(s);
    check(second.rig_template_bindings().size()==2&&second.scene().find({4})->parent==scene::ObjectId{3}&&second.scene().find({4})->local_transform.translation.y==6,"same shared structure reused on second asset with explicit scale");
    check(second.scene().find({1})->local_transform.translation.x==10&&second.scene().find({3})->local_transform.translation.x==-10,"asset placement preserved");
    check(second.rig_module_templates().begin()->second.digest==library_digest,"source rig identity and digest preserved");
    send(s,application::UndoAction{});auto undone=source(s);
    check(undone.rig_template_bindings().size()==1&&!undone.scene().find({4})->parent&&undone.scene().find({4})->local_transform.translation.y==9,"undo restores original hierarchy/rest and only removes new binding");
    send(s,application::RedoAction{});send(s,application::SaveProjectAction{std::nullopt});
    auto reopened=take(project::ProjectDocument::load(path));
    check(reopened.rig_template_bindings().size()==2&&reopened.rig_template_bindings().at({3}).module_digest==library_digest,"bindings, source provenance and explicit mapping survive reopen");
    auto before=source(s).serialize();snap=s.snapshot();auto duplicate=binding(s,1,1);
    check(!application::testing::dispatch(s,application::ApplySharedRigAction{snap.project_generation,snap.project_revision,duplicate,true,true}),"duplicate target refused");
    auto stale=binding(s,1,2);stale.module_revision=stale.module_revision.next();
    check(!application::testing::dispatch(s,application::ApplySharedRigAction{snap.project_generation,snap.project_revision,stale,true,true}),"stale source refused");
    check(source(s).serialize()==before,"mapping failures leave existing rigs and geometry untouched");
    snap=s.snapshot();send(s,application::ImportRigModuleAction{snap.project_generation,snap.project_revision,take(module(true).serialize()),true});
    before=source(s).serialize();snap=s.snapshot();auto skin=binding(s,1,2,1,"public.synthetic.skinned");
    check(!application::testing::dispatch(s,application::ApplySharedRigAction{snap.project_generation,snap.project_revision,skin,true,true}),"incompatible skinned rig cannot silently become rigid");
    check(source(s).serialize()==before,"incompatible rig refusal is atomic");
    snap=s.snapshot();send(s,application::CaptureSharedRigModuleAction{snap.project_generation,snap.project_revision,{1},"public.synthetic.captured",true});
    const auto captured=take(character::RigModuleTemplate::deserialize(source(s).rig_module_templates().at("public.synthetic.captured").bytes));
    check(captured.joints.size()==2&&captured.joints[1].kind==kinematic::JointKind::hinge,"capture reuses existing joint definitions and current structural rest frames");
    apply(s,binding(s,3,4,2,"public.synthetic.captured"));
    snap=s.snapshot();send(s,application::UnlinkSharedRigAction{snap.project_generation,snap.project_revision,{3}});
    check(source(s).rig_template_bindings().size()==1&&source(s).scene().find({4})->local_transform.translation.y==6,"unlink removes only source binding and preserves current target structure");
    send(s,application::UndoAction{});check(source(s).rig_template_bindings().at({3}).module_identity=="public.synthetic.captured","unlink is independently undoable");
    snap=s.snapshot();send(s,application::CaptureSharedRigModuleAction{snap.project_generation,snap.project_revision,{5},"public.synthetic.fixed",true});
    const auto fixed=take(character::RigModuleTemplate::deserialize(source(s).rig_module_templates().at("public.synthetic.fixed").bytes));
    check(fixed.joints.size()==2&&fixed.joints[1].kind==kinematic::JointKind::fixed&&fixed.joints[1].rest.translation.y==2,"unrigged hierarchy capture creates reusable fixed structure without copying a mesh");
    send(s,application::SaveProjectAction{std::nullopt});
    check(take(project::ProjectDocument::load(path)).rig_module_templates().contains("public.synthetic.fixed"),"captured local structures survive reopen");
    auto deep=module();deep.name="public.synthetic.deep";auto tip=deep.joints.back();tip.id={3};tip.parent=kinematic::JointId{2};tip.kind=kinematic::JointKind::fixed;tip.semantic_type="tip";tip.rest.translation={0,2,0};deep.joints.push_back(tip);
    snap=s.snapshot();send(s,application::ImportRigModuleAction{snap.project_generation,snap.project_revision,take(deep.serialize()),true});
    auto deep_a=binding(s,1,2,1,deep.name);deep_a.joint_objects[3]={7};apply(s,deep_a);
    const auto deep_result=source(s);const auto a_tip=take(deep_result.scene().world_transform({7}));
    check(std::abs(a_tip.translation.y-5)<1e-9&&a_tip.scale.x==.5&&a_tip.scale.y==1,"deep retarget preserves rest-frame positions and target world shape through a nonuniformly scaled mesh parent");
    auto deep_c=binding(s,5,6,.5,deep.name);deep_c.joint_objects[3]={8};apply(s,deep_c);
    const auto c_tip=take(source(s).scene().world_transform({8}));
    check(std::abs(c_tip.translation.y-2.5)<1e-9&&c_tip.scale.x==1&&c_tip.scale.y==1,"same deep rig retargets to a second asset without copying the first target's mesh scales");
    auto legacy=project::ProjectDocument{}.serialize();const auto table=legacy.find("RIG_TEMPLATE_BINDINGS 0\n");legacy.erase(table,std::string("RIG_TEMPLATE_BINDINGS 0\n").size());
    check(!project::ProjectDocument::deserialize(legacy),"schema34 requires rig binding table");
    const auto aging=legacy.find("AGING_LAYERS 0\n");legacy.erase(aging,std::string("AGING_LAYERS 0\n").size());
    legacy.replace(0,std::string("CARTOGRAPHER_PROJECT 35").size(),"CARTOGRAPHER_PROJECT 33");
    check(take(project::ProjectDocument::deserialize(legacy)).rig_template_bindings().empty(),"schema33 migrates with no invented bindings");
    std::cout<<"PASS two-asset rig reuse, explicit correspondence/license, rigid rest retarget, incompatible refusal, undo/reopen and v33 migration\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
