#include <carto/d3d12/detail/render_graph_dispatch.hpp>

#include <utility>

namespace carto::d3d12::detail {
namespace {

core::Diagnostic invalid_graph(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic unsupported_graph(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool valid_handle(const device_ir::ResourceHandle& resource) noexcept {
    return std::visit([](const auto handle) {
        return static_cast<bool>(handle);
    }, resource);
}

bool valid_state(device_ir::ResourceState state) noexcept {
    using device_ir::ResourceState;
    switch (state) {
    case ResourceState::undefined:
    case ResourceState::copy_source:
    case ResourceState::copy_destination:
    case ResourceState::vertex_read:
    case ResourceState::index_read:
    case ResourceState::constant_read:
    case ResourceState::shader_read:
    case ResourceState::shader_write:
    case ResourceState::render_target:
    case ResourceState::depth_write:
    case ResourceState::depth_read:
    case ResourceState::present:
        return true;
    }
    return false;
}

bool state_matches_resource_type(
    device_ir::ResourceState state,
    const device_ir::ResourceHandle& resource) noexcept {
    using device_ir::ResourceState;
    switch (state) {
    case ResourceState::vertex_read:
    case ResourceState::index_read:
    case ResourceState::constant_read:
        return std::holds_alternative<gpu::BufferHandle>(resource);
    case ResourceState::render_target:
    case ResourceState::depth_write:
    case ResourceState::depth_read:
    case ResourceState::present:
        return std::holds_alternative<gpu::TextureHandle>(resource);
    case ResourceState::undefined:
    case ResourceState::copy_source:
    case ResourceState::copy_destination:
    case ResourceState::shader_read:
    case ResourceState::shader_write:
        return true;
    }
    return false;
}

} // namespace

RenderGraphDispatch::RenderGraphDispatch(
    render_graph::CompiledGraph compiled,
    PassDescriptions passes,
    ResourceBindings resources,
    InitialStates states,
    std::set<render_graph::ResourceId> index_resources)
    : compiled_(std::move(compiled)),
      passes_(std::move(passes)),
      resources_(std::move(resources)),
      states_(std::move(states)),
      index_resources_(std::move(index_resources)) {
    recorded_passes_.reserve(compiled_.pass_order.size());
}

core::Result<RenderGraphDispatch> RenderGraphDispatch::create(
    render_graph::CompiledGraph compiled,
    PassDescriptions passes,
    ResourceBindings resources,
    InitialStates initial_states,
    std::set<render_graph::ResourceId> index_resources) {
    if (compiled.pass_order.empty() || passes.size() != compiled.pass_order.size()) {
        return core::Result<RenderGraphDispatch>::failure(invalid_graph(
            "D3D12 graph dispatch requires one description for every compiled pass"));
    }

    std::set<render_graph::PassId> unique_passes;
    for (const auto pass_id : compiled.pass_order) {
        if (!pass_id || !unique_passes.insert(pass_id).second ||
            !passes.contains(pass_id)) {
            return core::Result<RenderGraphDispatch>::failure(invalid_graph(
                "D3D12 graph dispatch contains a missing or repeated compiled pass"));
        }
        for (const auto& usage : passes.at(pass_id).usages) {
            if (!usage.resource || !resources.contains(usage.resource)) {
                return core::Result<RenderGraphDispatch>::failure(invalid_graph(
                    "D3D12 graph dispatch references an unbound resource"));
            }
        }
    }

    for (const auto& [resource_id, resource] : resources) {
        if (!resource_id || !valid_handle(resource)) {
            return core::Result<RenderGraphDispatch>::failure(invalid_graph(
                "D3D12 graph dispatch contains an invalid resource binding"));
        }
    }
    for (const auto& [resource_id, state] : initial_states) {
        const auto binding = resources.find(resource_id);
        if (binding == resources.end() || !valid_state(state) ||
            !state_matches_resource_type(state, binding->second)) {
            return core::Result<RenderGraphDispatch>::failure(invalid_graph(
                "D3D12 graph dispatch initial state is invalid for its resource binding"));
        }
    }
    for (const auto resource_id : index_resources) {
        const auto binding = resources.find(resource_id);
        if (binding == resources.end() ||
            !std::holds_alternative<gpu::BufferHandle>(binding->second)) {
            return core::Result<RenderGraphDispatch>::failure(invalid_graph(
                "D3D12 index resource must bind a live buffer"));
        }
    }

    return core::Result<RenderGraphDispatch>::success(RenderGraphDispatch(
        std::move(compiled), std::move(passes), std::move(resources),
        std::move(initial_states), std::move(index_resources)));
}

core::Result<device_ir::ResourceState> RenderGraphDispatch::state_for(
    const render_graph::ResourceUsage& usage,
    const device_ir::ResourceHandle& resource,
    const std::set<render_graph::ResourceId>& index_resources) {
    using render_graph::Access;
    using render_graph::PipelineStage;
    switch (usage.access) {
    case Access::read:
    case Access::write:
    case Access::read_write:
        break;
    default:
        return core::Result<device_ir::ResourceState>::failure(invalid_graph(
            "D3D12 graph usage has an invalid access mode"));
    }
    switch (usage.stage) {
    case PipelineStage::copy:
        if (usage.access == Access::read) {
            return core::Result<device_ir::ResourceState>::success(
                device_ir::ResourceState::copy_source);
        }
        if (usage.access == Access::write) {
            return core::Result<device_ir::ResourceState>::success(
                device_ir::ResourceState::copy_destination);
        }
        return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
            "D3D12 copy-stage graph usage must be read-only or write-only"));
    case PipelineStage::compute:
    case PipelineStage::fragment:
        return core::Result<device_ir::ResourceState>::success(
            usage.access == Access::read
                ? device_ir::ResourceState::shader_read
                : device_ir::ResourceState::shader_write);
    case PipelineStage::vertex:
        if (usage.access != Access::read) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 vertex-stage graph usage must be read-only"));
        }
        if (!std::holds_alternative<gpu::BufferHandle>(resource)) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 vertex-stage graph usage must bind a buffer"));
        }
        return core::Result<device_ir::ResourceState>::success(
            index_resources.contains(usage.resource)
                ? device_ir::ResourceState::index_read
                : device_ir::ResourceState::vertex_read);
    case PipelineStage::color_output:
        if (!std::holds_alternative<gpu::TextureHandle>(resource)) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 color-output graph usage must bind a texture"));
        }
        if (usage.access == Access::read) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 color-output graph usage must write the render target"));
        }
        return core::Result<device_ir::ResourceState>::success(
            device_ir::ResourceState::render_target);
    case PipelineStage::depth_stencil:
        if (!std::holds_alternative<gpu::TextureHandle>(resource)) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 depth-stencil graph usage must bind a texture"));
        }
        if (usage.access == Access::read) {
            return core::Result<device_ir::ResourceState>::success(
                device_ir::ResourceState::depth_read);
        }
        if (usage.access == Access::write) {
            return core::Result<device_ir::ResourceState>::success(
                device_ir::ResourceState::depth_write);
        }
        return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
            "D3D12 depth-stencil graph usage cannot combine read and write access"));
    case PipelineStage::present:
        if (!std::holds_alternative<gpu::TextureHandle>(resource)) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 present-stage graph usage must bind a texture"));
        }
        if (usage.access != Access::write) {
            return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
                "D3D12 present-stage graph usage must be write-only"));
        }
        return core::Result<device_ir::ResourceState>::success(
            device_ir::ResourceState::present);
    }
    return core::Result<device_ir::ResourceState>::failure(unsupported_graph(
        "D3D12 graph dispatch encountered an unknown pipeline stage"));
}

core::Result<void> RenderGraphDispatch::record_pass(
    render_graph::PassId pass_id,
    device_ir::DeviceCommandStream& stream) {
    if (next_pass_ >= compiled_.pass_order.size() ||
        compiled_.pass_order[next_pass_] != pass_id) {
        return core::Result<void>::failure(invalid_graph(
            "D3D12 command recording diverged from compiled graph order"));
    }
    const auto pass = passes_.find(pass_id);
    if (pass == passes_.end()) {
        return core::Result<void>::failure(invalid_graph(
            "D3D12 compiled graph pass has no command description"));
    }

    struct Transition {
        device_ir::ResourceHandle resource;
        device_ir::ResourceState before;
        device_ir::ResourceState after;
    };
    std::vector<Transition> transitions;
    transitions.reserve(pass->second.usages.size());
    InitialStates next_states = states_;
    for (const auto& usage : pass->second.usages) {
        const auto binding = resources_.find(usage.resource);
        if (binding == resources_.end()) {
            return core::Result<void>::failure(invalid_graph(
                "D3D12 graph pass references an unbound resource"));
        }
        const auto after = state_for(usage, binding->second, index_resources_);
        if (!after) return core::Result<void>::failure(after.error());
        const auto current = next_states.contains(usage.resource)
            ? next_states.at(usage.resource) : device_ir::ResourceState::undefined;
        if (current != after.value()) {
            transitions.push_back({binding->second, current, after.value()});
        }
        next_states[usage.resource] = after.value();
    }
    std::string pass_name = pass->second.name;

    for (const auto& transition : transitions) {
        stream.append(device_ir::CmdTransitionResource{
            transition.resource, transition.before, transition.after});
    }
    states_ = std::move(next_states);
    recorded_passes_.push_back(std::move(pass_name));
    ++next_pass_;
    return core::Result<void>::success();
}

core::Result<void> RenderGraphDispatch::finish() const {
    if (next_pass_ != compiled_.pass_order.size()) {
        return core::Result<void>::failure(invalid_graph(
            "D3D12 command recording omitted a compiled graph pass"));
    }
    return core::Result<void>::success();
}

} // namespace carto::d3d12::detail
