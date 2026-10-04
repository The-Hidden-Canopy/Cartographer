#include <carto/attributes/uv.hpp>

#include <string>
#include <utility>

namespace carto::attributes {

namespace {

core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

core::Result<void> validation(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::validation_failed, std::move(message)));
}

core::Result<void> not_found(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::not_found, std::move(message)));
}

} // namespace

core::Result<void> UvSetDescriptor::validate() const {
    if (id == 0U || name.empty() || layer_id.empty()) {
        return invalid("UV set identity, display name, and corner layer are required");
    }
    if (tile_policy_version == 0U) {
        return invalid("UV set tile policy version must be non-zero");
    }
    return core::Result<void>::success();
}

core::Result<void> UvSetTable::add(UvSetDescriptor descriptor) {
    if (auto result = descriptor.validate(); !result) return result;
    if (descriptors_.contains(descriptor.id)) {
        return validation("UV set identity already exists");
    }
    for (const auto& [ignored, existing] : descriptors_) {
        static_cast<void>(ignored);
        if (existing.layer_id == descriptor.layer_id) {
            return validation("UV set corner layer is already bound to another set");
        }
    }
    descriptors_.emplace(descriptor.id, std::move(descriptor));
    return core::Result<void>::success();
}

core::Result<void> UvSetTable::remove(
    UvSetId id,
    std::span<const UvSetId> referenced_sets) {
    if (!descriptors_.contains(id)) return not_found("UV set does not exist");
    for (const UvSetId referenced : referenced_sets) {
        if (referenced == id) {
            return validation(
                "UV set removal requires explicit consumer rebind before deletion");
        }
    }
    descriptors_.erase(id);
    return core::Result<void>::success();
}

core::Result<void> UvSetTable::set_active_for_editing(UvSetId id) {
    if (!descriptors_.contains(id)) return not_found("editing UV set does not exist");
    for (auto& [ignored, descriptor] : descriptors_) {
        static_cast<void>(ignored);
        descriptor.active_for_editing = false;
    }
    descriptors_.at(id).active_for_editing = true;
    return core::Result<void>::success();
}

core::Result<void> UvSetTable::set_active_for_render(UvSetId id) {
    if (!descriptors_.contains(id)) return not_found("render UV set does not exist");
    for (auto& [ignored, descriptor] : descriptors_) {
        static_cast<void>(ignored);
        descriptor.active_for_render = false;
    }
    descriptors_.at(id).active_for_render = true;
    return core::Result<void>::success();
}

core::Result<void> UvSetTable::validate(const AttributeSet& attributes) const {
    if (auto result = attributes.validate(); !result) return result;
    std::size_t active_editing = 0U;
    std::size_t active_render = 0U;
    std::map<std::string, UvSetId> layers;
    for (const auto& [id, descriptor] : descriptors_) {
        if (id != descriptor.id) return validation("UV set map key does not match identity");
        if (auto result = descriptor.validate(); !result) return result;
        if (descriptor.active_for_editing) ++active_editing;
        if (descriptor.active_for_render) ++active_render;
        if (!layers.emplace(descriptor.layer_id, id).second) {
            return validation("UV set corner layers must be unique");
        }
        const AttributeLayer* layer = attributes.find(descriptor.layer_id);
        if (layer == nullptr) {
            return not_found("UV set corner layer does not exist");
        }
        if (layer->descriptor.domain != AttributeDomain::corner ||
            layer->descriptor.type != AttributeType::vec2) {
            return validation("UV set coordinate layers must be corner-domain vec2 attributes");
        }
    }
    if (active_editing > 1U || active_render > 1U) {
        return validation("UV set table may have at most one active edit and render set");
    }
    return core::Result<void>::success();
}

const UvSetDescriptor* UvSetTable::find(UvSetId id) const noexcept {
    const auto iterator = descriptors_.find(id);
    return iterator == descriptors_.end() ? nullptr : &iterator->second;
}

const UvSetDescriptor* UvSetTable::active_for_editing() const noexcept {
    for (const auto& [ignored, descriptor] : descriptors_) {
        static_cast<void>(ignored);
        if (descriptor.active_for_editing) return &descriptor;
    }
    return nullptr;
}

const UvSetDescriptor* UvSetTable::active_for_render() const noexcept {
    for (const auto& [ignored, descriptor] : descriptors_) {
        static_cast<void>(ignored);
        if (descriptor.active_for_render) return &descriptor;
    }
    return nullptr;
}

} // namespace carto::attributes
