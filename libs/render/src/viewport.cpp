#include <carto/render/viewport.hpp>

#include <limits>
#include <string>

namespace carto::render {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool is_color_format(gpu::Format format) {
    return format == gpu::Format::rgba8_unorm || format == gpu::Format::rgba8_srgb ||
           format == gpu::Format::bgra8_unorm || format == gpu::Format::bgra8_srgb ||
           format == gpu::Format::rgba16_float || format == gpu::Format::rgba32_float;
}

} // namespace

core::Result<void> ViewportDescription::validate() const {
    if (width == 0U || height == 0U) {
        return core::Result<void>::failure(invalid("offscreen viewport extent must be non-zero"));
    }
    if (!is_color_format(color_format)) {
        return core::Result<void>::failure(invalid("offscreen viewport color format is not renderable"));
    }
    if (depth_format != gpu::Format::depth24_stencil8 &&
        depth_format != gpu::Format::depth32_float) {
        return core::Result<void>::failure(
            invalid("offscreen viewport requires a supported depth format"));
    }
    return core::Result<void>::success();
}

core::Result<OffscreenViewport> OffscreenViewport::create(ViewportDescription description) {
    if (auto result = description.validate(); !result) {
        return core::Result<OffscreenViewport>::failure(result.error());
    }
    return core::Result<OffscreenViewport>::success(OffscreenViewport(description));
}

core::Result<ViewportResizeReceipt> OffscreenViewport::resize(ViewportDescription description) {
    if (auto result = description.validate(); !result) {
        return core::Result<ViewportResizeReceipt>::failure(result.error());
    }
    const ViewportGeneration before = generation_;
    if (description == description_) {
        return core::Result<ViewportResizeReceipt>::success({before, before, false});
    }
    if (generation_.value == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<ViewportResizeReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "offscreen viewport generation space is exhausted"));
    }
    description_ = description;
    ++generation_.value;
    return core::Result<ViewportResizeReceipt>::success({before, generation_, true});
}

core::Result<ViewportGraphResources> OffscreenViewport::declare_resources(
    render_graph::GraphBuilder& graph) const {
    const auto create_texture = [&](std::string name, gpu::Format format) {
        return graph.create_resource({
            .name = std::move(name),
            .kind = render_graph::ResourceKind::texture,
            .width = description_.width,
            .height = description_.height,
            .depth = 1U,
            .layers = 1U,
            .mip_levels = 1U,
            .format = format,
        });
    };

    const auto color = create_texture("viewport.color", description_.color_format);
    if (!color) return core::Result<ViewportGraphResources>::failure(color.error());
    const auto depth = create_texture("viewport.depth", description_.depth_format);
    if (!depth) return core::Result<ViewportGraphResources>::failure(depth.error());

    ViewportGraphResources resources{color.value(), depth.value(), std::nullopt, std::nullopt};
    if (description_.selection_id) {
        const auto selection = create_texture("viewport.selection_id", gpu::Format::r32_uint);
        if (!selection) return core::Result<ViewportGraphResources>::failure(selection.error());
        resources.selection_id = selection.value();
    }
    if (description_.normal) {
        const auto normal = create_texture("viewport.normal", gpu::Format::rgba16_float);
        if (!normal) return core::Result<ViewportGraphResources>::failure(normal.error());
        resources.normal = normal.value();
    }
    return core::Result<ViewportGraphResources>::success(std::move(resources));
}

} // namespace carto::render
