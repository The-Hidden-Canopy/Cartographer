#pragma once

#include <carto/core/result.hpp>
#include <carto/device/kernel_evidence.hpp>
#include <carto/render/render_request.hpp>
#include <carto/render_graph/graph.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace carto::render {

struct GpuKernelPlan {
    render_graph::PassId pass;
    device::KernelContract contract;
};

// Backend-neutral GPU pass preparation. The plan owns graph semantics only;
// a D3D12/Vulkan executor is responsible for allocating the resources and
// binding the shader ABI declared in gpu_abi.hpp.
struct GpuRenderPlan {
    render_graph::CompiledGraph graph;
    std::vector<GpuKernelPlan> kernels;
    RenderQualitySettings settings;
    render_graph::ResourceId shadow_depth;
    render_graph::ResourceId hdr_color;
    render_graph::ResourceId depth;
    render_graph::ResourceId motion;
    render_graph::ResourceId temporal_color;
    render_graph::ResourceId output;
    std::optional<render_graph::ResourceId> history;
    std::optional<render_graph::ResourceId> normal;
    std::optional<render_graph::ResourceId> object_id;
    bool presents = false;
};

[[nodiscard]] core::Result<GpuRenderPlan> prepare_gpu_render_plan(
    const RenderRequest& request,
    bool imported_present_target,
    bool history_available);

} // namespace carto::render
