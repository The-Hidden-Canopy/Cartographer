#include <carto/project/project.hpp>
#include <cmath>
#include <set>
namespace carto::project {
core::Result<void> ProjectDocument::validate_rig_template_bindings() const {
    const auto fail=[](const char* reason){return core::Result<void>::failure({core::ErrorCode::validation_failed,reason});};
    if(rig_template_bindings_.size()>128)return fail("too many shared rig bindings");
    for(const auto& [root,b]:rig_template_bindings_) {
        if(root!=b.root||!scene_.find(root)||!rig_module_templates_.contains(b.module_identity)||
            b.module_revision.value()==0||b.module_digest.is_zero()||b.provenance.empty()||b.provenance.size()>512||
            !std::isfinite(b.length_scale)||b.length_scale<=0||b.length_scale>1000||b.joint_objects.empty()||b.joint_objects.size()>1024)
            return fail("invalid shared rig binding source, root, provenance or bounds");
        std::set<scene::ObjectId> unique;bool root_mapped=false;
        for(const auto& [joint,object]:b.joint_objects) {
            if(joint==0||!scene_.find(object)||!unique.insert(object).second)return fail("shared rig mapping has missing or duplicate targets");
            root_mapped=root_mapped||object==root;
            auto current=std::optional<scene::ObjectId>{object};bool descendant=false;
            for(std::size_t steps=0;current&&steps<=scene_.size();++steps) {
                if(*current==root){descendant=true;break;}
                const auto* node=scene_.find(*current);if(!node)break;current=node->parent;
            }
            if(!descendant)return fail("shared rig target lies outside its asset root");
        }
        if(!root_mapped)return fail("shared rig requires an explicit root correspondence");
    }
    return core::Result<void>::success();
}
core::Result<void> ProjectDocument::set_rig_template_binding(RigTemplateBinding binding) {
    if(revision_.exhausted())return core::Result<void>::failure({core::ErrorCode::invalid_state,"project revision exhausted"});
    auto copy=*this;copy.rig_template_bindings_[binding.root]=std::move(binding);
    auto valid=copy.validate_rig_template_bindings();if(!valid)return valid;
    rig_template_bindings_=std::move(copy.rig_template_bindings_);revision_=revision_.next();return core::Result<void>::success();
}
core::Result<void> ProjectDocument::set_rig_part_parent(scene::ObjectId part, scene::ObjectId parent) {
    if(revision_.exhausted())return core::Result<void>::failure({core::ErrorCode::invalid_state,"project revision exhausted"});
    // Reparenting an unmapped ancestor can move mapped descendants outside a
    // binding's root as well. Validate the candidate before publishing it so
    // a rejected move leaves both scene and project revisions untouched.
    auto candidate=*this;
    auto result=candidate.scene_.set_parent(part,parent);if(!result)return result;
    auto valid=candidate.validate_rig_template_bindings();if(!valid)return valid;
    scene_=std::move(candidate.scene_);
    revision_=revision_.next();return core::Result<void>::success();
}
core::Result<void> ProjectDocument::erase_rig_template_binding(scene::ObjectId root) {
    if(revision_.exhausted())return core::Result<void>::failure({core::ErrorCode::invalid_state,"project revision exhausted"});
    if(!rig_template_bindings_.erase(root))return core::Result<void>::failure({core::ErrorCode::not_found,"shared rig binding not found"});
    revision_=revision_.next();return core::Result<void>::success();
}
} // namespace carto::project
