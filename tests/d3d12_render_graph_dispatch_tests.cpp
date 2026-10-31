#include <carto/d3d12/detail/render_graph_dispatch.hpp>
#include "../libs/d3d12/src/bent_normal_validation.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using carto::d3d12::detail::RenderGraphDispatch;
using carto::device_ir::CmdTransitionResource;
using carto::device_ir::DeviceCommandStream;
using carto::device_ir::ResourceState;
using carto::gpu::BufferHandle;
using carto::gpu::Format;
using carto::gpu::TextureHandle;
using carto::render_graph::Access;
using carto::render_graph::GraphBuilder;
using carto::render_graph::PassDesc;
using carto::render_graph::PipelineStage;
using carto::render_graph::ResourceDesc;
using carto::render_graph::ResourceKind;
using carto::render_graph::ResourceUsage;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    GraphBuilder builder;
    carto::render_graph::ResourceId color;
    carto::render_graph::ResourceId vertex;
    carto::render_graph::ResourceId index;
    carto::render_graph::PassId render;
    carto::render_graph::PassId copy;
    carto::render_graph::CompiledGraph compiled;
    RenderGraphDispatch::PassDescriptions passes;
    RenderGraphDispatch::ResourceBindings resources;
    std::set<carto::render_graph::ResourceId> index_resources;

    Fixture() {
        ResourceDesc color_desc;
        color_desc.name = "color";
        color_desc.kind = ResourceKind::texture;
        color_desc.width = 8U;
        color_desc.height = 8U;
        color_desc.depth = color_desc.layers = color_desc.mip_levels = 1U;
        color_desc.format = Format::rgba8_unorm;
        const auto color_result = builder.create_resource(color_desc);
        require(static_cast<bool>(color_result), "color resource fixture is valid");
        color = color_result.value();

        ResourceDesc vertex_desc;
        vertex_desc.name = "vertex";
        vertex_desc.kind = ResourceKind::buffer;
        vertex_desc.imported = true;
        vertex_desc.bytes = 48U;
        const auto vertex_result = builder.create_resource(vertex_desc);
        require(static_cast<bool>(vertex_result), "vertex buffer fixture is valid");
        vertex = vertex_result.value();

        ResourceDesc index_desc = vertex_desc;
        index_desc.name = "index";
        const auto index_result = builder.create_resource(index_desc);
        require(static_cast<bool>(index_result), "index buffer fixture is valid");
        index = index_result.value();

        const auto render_result = builder.add_pass({"render", {
            ResourceUsage{color, Access::write, PipelineStage::color_output},
            ResourceUsage{vertex, Access::read, PipelineStage::vertex},
            ResourceUsage{index, Access::read, PipelineStage::vertex}}});
        require(static_cast<bool>(render_result), "render pass fixture is valid");
        render = render_result.value();
        const auto copy_result = builder.add_pass({"copy", {
            ResourceUsage{color, Access::read, PipelineStage::copy}}});
        require(static_cast<bool>(copy_result), "copy pass fixture is valid");
        copy = copy_result.value();

        const auto compiled_result = builder.compile();
        require(static_cast<bool>(compiled_result), "fixture graph compiles");
        compiled = compiled_result.value();
        passes.emplace(render, PassDesc{"render", {
            ResourceUsage{color, Access::write, PipelineStage::color_output},
            ResourceUsage{vertex, Access::read, PipelineStage::vertex},
            ResourceUsage{index, Access::read, PipelineStage::vertex}}});
        passes.emplace(copy, PassDesc{"copy", {
            ResourceUsage{color, Access::read, PipelineStage::copy}}});
        resources.emplace(color, TextureHandle{1U, 1U});
        resources.emplace(vertex, BufferHandle{2U, 1U});
        resources.emplace(index, BufferHandle{3U, 1U});
        index_resources.insert(index);
    }
};

void dispatch_uses_compiled_order_and_resource_states() {
    Fixture fixture;
    auto created = RenderGraphDispatch::create(
        fixture.compiled, fixture.passes, fixture.resources, {}, fixture.index_resources);
    require(static_cast<bool>(created), "valid compiled graph bindings are admitted");
    auto dispatch = created.value();
    DeviceCommandStream stream;

    require(static_cast<bool>(dispatch.record_pass(fixture.render, stream)),
        "first compiled pass dispatches");
    require(static_cast<bool>(dispatch.record_pass(fixture.copy, stream)),
        "second compiled pass dispatches");
    require(static_cast<bool>(dispatch.finish()), "complete graph dispatch finishes");
    require(dispatch.recorded_passes() == std::vector<std::string>{"render", "copy"},
        "receipt names follow compiled execution order");
    require(stream.commands().size() == 4U,
        "only required state transitions are emitted");

    const auto* color_write = std::get_if<CmdTransitionResource>(&stream.commands()[0]);
    const auto* vertex_read = std::get_if<CmdTransitionResource>(&stream.commands()[1]);
    const auto* index_read = std::get_if<CmdTransitionResource>(&stream.commands()[2]);
    const auto* color_copy = std::get_if<CmdTransitionResource>(&stream.commands()[3]);
    require(color_write != nullptr && color_write->before == ResourceState::undefined &&
            color_write->after == ResourceState::render_target,
        "color output maps to the render-target state");
    require(vertex_read != nullptr && vertex_read->after == ResourceState::vertex_read,
        "vertex buffers map to the vertex-read state");
    require(index_read != nullptr && index_read->after == ResourceState::index_read,
        "indexed buffers map to the index-read state");
    require(color_copy != nullptr && color_copy->before == ResourceState::render_target &&
            color_copy->after == ResourceState::copy_source,
        "later copy reads transition from the prior pass state");
}

void invalid_order_and_unbound_resources_fail_before_dispatch() {
    Fixture fixture;
    auto created = RenderGraphDispatch::create(
        fixture.compiled, fixture.passes, fixture.resources, {}, fixture.index_resources);
    require(static_cast<bool>(created), "valid graph dispatcher is created");
    auto dispatch = created.value();
    DeviceCommandStream stream;
    require(!dispatch.record_pass(fixture.copy, stream),
        "out-of-order pass dispatch is refused");
    require(stream.commands().empty(), "wrong pass order appends no transitions");
    require(!dispatch.finish(), "omitted passes prevent successful completion");

    auto unbound = fixture.resources;
    unbound.erase(fixture.index);
    auto rejected = RenderGraphDispatch::create(
        fixture.compiled, fixture.passes, std::move(unbound), {}, fixture.index_resources);
    require(!rejected, "a graph with an unbound index resource is refused");

    auto invalid_states = fixture.resources;
    RenderGraphDispatch::InitialStates initial_states{
        {fixture.color, static_cast<ResourceState>(255)}};
    const auto invalid_state_rejected = RenderGraphDispatch::create(
        fixture.compiled, fixture.passes, std::move(invalid_states),
        std::move(initial_states), fixture.index_resources);
    require(!invalid_state_rejected,
        "invalid initial resource states are refused at creation");

    RenderGraphDispatch::InitialStates wrong_kind_states{
        {fixture.vertex, ResourceState::present}};
    const auto wrong_kind_rejected = RenderGraphDispatch::create(
        fixture.compiled, fixture.passes, fixture.resources,
        std::move(wrong_kind_states), fixture.index_resources);
    require(!wrong_kind_rejected,
        "initial states incompatible with handle kinds are refused");
}

void unsupported_pass_is_atomic_and_does_not_advance() {
    GraphBuilder builder;
    ResourceDesc color_desc;
    color_desc.name = "valid-color";
    color_desc.kind = ResourceKind::texture;
    color_desc.width = color_desc.height = 4U;
    color_desc.depth = color_desc.layers = color_desc.mip_levels = 1U;
    color_desc.format = Format::rgba8_unorm;
    const auto color = builder.create_resource(color_desc);
    require(static_cast<bool>(color), "color texture fixture is valid");
    ResourceDesc wrong_vertex_desc = color_desc;
    wrong_vertex_desc.name = "wrong-vertex-resource";
    wrong_vertex_desc.imported = true;
    const auto wrong_vertex = builder.create_resource(wrong_vertex_desc);
    require(static_cast<bool>(wrong_vertex), "imported texture fixture is valid");
    const auto pass = builder.add_pass({"invalid", {
        ResourceUsage{color.value(), Access::write, PipelineStage::color_output},
        ResourceUsage{wrong_vertex.value(), Access::read, PipelineStage::vertex}}});
    require(static_cast<bool>(pass), "unsupported pass fixture can be described");
    const auto compiled = builder.compile();
    require(static_cast<bool>(compiled), "unsupported pass graph still compiles generically");

    RenderGraphDispatch::PassDescriptions passes;
    passes.emplace(pass.value(), PassDesc{"invalid", {
        ResourceUsage{color.value(), Access::write, PipelineStage::color_output},
        ResourceUsage{wrong_vertex.value(), Access::read, PipelineStage::vertex}}});
    RenderGraphDispatch::ResourceBindings resources;
    resources.emplace(color.value(), TextureHandle{4U, 1U});
    resources.emplace(wrong_vertex.value(), TextureHandle{5U, 1U});
    auto created = RenderGraphDispatch::create(compiled.value(), std::move(passes),
        std::move(resources));
    require(static_cast<bool>(created), "bound graph fixture is admitted before dispatch");
    auto dispatch = created.value();
    DeviceCommandStream stream;
    require(!dispatch.record_pass(pass.value(), stream),
        "texture bound to vertex input is rejected");
    require(stream.commands().empty(),
        "unsupported usage does not emit earlier transitions from the same pass");
    require(!dispatch.finish(), "rejected pass does not advance the graph cursor");
}

void unsupported_stage_access_pairs_refuse_without_transitions() {
    struct Case {
        const char* name;
        ResourceKind kind;
        bool imported;
        Access access;
        PipelineStage stage;
        bool bind_buffer_to_texture = false;
    };
    const Case cases[]{
        {"vertex-write", ResourceKind::buffer, false, Access::write,
            PipelineStage::vertex},
        {"color-read", ResourceKind::texture, true, Access::read,
            PipelineStage::color_output},
        {"copy-read-write", ResourceKind::texture, true, Access::read_write,
            PipelineStage::copy},
        {"depth-read-write", ResourceKind::texture, true, Access::read_write,
            PipelineStage::depth_stencil},
        {"present-read-write", ResourceKind::texture, true, Access::read_write,
            PipelineStage::present},
        {"color-buffer-binding", ResourceKind::texture, false, Access::write,
            PipelineStage::color_output, true},
        {"depth-buffer-binding", ResourceKind::texture, false, Access::write,
            PipelineStage::depth_stencil, true},
        {"present-buffer-binding", ResourceKind::texture, true, Access::write,
            PipelineStage::present, true},
    };

    std::uint32_t handle_id = 10U;
    for (const auto& test_case : cases) {
        GraphBuilder builder;
        ResourceDesc resource_desc;
        resource_desc.name = test_case.name;
        resource_desc.kind = test_case.kind;
        resource_desc.imported = test_case.imported;
        if (test_case.kind == ResourceKind::buffer) {
            resource_desc.bytes = 16U;
        } else {
            resource_desc.width = resource_desc.height = 4U;
            resource_desc.depth = resource_desc.layers = resource_desc.mip_levels = 1U;
            resource_desc.format = test_case.stage == PipelineStage::depth_stencil
                ? Format::depth32_float : Format::rgba8_unorm;
        }
        const auto resource = builder.create_resource(resource_desc);
        require(static_cast<bool>(resource), "stage-access resource fixture is valid");
        const auto pass = builder.add_pass({test_case.name, {
            ResourceUsage{resource.value(), test_case.access, test_case.stage}}});
        require(static_cast<bool>(pass), "stage-access pass fixture is valid");
        const auto compiled = builder.compile();
        require(static_cast<bool>(compiled), "stage-access graph compiles generically");

        RenderGraphDispatch::PassDescriptions passes;
        passes.emplace(pass.value(), PassDesc{test_case.name, {
            ResourceUsage{resource.value(), test_case.access, test_case.stage}}});
        RenderGraphDispatch::ResourceBindings resources;
        if (test_case.kind == ResourceKind::buffer ||
            test_case.bind_buffer_to_texture) {
            resources.emplace(resource.value(), BufferHandle{handle_id++, 1U});
        } else {
            resources.emplace(resource.value(), TextureHandle{handle_id++, 1U});
        }
        auto created = RenderGraphDispatch::create(
            compiled.value(), std::move(passes), std::move(resources));
        require(static_cast<bool>(created),
            "dispatcher admits generic graph before backend-stage validation");
        auto dispatch = created.value();
        DeviceCommandStream stream;
        require(!dispatch.record_pass(pass.value(), stream),
            "unrepresentable stage-access pair is refused");
        require(stream.commands().empty(),
            "unrepresentable stage-access pair emits no transitions");
    }
}

void invalid_access_enum_refuses_without_transitions() {
    GraphBuilder builder;
    ResourceDesc color_desc;
    color_desc.name = "color";
    color_desc.kind = ResourceKind::texture;
    color_desc.width = color_desc.height = 4U;
    color_desc.depth = color_desc.layers = color_desc.mip_levels = 1U;
    color_desc.format = Format::rgba8_unorm;
    const auto color = builder.create_resource(color_desc);
    require(static_cast<bool>(color), "color fixture is valid");
    const auto pass = builder.add_pass({"invalid-access", {
        ResourceUsage{color.value(), Access::write, PipelineStage::color_output}}});
    require(static_cast<bool>(pass), "valid source pass can be compiled");
    const auto compiled = builder.compile();
    require(static_cast<bool>(compiled), "valid source graph compiles");

    RenderGraphDispatch::PassDescriptions passes;
    passes.emplace(pass.value(), PassDesc{"invalid-access", {
        ResourceUsage{color.value(), static_cast<Access>(255),
            PipelineStage::color_output}}});
    RenderGraphDispatch::ResourceBindings resources;
    resources.emplace(color.value(), TextureHandle{20U, 1U});
    auto created = RenderGraphDispatch::create(
        compiled.value(), std::move(passes), std::move(resources));
    require(static_cast<bool>(created), "bound graph reaches backend validation");
    auto dispatch = created.value();
    DeviceCommandStream stream;
    require(!dispatch.record_pass(pass.value(), stream),
        "invalid access enum is refused at backend dispatch");
    require(stream.commands().empty(), "invalid access emits no transitions");
}

void invalid_stage_enum_refuses_without_transitions() {
    GraphBuilder builder;
    ResourceDesc color_desc;
    color_desc.name = "color";
    color_desc.kind = ResourceKind::texture;
    color_desc.width = color_desc.height = 4U;
    color_desc.depth = color_desc.layers = color_desc.mip_levels = 1U;
    color_desc.format = Format::rgba8_unorm;
    const auto color = builder.create_resource(color_desc);
    require(static_cast<bool>(color), "color fixture is valid");
    const auto pass = builder.add_pass({"invalid-stage", {
        ResourceUsage{color.value(), Access::write, PipelineStage::color_output}}});
    require(static_cast<bool>(pass), "valid source pass can be compiled");
    const auto compiled = builder.compile();
    require(static_cast<bool>(compiled), "valid source graph compiles");

    RenderGraphDispatch::PassDescriptions passes;
    passes.emplace(pass.value(), PassDesc{"invalid-stage", {
        ResourceUsage{color.value(), Access::write,
            static_cast<PipelineStage>(255)}}});
    RenderGraphDispatch::ResourceBindings resources;
    resources.emplace(color.value(), TextureHandle{21U, 1U});
    auto created = RenderGraphDispatch::create(
        compiled.value(), std::move(passes), std::move(resources));
    require(static_cast<bool>(created), "bound graph reaches backend validation");
    auto dispatch = created.value();
    DeviceCommandStream stream;
    require(!dispatch.record_pass(pass.value(), stream),
        "invalid stage enum is refused at backend dispatch");
    require(stream.commands().empty(), "invalid stage emits no transitions");
}

void bent_normal_validation_preserves_failure_evidence_without_an_AO_artifact() {
    using carto::production::AmbientOcclusionExecutionReport;
    using carto::production::BakeDiagnostic;
    AmbientOcclusionExecutionReport validation;
    validation.artifact.emplace(); // Synthetic AO template must not cross map types.
    validation.target_texels_visited = (1ULL << 34U) + 17U;
    validation.covered_texels = 23U;
    validation.rays_cast = (1ULL << 35U) + 31U;
    validation.occluded_rays = 7U;
    validation.diagnostics.push_back(BakeDiagnostic{"minority_warning", "retain warning context", false,
        carto::geometry::FaceId{11U}, carto::geometry::FaceId{12U}, 3U, 5U});
    validation.diagnostics.push_back(BakeDiagnostic{"projection_refused", "retain failure evidence", true,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    const auto rejected = carto::d3d12::detail::bent_normal_validation_failure(std::move(validation));
    require(!rejected.artifact.has_value() && !rejected.succeeded(),
        "failed AO projection cannot become a bent-normal artifact or success");
    require(rejected.target_texels_visited == (1ULL << 34U) + 17U &&
            rejected.covered_texels == 23U && rejected.rays_cast == (1ULL << 35U) + 31U &&
            rejected.occluded_rays == 7U, "shared failure counters retain their full width");
    require(rejected.diagnostics.size() == 2U &&
            rejected.diagnostics[0].code == "minority_warning" &&
            rejected.diagnostics[0].message == "retain warning context" &&
            !rejected.diagnostics[0].is_error &&
            rejected.diagnostics[0].target_face == carto::geometry::FaceId{11U} &&
            rejected.diagnostics[0].source_face == carto::geometry::FaceId{12U} &&
            rejected.diagnostics[0].texel_x == 3U && rejected.diagnostics[0].texel_y == 5U &&
            rejected.diagnostics[1].code == "projection_refused" && rejected.diagnostics[1].is_error,
        "warning, error, order and per-face/texel context survive failure conversion");
    const auto empty = carto::d3d12::detail::bent_normal_validation_failure({});
    require(!empty.artifact && !empty.succeeded() && empty.diagnostics.empty(),
        "missing evidence never creates a bent-normal artifact");
    AmbientOcclusionExecutionReport warning_only;
    warning_only.artifact.emplace();
    warning_only.diagnostics.push_back(BakeDiagnostic{"warning_only", "not verified bent-normal evidence", false,
        std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    const auto incomplete = carto::d3d12::detail::bent_normal_validation_failure(std::move(warning_only));
    require(!incomplete.artifact && !incomplete.succeeded() && incomplete.diagnostics.size() == 1U &&
            !incomplete.diagnostics[0].is_error, "AO success or warning-only evidence is not bent-normal success");
}

} // namespace

int main() {
    try {
        dispatch_uses_compiled_order_and_resource_states();
        invalid_order_and_unbound_resources_fail_before_dispatch();
        unsupported_pass_is_atomic_and_does_not_advance();
        unsupported_stage_access_pairs_refuse_without_transitions();
        invalid_access_enum_refuses_without_transitions();
        invalid_stage_enum_refuses_without_transitions();
        bent_normal_validation_preserves_failure_evidence_without_an_AO_artifact();
        std::cout << "PASS bent-normal failure evidence and artifact separation\n";
        std::cout << "PASS D3D12 compiled render-graph dispatch\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL D3D12 render-graph dispatch: " << error.what() << '\n';
        return 1;
    }
}
