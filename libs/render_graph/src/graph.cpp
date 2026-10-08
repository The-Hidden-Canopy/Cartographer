#include <carto/render_graph/graph.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace carto::render_graph {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

bool is_write(Access access) {
    return access == Access::write || access == Access::read_write;
}

} // namespace

core::Result<void> GraphBuilder::validate_resource(const ResourceDesc& descriptor) const {
    if (descriptor.name.empty()) {
        return core::Result<void>::failure(invalid("render-graph resource name must not be empty"));
    }
    if (descriptor.kind == ResourceKind::buffer) {
        if (descriptor.bytes == 0U || descriptor.format != gpu::Format::unknown ||
            descriptor.width != 0U || descriptor.height != 0U || descriptor.depth != 0U ||
            descriptor.layers != 0U || descriptor.mip_levels != 0U) {
            return core::Result<void>::failure(
                invalid("buffer resource descriptor contains incompatible texture fields"));
        }
        return core::Result<void>::success();
    }
    if (descriptor.kind != ResourceKind::texture || descriptor.width == 0U ||
        descriptor.height == 0U || descriptor.depth == 0U || descriptor.layers == 0U ||
        descriptor.mip_levels == 0U || descriptor.format == gpu::Format::unknown ||
        descriptor.bytes != 0U) {
        return core::Result<void>::failure(
            invalid("texture resource descriptor is incomplete or contains buffer fields"));
    }
    return core::Result<void>::success();
}

core::Result<void> GraphBuilder::validate_usage(const ResourceUsage& usage) const {
    const auto resource = resources_.find(usage.resource);
    if (resource == resources_.end()) {
        return core::Result<void>::failure(
            Diagnostic(ErrorCode::not_found, "render-graph usage references a missing resource"));
    }
    switch (usage.access) {
    case Access::read:
    case Access::write:
    case Access::read_write:
        break;
    default:
        return core::Result<void>::failure(invalid("render-graph usage has an invalid access mode"));
    }
    switch (usage.stage) {
    case PipelineStage::color_output:
        if (resource->second.kind != ResourceKind::texture ||
            gpu::is_depth_format(resource->second.format)) {
            return core::Result<void>::failure(
                validation("color-output usage requires a non-depth texture"));
        }
        break;
    case PipelineStage::depth_stencil:
        if (resource->second.kind != ResourceKind::texture ||
            !gpu::is_depth_format(resource->second.format)) {
            return core::Result<void>::failure(
                validation("depth-stencil usage requires a depth texture"));
        }
        break;
    case PipelineStage::present:
        if (resource->second.kind != ResourceKind::texture || !resource->second.imported ||
            usage.access == Access::read) {
            return core::Result<void>::failure(
                validation("present usage requires an imported writable texture"));
        }
        break;
    case PipelineStage::copy:
    case PipelineStage::compute:
    case PipelineStage::vertex:
    case PipelineStage::fragment:
        break;
    }
    return core::Result<void>::success();
}

core::Result<ResourceId> GraphBuilder::create_resource(ResourceDesc descriptor) {
    if (auto result = validate_resource(descriptor); !result) {
        return core::Result<ResourceId>::failure(result.error());
    }
    if (next_resource_id_ == std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<ResourceId>::failure(Diagnostic(
            ErrorCode::invalid_state, "render-graph resource id space is exhausted"));
    }
    const ResourceId id{next_resource_id_++};
    resources_.emplace(id, std::move(descriptor));
    return core::Result<ResourceId>::success(id);
}

core::Result<PassId> GraphBuilder::add_pass(PassDesc descriptor) {
    if (descriptor.name.empty()) {
        return core::Result<PassId>::failure(invalid("render-graph pass name must not be empty"));
    }
    if (descriptor.usages.empty()) {
        return core::Result<PassId>::failure(
            invalid("render-graph pass must declare at least one resource usage"));
    }
    std::set<ResourceId> seen;
    for (const auto& usage : descriptor.usages) {
        if (!seen.insert(usage.resource).second) {
            return core::Result<PassId>::failure(
                validation("render-graph pass uses one resource more than once"));
        }
        if (auto result = validate_usage(usage); !result) {
            return core::Result<PassId>::failure(result.error());
        }
    }
    if (next_pass_id_ == std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<PassId>::failure(Diagnostic(
            ErrorCode::invalid_state, "render-graph pass id space is exhausted"));
    }
    const PassId id{next_pass_id_++};
    passes_.emplace(id, std::move(descriptor));
    return core::Result<PassId>::success(id);
}

core::Result<CompiledGraph> GraphBuilder::compile() const {
    CompiledGraph compiled;
    compiled.pass_order.reserve(passes_.size());
    std::map<ResourceId, std::size_t> first_use;
    std::map<ResourceId, std::size_t> last_use;
    std::map<ResourceId, ResourceUsage> previous_usage;
    std::map<ResourceId, PassId> previous_pass;
    std::map<ResourceId, bool> initialized;
    for (const auto& [id, descriptor] : resources_) {
        static_cast<void>(descriptor);
        initialized.emplace(id, false);
    }
    for (const auto& [pass_id, pass] : passes_) {
        const std::size_t pass_index = compiled.pass_order.size();
        compiled.pass_order.push_back(pass_id);
        for (const auto& usage : pass.usages) {
            const auto resource = resources_.find(usage.resource);
            if (resource == resources_.end()) {
                return core::Result<CompiledGraph>::failure(
                    Diagnostic(ErrorCode::not_found, "render-graph pass references a missing resource"));
            }
            if (auto result = validate_usage(usage); !result) {
                return core::Result<CompiledGraph>::failure(result.error());
            }
            if (first_use.emplace(usage.resource, pass_index).second) {
                initialized[usage.resource] = resource->second.imported;
            }
            if (!initialized.at(usage.resource) && usage.access != Access::write) {
                return core::Result<CompiledGraph>::failure(validation(
                    "render-graph rejects read-before-write for resource " + resource->second.name));
            }
            const auto prior = previous_usage.find(usage.resource);
            if (prior != previous_usage.end() &&
                (is_write(prior->second.access) || is_write(usage.access))) {
                compiled.barriers.push_back(Barrier{
                    usage.resource,
                    previous_pass.at(usage.resource),
                    pass_id,
                    prior->second.access,
                    usage.access,
                    prior->second.stage,
                    usage.stage,
                });
            }
            initialized[usage.resource] = initialized.at(usage.resource) || is_write(usage.access);
            last_use[usage.resource] = pass_index;
            previous_usage[usage.resource] = usage;
            previous_pass[usage.resource] = pass_id;
        }
    }
    for (const auto& [resource, first] : first_use) {
        compiled.lifetimes.push_back(ResourceLifetime{resource, first, last_use.at(resource)});
    }

    for (std::size_t left = 0U; left < compiled.lifetimes.size(); ++left) {
        const auto left_descriptor = resources_.at(compiled.lifetimes[left].resource);
        if (left_descriptor.alias_group == 0U) {
            continue;
        }
        for (std::size_t right = left + 1U; right < compiled.lifetimes.size(); ++right) {
            const auto right_descriptor = resources_.at(compiled.lifetimes[right].resource);
            if (left_descriptor.alias_group != right_descriptor.alias_group ||
                compiled.lifetimes[left].last_pass < compiled.lifetimes[right].first_pass ||
                compiled.lifetimes[right].last_pass < compiled.lifetimes[left].first_pass) {
                continue;
            }
            return core::Result<CompiledGraph>::failure(validation(
                "render-graph resources in one alias group have overlapping lifetimes"));
        }
    }
    return core::Result<CompiledGraph>::success(std::move(compiled));
}

} // namespace carto::render_graph
