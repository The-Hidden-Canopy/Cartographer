#pragma once

#include <carto/core/result.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/render_graph/graph.hpp>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace carto::d3d12::detail {

// Shared pass-order and resource-transition dispatch for D3D12 graph clients.
// Pass commands remain owned by the caller and are appended after record_pass.
class RenderGraphDispatch final {
public:
    using ResourceBindings =
        std::map<render_graph::ResourceId, device_ir::ResourceHandle>;
    using InitialStates =
        std::map<render_graph::ResourceId, device_ir::ResourceState>;
    using PassDescriptions =
        std::map<render_graph::PassId, render_graph::PassDesc>;

    [[nodiscard]] static core::Result<RenderGraphDispatch> create(
        render_graph::CompiledGraph compiled,
        PassDescriptions passes,
        ResourceBindings resources,
        InitialStates initial_states = {},
        std::set<render_graph::ResourceId> index_resources = {});

    [[nodiscard]] core::Result<void> record_pass(
        render_graph::PassId pass,
        device_ir::DeviceCommandStream& stream);

    [[nodiscard]] core::Result<void> finish() const;

    [[nodiscard]] const std::vector<std::string>& recorded_passes() const noexcept {
        return recorded_passes_;
    }

private:
    RenderGraphDispatch(
        render_graph::CompiledGraph compiled,
        PassDescriptions passes,
        ResourceBindings resources,
        InitialStates states,
        std::set<render_graph::ResourceId> index_resources);

    [[nodiscard]] static core::Result<device_ir::ResourceState> state_for(
        const render_graph::ResourceUsage& usage,
        const device_ir::ResourceHandle& resource,
        const std::set<render_graph::ResourceId>& index_resources);

    render_graph::CompiledGraph compiled_;
    PassDescriptions passes_;
    ResourceBindings resources_;
    InitialStates states_;
    std::set<render_graph::ResourceId> index_resources_;
    std::vector<std::string> recorded_passes_;
    std::size_t next_pass_ = 0U;
};

} // namespace carto::d3d12::detail
