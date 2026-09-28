#include <carto/gpu/handles.hpp>
#include <carto/gpu/lifetime.hpp>
#include <carto/gpu/rhi.hpp>
#include <carto/render/viewport.hpp>
#include <carto/render_graph/graph.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw TestFailure(std::string("requirement failed: ") + #condition);         \
        }                                                                                \
    } while (false)

using carto::core::ErrorCode;
using carto::gpu::Format;
using carto::render_graph::Access;
using carto::render_graph::GraphBuilder;
using carto::render_graph::PassDesc;
using carto::render_graph::PipelineStage;
using carto::render_graph::ResourceDesc;
using carto::render_graph::ResourceKind;
using carto::render_graph::ResourceUsage;

static_assert(!std::is_convertible_v<carto::gpu::BufferHandle, carto::gpu::TextureHandle>);
static_assert(sizeof(carto::gpu::BufferHandle) == sizeof(std::uint64_t));

void generational_registries_reject_stale_handles() {
    struct TestBufferTag;
    carto::gpu::Registry<TestBufferTag, int> registry;

    const auto first = registry.insert(7);
    REQUIRE(first);
    REQUIRE(registry.size() == 1U);
    const auto resolved = registry.resolve(first.value());
    REQUIRE(resolved);
    REQUIRE(*resolved.value() == 7);

    REQUIRE(registry.remove(first.value()));
    REQUIRE(registry.size() == 0U);
    const auto stale = registry.resolve(first.value());
    REQUIRE(!stale);
    REQUIRE(stale.error().code == ErrorCode::stale_data);

    const auto second = registry.insert(9);
    REQUIRE(second);
    REQUIRE(second.value().index == first.value().index);
    REQUIRE(second.value().generation != first.value().generation);
    REQUIRE(*registry.resolve(second.value()).value() == 9);
    REQUIRE(registry.remove(second.value()));
}

void deferred_destruction_waits_for_completed_submission() {
    carto::gpu::DeferredDestructionQueue queue;
    auto resource = std::make_shared<int>(42);
    const std::weak_ptr<int> observation = resource;
    REQUIRE(queue.retire(8U, resource));
    resource.reset();
    REQUIRE(queue.pending_count() == 1U);
    REQUIRE(queue.collect(7U) == 0U);
    REQUIRE(queue.pending_count() == 1U);
    REQUIRE(!observation.expired());
    REQUIRE(queue.collect(8U) == 1U);
    REQUIRE(queue.pending_count() == 0U);
    REQUIRE(observation.expired());
    REQUIRE(!queue.retire(9U, std::shared_ptr<int>{}));
}

ResourceDesc make_buffer(std::string name, std::uint32_t alias_group = 0U) {
    ResourceDesc descriptor;
    descriptor.name = std::move(name);
    descriptor.kind = ResourceKind::buffer;
    descriptor.alias_group = alias_group;
    descriptor.bytes = 1024U;
    return descriptor;
}

ResourceDesc make_texture(std::string name, bool imported, Format format,
                          std::uint32_t alias_group = 0U) {
    ResourceDesc descriptor;
    descriptor.name = std::move(name);
    descriptor.kind = ResourceKind::texture;
    descriptor.imported = imported;
    descriptor.alias_group = alias_group;
    descriptor.width = 640U;
    descriptor.height = 480U;
    descriptor.depth = 1U;
    descriptor.layers = 1U;
    descriptor.mip_levels = 1U;
    descriptor.format = format;
    return descriptor;
}

void valid_graph_compiles_with_write_barriers() {
    GraphBuilder graph;
    const auto color = graph.create_resource(
        make_texture("swapchain", true, Format::rgba8_unorm));
    const auto vertices = graph.create_resource(make_buffer("vertices"));
    REQUIRE(color);
    REQUIRE(vertices);

    REQUIRE(graph.add_pass(PassDesc{
        "upload",
        {ResourceUsage{vertices.value(), Access::write, PipelineStage::copy}},
    }));
    REQUIRE(graph.add_pass(PassDesc{
        "draw",
        {
            ResourceUsage{vertices.value(), Access::read, PipelineStage::vertex},
            ResourceUsage{color.value(), Access::write, PipelineStage::color_output},
        },
    }));
    REQUIRE(graph.add_pass(PassDesc{
        "present",
        {ResourceUsage{color.value(), Access::write, PipelineStage::present}},
    }));

    const auto compiled = graph.compile();
    REQUIRE(compiled);
    REQUIRE(compiled.value().pass_order.size() == 3U);
    REQUIRE(compiled.value().barriers.size() == 2U);
    REQUIRE(compiled.value().lifetimes.size() == 2U);
}

void graph_rejects_invalid_descriptors_and_hazards() {
    GraphBuilder invalid_descriptor_graph;
    auto invalid_buffer = make_buffer("invalid");
    invalid_buffer.format = Format::rgba8_unorm;
    const auto invalid_resource = invalid_descriptor_graph.create_resource(invalid_buffer);
    REQUIRE(!invalid_resource);
    REQUIRE(invalid_resource.error().code == ErrorCode::invalid_argument);

    GraphBuilder read_before_write_graph;
    const auto transient = read_before_write_graph.create_resource(make_buffer("transient"));
    REQUIRE(transient);
    REQUIRE(read_before_write_graph.add_pass(PassDesc{
        "read too early",
        {ResourceUsage{transient.value(), Access::read, PipelineStage::vertex}},
    }));
    const auto read_before_write = read_before_write_graph.compile();
    REQUIRE(!read_before_write);
    REQUIRE(read_before_write.error().code == ErrorCode::validation_failed);

    GraphBuilder duplicate_usage_graph;
    const auto duplicate_resource = duplicate_usage_graph.create_resource(make_buffer("duplicate"));
    REQUIRE(duplicate_resource);
    const auto duplicate_pass = duplicate_usage_graph.add_pass(PassDesc{
        "duplicate usage",
        {
            ResourceUsage{duplicate_resource.value(), Access::write, PipelineStage::copy},
            ResourceUsage{duplicate_resource.value(), Access::read, PipelineStage::vertex},
        },
    });
    REQUIRE(!duplicate_pass);
    REQUIRE(duplicate_pass.error().code == ErrorCode::validation_failed);

    GraphBuilder stage_graph;
    const auto depth = stage_graph.create_resource(
        make_texture("depth", false, Format::depth24_stencil8));
    REQUIRE(depth);
    const auto wrong_stage = stage_graph.add_pass(PassDesc{
        "wrong stage",
        {ResourceUsage{depth.value(), Access::write, PipelineStage::color_output}},
    });
    REQUIRE(!wrong_stage);
    REQUIRE(wrong_stage.error().code == ErrorCode::validation_failed);
}

void graph_rejects_overlapping_alias_lifetimes() {
    GraphBuilder graph;
    const auto first = graph.create_resource(make_buffer("first", 11U));
    const auto second = graph.create_resource(make_buffer("second", 11U));
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(graph.add_pass(PassDesc{
        "write first",
        {ResourceUsage{first.value(), Access::write, PipelineStage::compute}},
    }));
    REQUIRE(graph.add_pass(PassDesc{
        "overlap",
        {
            ResourceUsage{first.value(), Access::read, PipelineStage::compute},
            ResourceUsage{second.value(), Access::write, PipelineStage::compute},
        },
    }));

    const auto compiled = graph.compile();
    REQUIRE(!compiled);
    REQUIRE(compiled.error().code == ErrorCode::validation_failed);
}

void offscreen_viewport_tracks_generation_and_declares_canonical_targets() {
    const auto invalid = carto::render::OffscreenViewport::create({});
    REQUIRE(!invalid);
    REQUIRE(invalid.error().code == ErrorCode::invalid_argument);

    const auto created = carto::render::OffscreenViewport::create({
        .width = 640U,
        .height = 480U,
        .selection_id = true,
        .normal = true,
    });
    REQUIRE(created);
    auto viewport = created.value();
    REQUIRE(viewport.generation().value == 1U);

    const auto unchanged = viewport.resize(viewport.description());
    REQUIRE(unchanged);
    REQUIRE(!unchanged.value().changed);
    REQUIRE(unchanged.value().before == unchanged.value().after);

    const auto resized = viewport.resize({
        .width = 1280U,
        .height = 720U,
        .selection_id = true,
        .normal = false,
    });
    REQUIRE(resized);
    REQUIRE(resized.value().changed);
    REQUIRE(resized.value().before.value == 1U);
    REQUIRE(resized.value().after.value == 2U);
    REQUIRE(viewport.description().width == 1280U);
    REQUIRE(viewport.description().height == 720U);

    GraphBuilder graph;
    const auto resources = viewport.declare_resources(graph);
    REQUIRE(resources);
    REQUIRE(resources.value().color);
    REQUIRE(resources.value().depth);
    REQUIRE(resources.value().selection_id.has_value());
    REQUIRE(!resources.value().normal.has_value());
    REQUIRE(graph.resource_count() == 3U);
}

} // namespace

int main() {
    try {
        generational_registries_reject_stale_handles();
        deferred_destruction_waits_for_completed_submission();
        valid_graph_compiles_with_write_barriers();
        graph_rejects_invalid_descriptors_and_hazards();
        graph_rejects_overlapping_alias_lifetimes();
        offscreen_viewport_tracks_generation_and_declares_canonical_targets();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
