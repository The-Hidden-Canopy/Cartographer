#include <carto/application/application.hpp>
#include <carto/character/rig_module.hpp>
#include <cmath>
#include <set>
#include <tuple>
#include <cctype>
namespace carto::application {
core::Result<DispatchReceipt> ApplicationSession::unlink_shared_rig(const UnlinkSharedRigAction& action) {
    if(active_source_!=OperationSource::human)return failure({core::ErrorCode::unsupported,"rig unlink requires human authoring"});
    if(project_generation_!=action.expected_project_generation||document_.revision()!=action.expected_project_revision)return failure({core::ErrorCode::stale_data,"rig unlink snapshot is stale"});
    auto candidate=document_;if(auto removed=candidate.erase_rig_template_binding(action.root);!removed)return failure(removed.error());
    editor::ProjectCommandAdmission admission;
    return execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(admission,document_,std::move(candidate),"Unlink Shared Rig Source"),"Unlink Shared Rig Source",AffectedSet{{action.root},{},{},{}});
}
core::Result<DispatchReceipt> ApplicationSession::capture_shared_rig(const CaptureSharedRigModuleAction& action) {
    if(active_source_!=OperationSource::human||!action.ownership_or_license_confirmed)
        return failure({core::ErrorCode::unsupported,"capturing shared structures requires human local reuse-rights confirmation"});
    if(project_generation_!=action.expected_project_generation||document_.revision()!=action.expected_project_revision)
        return failure({core::ErrorCode::stale_data,"shared structure capture snapshot is stale"});
    if(!document_.scene().find(action.root))return failure({core::ErrorCode::not_found,"shared structure root is missing"});
    character::RigModuleTemplate module;
    if(auto bound=document_.rig_template_bindings().find(action.root);bound!=document_.rig_template_bindings().end()) {
        const auto source=document_.rig_module_templates().find(bound->second.module_identity);
        if(source==document_.rig_module_templates().end()||source->second.digest!=bound->second.module_digest||source->second.revision!=bound->second.module_revision)
            return failure({core::ErrorCode::stale_data,"repair stale rig source correspondence before capturing it for reuse"});
        auto decoded=character::RigModuleTemplate::deserialize(source->second.bytes);if(!decoded)return failure(decoded.error());module=std::move(decoded.value());
        for(auto& joint:module.joints) {
            const auto target=bound->second.joint_objects.find(joint.id.value);
            if(target==bound->second.joint_objects.end()||!document_.scene().find(target->second))return failure({core::ErrorCode::stale_data,"rig capture correspondence no longer resolves"});
            joint.rest=joint.parent?document_.scene().find(target->second)->local_transform:core::Transform::identity();
        }
    } else {
        std::set<scene::ObjectId> descendants{action.root};
        const auto objects=document_.scene().objects_sorted();
        std::map<scene::ObjectId,std::vector<scene::ObjectId>> children;
        for(const auto& o:objects)if(o.parent)children[*o.parent].push_back(o.id);
        std::vector<scene::ObjectId> pending{action.root};
        for(std::size_t cursor=0;cursor<pending.size();++cursor)for(auto child:children[pending[cursor]])if(descendants.insert(child).second) {
            if(descendants.size()>character::RigModuleTemplate::kMaxJoints)return failure({core::ErrorCode::unsupported,"shared hierarchy exceeds module joint budget"});pending.push_back(child);
        }
        for(const auto& o:objects)if(descendants.contains(o.id)) {
            kinematic::KinematicNode node;node.id={o.id.value};node.rest=o.id==action.root?core::Transform::identity():o.local_transform;
            if(o.id!=action.root)node.parent=kinematic::JointId{o.parent->value};
            std::string role;for(char byte:o.name){const auto c=static_cast<unsigned char>(byte);if(role.size()>=96)break;if(std::isalnum(c)&&c<128)role.push_back(static_cast<char>(std::tolower(c)));else if(!role.empty()&&role.back()!='.')role.push_back('.');}
            node.semantic_type=(role.empty()?"part":role)+"."+std::to_string(o.id.value);module.joints.push_back(std::move(node));
        }
    }
    module.name=action.module_identity;auto bytes=module.serialize();if(!bytes)return failure(bytes.error());
    auto candidate=document_;if(auto added=candidate.insert_rig_module_template(std::move(bytes.value()));!added)return failure(added.error());
    editor::ProjectCommandAdmission admission;
    return execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(admission,document_,std::move(candidate),"Capture Reusable Structure/Rig"),"Capture Reusable Structure/Rig",AffectedSet{{action.root},{},{},{}});
}
core::Result<DispatchReceipt> ApplicationSession::import_rig_module(const ImportRigModuleAction& action) {
    if(active_source_!=OperationSource::human||!action.ownership_or_license_confirmed)
        return failure({core::ErrorCode::unsupported,"rig import requires human confirmation of local ownership or reuse license"});
    if(project_generation_!=action.expected_project_generation||document_.revision()!=action.expected_project_revision)
        return failure({core::ErrorCode::stale_data,"rig import snapshot is stale"});
    auto candidate=document_;
    if(auto imported=candidate.insert_rig_module_template(action.canonical_module);!imported)return failure(imported.error());
    editor::ProjectCommandAdmission admission;
    return execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(admission,document_,std::move(candidate),"Import Shared Rig Module"),
        "Import Shared Rig Module (local reuse rights confirmed)",{});
}
core::Result<DispatchReceipt> ApplicationSession::apply_shared_rig(const ApplySharedRigAction& action) {
    if(active_source_!=OperationSource::human||!action.correspondence_confirmed||!action.ownership_or_license_confirmed)
        return failure({core::ErrorCode::unsupported,"shared rig requires explicit human correspondence and local reuse-rights confirmation"});
    if(project_generation_!=action.expected_project_generation||document_.revision()!=action.expected_project_revision)
        return failure({core::ErrorCode::stale_data,"shared rig application snapshot is stale"});
    const auto& b=action.binding;
    const auto entry=document_.rig_module_templates().find(b.module_identity);
    if(entry==document_.rig_module_templates().end()||entry->second.revision!=b.module_revision||entry->second.digest!=b.module_digest)
        return failure({core::ErrorCode::stale_data,"shared rig source identity/revision/digest no longer matches"});
    auto module=character::RigModuleTemplate::deserialize(entry->second.bytes);if(!module)return failure(module.error());
    if(b.joint_objects.size()!=module.value().joints.size())return failure({core::ErrorCode::invalid_argument,"every source rig joint requires an explicit unique target correspondence"});
    if(!std::isfinite(b.length_scale)||b.length_scale<=0||b.length_scale>1000)return failure({core::ErrorCode::invalid_argument,"rig length scale is outside (0,1000]"});
    auto candidate=document_;
    std::set<scene::ObjectId> targets;
    for(const auto& [joint,object]:b.joint_objects) {
        static_cast<void>(joint);
        if(!targets.insert(object).second)return failure({core::ErrorCode::invalid_argument,"rig correspondences must use unique targets"});
    }
    AffectedSet affected;
    std::vector<std::tuple<scene::ObjectId,scene::ObjectId,core::Transform>> edits;
    std::map<scene::ObjectId,core::Vec3d> original_world_scales;
    for(const auto& joint:module.value().joints) {
        auto mapped=b.joint_objects.find(joint.id.value);
        if(mapped==b.joint_objects.end())return failure({core::ErrorCode::invalid_argument,"shared rig joint correspondence is incomplete"});
        const auto* target=document_.scene().find(mapped->second);
        if(!target||target->locked||!target->visible)return failure({core::ErrorCode::invalid_state,"shared rig target is missing, hidden or locked"});
        for(auto ancestor=target->parent;ancestor;) {
            const auto* parent=document_.scene().find(*ancestor);
            if(!parent||parent->locked)return failure({core::ErrorCode::invalid_state,"shared rig cannot retarget parts from a locked hierarchy"});
            ancestor=parent->parent;
        }
        if(joint.node_class!=kinematic::NodeClass::rigid)return failure({core::ErrorCode::unsupported,"skinned rig reuse requires explicit compatible skin-weight retargeting; rigid-part binding cannot substitute for it"});
        auto old_world=document_.scene().world_transform(target->id);if(!old_world)return failure(old_world.error());
        original_world_scales[target->id]=old_world.value().scale;
        if(!joint.parent) {
            if(target->id!=b.root)return failure({core::ErrorCode::invalid_argument,"source attachment root must map to the selected asset root"});
        } else {
            const auto parent=b.joint_objects.find(joint.parent->value);
            if(parent==b.joint_objects.end())return failure({core::ErrorCode::invalid_argument,"rig parent correspondence is incomplete"});
            auto rest=joint.rest;rest.translation=rest.translation*b.length_scale;
            edits.emplace_back(target->id,parent->second,rest);
        }
        affected.objects.push_back(target->id);
    }
    auto root_world=document_.scene().world_transform(b.root);if(!root_world)return failure(root_world.error());
    std::map<scene::ObjectId,core::Transform> desired{{b.root,root_world.value()}};
    std::vector<scene::ObjectId> order;
    while(order.size()<edits.size()) {
        bool progress=false;
        for(const auto& [part,parent,rest]:edits)if(!desired.contains(part)&&desired.contains(parent)) {
            desired[part]=desired.at(parent).combine(rest);order.push_back(part);progress=true;
        }
        if(!progress)return failure({core::ErrorCode::invalid_argument,"shared rig rest hierarchy cannot be resolved"});
    }
    // Establish a temporary star only inside the unpublished candidate. This
    // avoids rejecting a valid retarget merely because source joint IDs are
    // not topologically ordered relative to the old target hierarchy.
    for(const auto& [part,parent,rest]:edits) {
        static_cast<void>(parent);static_cast<void>(rest);
        if(auto changed=candidate.set_rig_part_parent(part,b.root);!changed)return failure(changed.error());
    }
    for(const auto& [part,parent,rest]:edits) {
        static_cast<void>(rest);
        if(auto changed=candidate.set_rig_part_parent(part,parent);!changed)return failure(changed.error());
    }
    for(auto part:order) {
        const auto* object=candidate.scene().find(part);auto parent_world=candidate.scene().world_transform(*object->parent);
        if(!parent_world)return failure(parent_world.error());
        const auto& p=parent_world.value();const auto& target=desired.at(part);
        if(std::abs(p.scale.x)<1e-12||std::abs(p.scale.y)<1e-12||std::abs(p.scale.z)<1e-12)
            return failure({core::ErrorCode::unsupported,"shared rig cannot infer a rest frame through singular parent scale"});
        const auto q=p.rotation.normalized();const core::Quaternion inverse{-q.x,-q.y,-q.z,q.w};
        const auto delta=inverse.rotate(target.translation-p.translation);const auto shape=original_world_scales.at(part);
        core::Transform local{{delta.x/p.scale.x,delta.y/p.scale.y,delta.z/p.scale.z},(inverse*target.rotation).normalized(),{shape.x/p.scale.x,shape.y/p.scale.y,shape.z/p.scale.z}};
        if(auto changed=candidate.set_object_transform(part,local);!changed)return failure(changed.error());
    }
    if(auto bound=candidate.set_rig_template_binding(b);!bound)return failure(bound.error());
    if(auto valid=candidate.validate();!valid)return failure(valid.error());
    editor::ProjectCommandAdmission admission;
    return execute_command(std::make_unique<editor::ReplaceProjectDocumentCommand>(admission,document_,std::move(candidate),"Apply Shared Rig Rest Structure"),
        "Apply Shared Rig Rest Structure",std::move(affected));
}
} // namespace carto::application
