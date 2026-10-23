#pragma once

#include <carto/core/result.hpp>
#include <carto/gpu/rhi.hpp>
#include <carto/render_graph/graph.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace carto::render {

struct ViewportDescription {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    gpu::Format color_format = gpu::Format::rgba8_unorm;
    gpu::Format depth_format = gpu::Format::depth24_stencil8;
    bool selection_id = false;
    bool normal = false;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] constexpr bool operator==(const ViewportDescription&) const noexcept = default;
};

struct ViewportGeneration {
    std::uint64_t value = 0U;

    [[nodiscard]] constexpr auto operator<=>(const ViewportGeneration&) const noexcept = default;
};

struct ViewportResizeReceipt {
    ViewportGeneration before;
    ViewportGeneration after;
    bool changed = false;
};

struct ViewportGraphResources {
    render_graph::ResourceId color;
    render_graph::ResourceId depth;
    std::optional<render_graph::ResourceId> selection_id;
    std::optional<render_graph::ResourceId> normal;
};

// Backend-neutral description of the canonical offscreen viewport. It owns
// no GPU object; the generation is the invalidation boundary a backend uses
// when replacing its transient render targets after a resize or attachment
// policy change.
class OffscreenViewport {
public:
    [[nodiscard]] static core::Result<OffscreenViewport> create(ViewportDescription description);

    [[nodiscard]] core::Result<ViewportResizeReceipt> resize(ViewportDescription description);
    [[nodiscard]] core::Result<ViewportGraphResources> declare_resources(
        render_graph::GraphBuilder& graph) const;

    [[nodiscard]] const ViewportDescription& description() const noexcept { return description_; }
    [[nodiscard]] ViewportGeneration generation() const noexcept { return generation_; }

private:
    explicit OffscreenViewport(ViewportDescription description) : description_(description) {}

    ViewportDescription description_;
    ViewportGeneration generation_{1U};
};

} // namespace carto::render
