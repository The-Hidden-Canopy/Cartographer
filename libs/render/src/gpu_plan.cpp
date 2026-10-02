#include <carto/render/gpu_plan.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace carto::render {

namespace {

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

render_graph::ResourceDesc texture(
    std::string name,
    std::uint32_t width,
    std::uint32_t height,
    gpu::Format format,
    bool imported = false) {
    return render_graph::ResourceDesc{
        std::move(name),
        render_graph::ResourceKind::texture,
        imported,
        0U,
        0U,
        width,
        height,
        1U,
        1U,
        1U,
        format,
    };
}

bool contains(const RenderRequest& request, CaptureResource capture) {
    return std::find(request.captures.begin(), request.captures.end(), capture) !=
        request.captures.end();
}

} // namespace

core::Result<GpuRenderPlan> prepare_gpu_render_plan(
    const RenderRequest& request,
    bool imported_present_target,
    bool history_available) {
    if (auto result = request.validate(); !result) {
        return core::Result<GpuRenderPlan>::failure(result.error());
    }
    if (imported_present_target && request.output != OutputFormat::png) {
        return core::Result<GpuRenderPlan>::failure(validation(
            "an imported presentation target requires the display PNG surface"));
    }
    if (!history_available && request.quality == RenderQualityProfile::reference) {
        return core::Result<GpuRenderPlan>::failure(validation(
            "reference GPU rendering requires an explicit temporal history surface"));
    }

    const auto settings = request.resolve();
    if (!settings) return core::Result<GpuRenderPlan>::failure(settings.error());

    render_graph::GraphBuilder graph;
    const auto shadow = graph.create_resource(texture(
        "shadow_depth", settings.value().shadow_map_size,
        settings.value().shadow_map_size, gpu::Format::depth32_float));
    const auto hdr = graph.create_resource(texture(
        "linear_hdr_color", request.extent.width, request.extent.height,
        gpu::Format::rgba16_float));
    const auto depth = graph.create_resource(texture(
        "scene_depth", request.extent.width, request.extent.height,
        gpu::Format::depth32_float));
    const auto motion = graph.create_resource(texture(
        "motion_vectors", request.extent.width, request.extent.height,
        gpu::Format::rg16_float));
    const auto temporal = graph.create_resource(texture(
        "temporal_color", request.extent.width, request.extent.height,
        gpu::Format::rgba16_float));
    const auto history = graph.create_resource(texture(
        "history_color", request.extent.width, request.extent.height,
        gpu::Format::rgba16_float, history_available));
    const auto output = graph.create_resource(texture(
        imported_present_target ? "present_target" : "tone_mapped_color",
        request.extent.width,
        request.extent.height,
        imported_present_target ? gpu::Format::bgra8_unorm : gpu::Format::rgba8_unorm,
        imported_present_target));
    if (!shadow || !hdr || !depth || !motion || !temporal || !history || !output) {
        const auto& error = !shadow ? shadow.error() : !hdr ? hdr.error() :
            !depth ? depth.error() : !motion ? motion.error() :
            !temporal ? temporal.error() : !history ? history.error() : output.error();
        return core::Result<GpuRenderPlan>::failure(error);
    }

    auto kernel_contract = [](
        std::string kernel_id,
        std::string operation,
        device::PrecisionRole role,
        device::KernelDirection direction,
        std::string representation) {
        return device::KernelContract{
            std::move(kernel_id),
            std::move(operation),
            role,
            direction,
            std::move(representation),
            "fp32",
            std::nullopt,
            false,
        };
    };

    std::optional<render_graph::ResourceId> normal;
    if (contains(request, CaptureResource::normals)) {
        const auto resource = graph.create_resource(texture(
            "world_normals", request.extent.width, request.extent.height,
            gpu::Format::rgba16_float));
        if (!resource) return core::Result<GpuRenderPlan>::failure(resource.error());
        normal = resource.value();
    }
    std::optional<render_graph::ResourceId> object_id;
    if (contains(request, CaptureResource::object_id)) {
        const auto resource = graph.create_resource(texture(
            "object_id", request.extent.width, request.extent.height,
            gpu::Format::r32_uint));
        if (!resource) return core::Result<GpuRenderPlan>::failure(resource.error());
        object_id = resource.value();
    }

    std::vector<GpuKernelPlan> kernels;
    auto add_pass = [&graph, &kernels](
        std::string name,
        std::vector<render_graph::ResourceUsage> usages,
        device::KernelContract contract)
        -> core::Result<render_graph::PassId> {
        if (auto result = device::validate(contract); !result) {
            return core::Result<render_graph::PassId>::failure(result.error());
        }
        auto pass = graph.add_pass(render_graph::PassDesc{std::move(name), std::move(usages)});
        if (pass) kernels.push_back(GpuKernelPlan{pass.value(), std::move(contract)});
        return pass;
    };
    if (auto pass = add_pass("shadow_map", {
            {shadow.value(), render_graph::Access::write,
             render_graph::PipelineStage::depth_stencil},
        }, kernel_contract(
            "cartographer.render.shadow_map",
            "depth shadow raster",
            device::PrecisionRole::arithmetic_input,
            device::KernelDirection::raster,
            "depth32f")); !pass) {
        return core::Result<GpuRenderPlan>::failure(pass.error());
    }

    std::vector<render_graph::ResourceUsage> opaque = {
        {shadow.value(), render_graph::Access::read, render_graph::PipelineStage::fragment},
        {hdr.value(), render_graph::Access::write, render_graph::PipelineStage::color_output},
        {depth.value(), render_graph::Access::write,
         render_graph::PipelineStage::depth_stencil},
        {motion.value(), render_graph::Access::write,
         render_graph::PipelineStage::color_output},
    };
    if (normal.has_value()) {
        opaque.push_back({*normal, render_graph::Access::write,
                          render_graph::PipelineStage::color_output});
    }
    if (object_id.has_value()) {
        opaque.push_back({*object_id, render_graph::Access::write,
                          render_graph::PipelineStage::color_output});
    }
    if (auto pass = add_pass(
            "opaque_pbr",
            std::move(opaque),
            kernel_contract(
                "cartographer.render.opaque_pbr",
                "opaque material resolve",
                device::PrecisionRole::arithmetic_input,
                device::KernelDirection::raster,
                "pbr_constants")); !pass) {
        return core::Result<GpuRenderPlan>::failure(pass.error());
    }

    std::vector<render_graph::ResourceUsage> temporal_usages = {
        {hdr.value(), render_graph::Access::read, render_graph::PipelineStage::fragment},
        {motion.value(), render_graph::Access::read,
         render_graph::PipelineStage::fragment},
        {temporal.value(), render_graph::Access::write,
         render_graph::PipelineStage::color_output},
    };
    if (history_available) {
        temporal_usages.insert(
            temporal_usages.begin() + 2,
            render_graph::ResourceUsage{
                history.value(), render_graph::Access::read,
                render_graph::PipelineStage::fragment});
    }
    if (auto pass = add_pass(
            "temporal_resolve",
            std::move(temporal_usages),
            kernel_contract(
                "cartographer.render.temporal_resolve",
                "history resolve",
                device::PrecisionRole::compressed_operand,
                device::KernelDirection::compute,
                "rgba16f_history")); !pass) {
        return core::Result<GpuRenderPlan>::failure(pass.error());
    }

    if (auto pass = add_pass("tone_map", {
            {temporal.value(), render_graph::Access::read,
             render_graph::PipelineStage::fragment},
            {output.value(), render_graph::Access::write,
             render_graph::PipelineStage::color_output},
        }, kernel_contract(
            "cartographer.render.tone_map",
            "HDR tone map",
            device::PrecisionRole::compressed_operand,
            device::KernelDirection::raster,
            "rgba16f_hdr")); !pass) {
        return core::Result<GpuRenderPlan>::failure(pass.error());
    }
    if (imported_present_target) {
        if (auto pass = add_pass("present", {
                {output.value(), render_graph::Access::write,
                 render_graph::PipelineStage::present},
            }, kernel_contract(
                "cartographer.render.present",
                "display surface presentation",
                device::PrecisionRole::output,
                device::KernelDirection::presentation,
                "bgra8_unorm")); !pass) {
            return core::Result<GpuRenderPlan>::failure(pass.error());
        }
    }

    auto compiled = graph.compile();
    if (!compiled) return core::Result<GpuRenderPlan>::failure(compiled.error());
    return core::Result<GpuRenderPlan>::success(GpuRenderPlan{
        std::move(compiled.value()),
        std::move(kernels),
        settings.value(),
        shadow.value(),
        hdr.value(),
        depth.value(),
        motion.value(),
        temporal.value(),
        output.value(),
        history_available ? std::optional<render_graph::ResourceId>{history.value()}
                          : std::nullopt,
        normal,
        object_id,
        imported_present_target,
    });
}

} // namespace carto::render
