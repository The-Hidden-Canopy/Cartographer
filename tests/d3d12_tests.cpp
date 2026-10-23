#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <carto/d3d12/device.hpp>
#include <carto/d3d12/bake_graph.hpp>
#include <carto/d3d12/projection_worker.hpp>
#include <carto/d3d12/swapchain.hpp>
#include <carto/d3d12/texture_upload.hpp>
#include <carto/production/render_mesh.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/render_graph/graph.hpp>

#include <carto/attributes/set.hpp>
#include <carto/attributes/uv.hpp>
#include <carto/geometry/mesh.hpp>

#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

class TempDirectory final {
public:
    TempDirectory() {
        path_ = std::filesystem::current_path() / "test-temp" /
            ("cartographer-d3d12-bake-publish-" +
                std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class D3D12RenderGraphParity final {
public:
    using ResourceHandle = carto::device_ir::ResourceHandle;

    [[nodiscard]] carto::render_graph::ResourceId add_texture(
        std::string name,
        carto::gpu::TextureHandle handle,
        std::uint32_t width,
        std::uint32_t height,
        carto::gpu::Format format,
        std::optional<carto::device_ir::ResourceState> initial_state = std::nullopt) {
        carto::render_graph::ResourceDesc descriptor;
        descriptor.name = std::move(name);
        descriptor.kind = carto::render_graph::ResourceKind::texture;
        descriptor.imported = initial_state.has_value();
        descriptor.width = width;
        descriptor.height = height;
        descriptor.depth = 1U;
        descriptor.layers = 1U;
        descriptor.mip_levels = 1U;
        descriptor.format = format;
        const auto id = graph_.create_resource(std::move(descriptor));
        if (!id) throw TestFailure("D3D12 graph texture: " + id.error().message);
        handles_.emplace(ResourceHandle{handle}, id.value());
        if (initial_state.has_value()) initial_states_[id.value()] = *initial_state;
        return id.value();
    }

    [[nodiscard]] carto::render_graph::ResourceId add_buffer(
        std::string name,
        carto::gpu::BufferHandle handle,
        std::uint64_t bytes) {
        carto::render_graph::ResourceDesc descriptor;
        descriptor.name = std::move(name);
        descriptor.kind = carto::render_graph::ResourceKind::buffer;
        descriptor.bytes = bytes;
        const auto id = graph_.create_resource(std::move(descriptor));
        if (!id) throw TestFailure("D3D12 graph buffer: " + id.error().message);
        handles_.emplace(ResourceHandle{handle}, id.value());
        return id.value();
    }

    [[nodiscard]] carto::render_graph::PassId add_pass(
        carto::render_graph::PassDesc descriptor) {
        const auto id = graph_.add_pass(descriptor);
        if (!id) throw TestFailure("D3D12 graph pass: " + id.error().message);
        passes_.push_back(std::move(descriptor));
        return id.value();
    }

    void submit(
        carto::d3d12::D3D12Device& device,
        carto::render_graph::PassId pass,
        const carto::device_ir::DeviceCommandStream& stream,
        carto::gpu::QueueType queue) {
        const auto compiled = graph_.compile();
        if (!compiled) throw TestFailure("D3D12 graph compile: " + compiled.error().message);
        if (submitted_.size() >= compiled.value().pass_order.size() ||
            compiled.value().pass_order.at(submitted_.size()) != pass ||
            passes_.at(submitted_.size()).name.empty()) {
            throw TestFailure("D3D12 submission order differs from the declared render graph");
        }

        struct Transition {
            carto::render_graph::ResourceId resource;
            carto::device_ir::ResourceState before;
            carto::device_ir::ResourceState after;
            auto operator<=>(const Transition&) const = default;
        };
        std::vector<Transition> expected;
        for (const auto& usage : passes_.at(submitted_.size()).usages) {
            const auto before = states_.contains(usage.resource)
                ? states_.at(usage.resource)
                : initial_states_.contains(usage.resource)
                    ? initial_states_.at(usage.resource)
                    : carto::device_ir::ResourceState::undefined;
            const auto after = state_for(usage);
            if (before != after) expected.push_back({usage.resource, before, after});
            states_[usage.resource] = after;
        }

        std::vector<Transition> actual;
        for (const auto& command : stream.commands()) {
            const auto* transition = std::get_if<carto::device_ir::CmdTransitionResource>(
                &command);
            if (transition == nullptr) continue;
            const auto resource = handles_.find(transition->resource);
            if (resource == handles_.end()) continue;
            actual.push_back({resource->second, transition->before, transition->after});
        }
        if (actual != expected) {
            throw TestFailure("D3D12 barriers differ from render-graph resource states in " +
                passes_.at(submitted_.size()).name);
        }
        if (auto valid = stream.validate(); !valid) {
            throw TestFailure("D3D12 graph command stream: " + valid.error().message);
        }
        const auto serial = device.submit(stream, queue);
        if (!serial) {
            std::string message = "D3D12 graph submit for " +
                passes_.at(submitted_.size()).name + ": " + serial.error().message;
            for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
            throw TestFailure(message);
        }
        if (auto waited = device.wait(serial.value()); !waited) {
            throw TestFailure("D3D12 graph wait: " + waited.error().message);
        }
        submitted_.push_back(pass);
    }

    [[nodiscard]] carto::render_graph::CompiledGraph finish() const {
        const auto compiled = graph_.compile();
        if (!compiled) throw TestFailure("D3D12 final graph compile: " + compiled.error().message);
        if (compiled.value().pass_order != submitted_) {
            throw TestFailure("D3D12 submitted passes do not match the complete graph order");
        }
        return compiled.value();
    }

    [[nodiscard]] carto::render_graph::ResourceId id_for(
        ResourceHandle handle) const {
        const auto found = handles_.find(handle);
        if (found == handles_.end()) throw TestFailure("D3D12 graph resource is not registered");
        return found->second;
    }

    void require_destroy_after_last_use(
        ResourceHandle handle,
        const carto::render_graph::CompiledGraph& compiled) const {
        const auto id = id_for(handle);
        const auto lifetime = std::find_if(
            compiled.lifetimes.begin(), compiled.lifetimes.end(),
            [id](const auto& value) { return value.resource == id; });
        if (lifetime == compiled.lifetimes.end() ||
            submitted_.size() <= lifetime->last_pass) {
            throw TestFailure("D3D12 resource destruction precedes its graph lifetime");
        }
    }

private:
    [[nodiscard]] static carto::device_ir::ResourceState state_for(
        const carto::render_graph::ResourceUsage& usage) {
        using carto::device_ir::ResourceState;
        using carto::render_graph::Access;
        using carto::render_graph::PipelineStage;
        switch (usage.stage) {
        case PipelineStage::copy:
            return usage.access == Access::read
                ? ResourceState::copy_source : ResourceState::copy_destination;
        case PipelineStage::compute:
            return usage.access == Access::read
                ? ResourceState::shader_read : ResourceState::shader_write;
        case PipelineStage::vertex:
            return ResourceState::vertex_read;
        case PipelineStage::fragment:
            return usage.access == Access::read
                ? ResourceState::shader_read : ResourceState::shader_write;
        case PipelineStage::color_output:
            return ResourceState::render_target;
        case PipelineStage::depth_stencil:
            return usage.access == Access::read
                ? ResourceState::depth_read : ResourceState::depth_write;
        case PipelineStage::present:
            return ResourceState::present;
        }
        throw TestFailure("D3D12 graph contains an unsupported pipeline stage");
    }

    carto::render_graph::GraphBuilder graph_;
    std::map<ResourceHandle, carto::render_graph::ResourceId> handles_;
    std::map<carto::render_graph::ResourceId, carto::device_ir::ResourceState> initial_states_;
    std::map<carto::render_graph::ResourceId, carto::device_ir::ResourceState> states_;
    std::vector<carto::render_graph::PassDesc> passes_;
    std::vector<carto::render_graph::PassId> submitted_;
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

carto::geometry::EditableMesh make_bake_mesh(
    bool target, bool reverse = false, double source_z_offset = 0.0) {
    using namespace carto;
    geometry::EditableMesh mesh;
    const std::array<core::Vec3d, 3U> positions = target
        ? std::array<core::Vec3d, 3U>{{{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}}}
        : std::array<core::Vec3d, 3U>{{{0.0, 0.0, 0.05 + source_z_offset},
            {1.0, 0.0, 0.05 + source_z_offset},
            {0.0, 1.0, 0.30 + source_z_offset}}};
    REQUIRE(mesh.insert_bulk(
        {{geometry::VertexId{1U}, positions[0U]},
         {geometry::VertexId{2U}, positions[1U]},
         {geometry::VertexId{3U}, positions[2U]}},
        {{geometry::FaceId{1U}, reverse
            ? std::vector<geometry::VertexId>{geometry::VertexId{1U},
                geometry::VertexId{3U}, geometry::VertexId{2U}}
            : std::vector<geometry::VertexId>{geometry::VertexId{1U},
                geometry::VertexId{2U}, geometry::VertexId{3U}}}}));
    if (target) {
        attributes::AttributeSet attributes({{attributes::AttributeDomain::corner, 3U}});
        const std::vector<core::Vec2d> uvs = reverse
            ? std::vector<core::Vec2d>{{0.0, 0.0}, {0.0, 1.0}, {1.0, 0.0}}
            : std::vector<core::Vec2d>{{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
        REQUIRE(attributes.add_layer(attributes::AttributeLayer{
            {"uv0.corners", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
            std::vector<attributes::AttributeValue>(uvs.begin(), uvs.end())}));
        attributes::UvSetTable sets;
        REQUIRE(sets.add(attributes::UvSetDescriptor{
            7U, "UV0", "uv0.corners", true, true, false, 1U}));
        REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(sets)));
    }
    REQUIRE(mesh.validate());
    return mesh;
}

void d3d12_device_reports_native_identity_and_owns_resources() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    if (!created) {
        throw TestFailure("D3D12 create: " + created.error().message);
    }
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.backend() == device::BackendKind::d3d12);
    REQUIRE(!device.identity().adapter_name.empty());
    REQUIRE(!device.identity().stable_adapter_id.empty());
    REQUIRE(device.capabilities().graphics_queue);
    REQUIRE(device.capabilities().compute_queue);
    REQUIRE(device.capabilities().copy_queue);

    const auto selected = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        device.identity().stable_adapter_id,
        false,
        false,
    });
    if (!selected) {
        throw TestFailure("D3D12 explicit adapter selection: " + selected.error().message);
    }
    REQUIRE(selected);
    REQUIRE(selected.value()->identity().stable_adapter_id ==
            device.identity().stable_adapter_id);

    const auto buffer = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::device_local,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(buffer);
    const auto upload = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(upload);
    const auto readback = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(readback);
    const std::vector<std::uint8_t> bytes = {1U, 2U, 3U, 5U};
    REQUIRE(device.write_buffer(upload.value(), 0U, bytes));

    device_ir::DeviceCommandStream copy_stream;
    copy_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    copy_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    copy_stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, static_cast<std::uint64_t>(bytes.size())});
    const auto copy_serial = device.submit(copy_stream, gpu::QueueType::copy);
    if (!copy_serial) {
        throw TestFailure("D3D12 copy submit: " + copy_serial.error().message);
    }
    REQUIRE(copy_serial);
    REQUIRE(device.wait(copy_serial.value()));
    const auto copied = device.read_buffer(readback.value(), 0U, bytes.size());
    REQUIRE(copied && copied.value() == bytes);

    device_ir::DeviceCommandStream rejected_stream;
    rejected_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::copy_source,
        device_ir::ResourceState::vertex_read,
    });
    rejected_stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, static_cast<std::uint64_t>(bytes.size())});
    const auto rejected = device.submit(rejected_stream, gpu::QueueType::copy);
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == core::ErrorCode::validation_failed);

    device_ir::DeviceCommandStream replay_stream;
    replay_stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, static_cast<std::uint64_t>(bytes.size())});
    const auto replay_serial = device.submit(replay_stream, gpu::QueueType::copy);
    REQUIRE(replay_serial);
    REQUIRE(device.wait(replay_serial.value()));
    const auto replayed = device.read_buffer(readback.value(), 0U, bytes.size());
    REQUIRE(replayed && replayed.value() == bytes);

    const auto texture = device.create_texture(gpu::TextureDesc{
        4U,
        4U,
        1U,
        1U,
        1U,
        gpu::Format::rgba8_unorm,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment,
        {0.1F, 0.2F, 0.3F, 1.0F},
        1.0F,
    });
    REQUIRE(texture);

    device_ir::DeviceCommandStream clear_stream;
    clear_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    clear_stream.append(device_ir::CmdBeginRendering{texture.value(), std::nullopt});
    clear_stream.append(device_ir::CmdClearColor{
        texture.value(), {0.1F, 0.2F, 0.3F, 1.0F}});
    clear_stream.append(device_ir::CmdEndRendering{});
    const auto clear_serial = device.submit(clear_stream, gpu::QueueType::graphics);
    REQUIRE(clear_serial);
    REQUIRE(device.wait(clear_serial.value()));

    REQUIRE(device.destroy_texture(texture.value()));
    REQUIRE(device.destroy_buffer(upload.value()));
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.destroy_buffer(buffer.value()));
    REQUIRE(!device.destroy_buffer(buffer.value()));
}

void d3d12_lifetimes_recycle_descriptors_and_defer_resource_destruction() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();

    gpu::SamplerDesc sampler_descriptor{};
    sampler_descriptor.anisotropy = true;
    sampler_descriptor.max_anisotropy = 17.0F;
    const auto rejected_sampler = device.create_sampler(sampler_descriptor);
    REQUIRE(!rejected_sampler);
    REQUIRE(rejected_sampler.error().code == core::ErrorCode::invalid_argument);

    sampler_descriptor.anisotropy = false;
    sampler_descriptor.max_anisotropy = 1.0F;
    for (std::size_t iteration = 0U; iteration < 2050U; ++iteration) {
        const auto sampler = device.create_sampler(sampler_descriptor);
        REQUIRE(sampler);
        REQUIRE(device.destroy_sampler(sampler.value()));
    }

    const auto upload = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    const auto readback = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(upload && readback);
    const std::vector<std::uint8_t> bytes = {9U, 8U, 7U};
    REQUIRE(device.write_buffer(upload.value(), 0U, bytes));

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    stream.append(device_ir::CmdCopyBuffer{
        upload.value(), readback.value(), 0U, 0U, 3U});
    const auto serial = device.submit(stream, gpu::QueueType::copy);
    REQUIRE(serial);
    REQUIRE(device.destroy_buffer(upload.value()));
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.wait(serial.value()));
    REQUIRE(!device.read_buffer(readback.value(), 0U, 3U));

    const auto live_at_device_teardown = device.create_sampler(sampler_descriptor);
    REQUIRE(live_at_device_teardown);
}

void d3d12_device_loss_is_fail_closed_and_recoverable() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, false});
    REQUIRE(created);
    auto& device = *created.value();
    device.record_external_device_loss(
        "test device-loss injection", static_cast<std::uint32_t>(DXGI_ERROR_DEVICE_REMOVED));
    REQUIRE(device.device_lost());

    const auto rejected_buffer = device.create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(!rejected_buffer);
    REQUIRE(rejected_buffer.error().code == core::ErrorCode::invalid_state);

    device_ir::DeviceCommandStream stream;
    const auto rejected_submit = device.submit(stream, gpu::QueueType::graphics);
    REQUIRE(!rejected_submit);
    REQUIRE(rejected_submit.error().code == core::ErrorCode::invalid_state);

    const auto recovered = device.recover();
    REQUIRE(recovered);
    REQUIRE(!recovered.value()->device_lost());
    const auto buffer = recovered.value()->create_buffer(gpu::BufferDesc{
        64U,
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(buffer);
    REQUIRE(recovered.value()->destroy_buffer(buffer.value()));
}

void d3d12_captures_native_timestamp_queries() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().timestamp_queries);
    REQUIRE(device.timestamp_frequency(gpu::QueueType::graphics) != 0U);
    REQUIRE(device.timestamp_frequency(gpu::QueueType::compute) != 0U);

    const auto readback = device.create_buffer(gpu::BufferDesc{
        2U * sizeof(std::uint64_t),
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(readback);

    const auto capture = [&device, readback](gpu::QueueType queue, bool initialize) {
        device_ir::DeviceCommandStream stream;
        if (initialize) {
            stream.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{readback.value()},
                device_ir::ResourceState::undefined,
                device_ir::ResourceState::copy_destination,
            });
        }
        stream.append(device_ir::CmdWriteTimestamp{0U});
        stream.append(device_ir::CmdWriteTimestamp{1U});
        stream.append(device_ir::CmdResolveTimestamps{
            readback.value(), 0U, 2U, 0U});
        const auto serial = device.submit(stream, queue);
        REQUIRE(serial);
        REQUIRE(device.wait(serial.value()));
        const auto bytes = device.read_buffer(
            readback.value(), 0U, 2U * sizeof(std::uint64_t));
        REQUIRE(bytes);
        std::uint64_t timestamps[2U]{};
        std::memcpy(timestamps, bytes.value().data(), sizeof(timestamps));
        REQUIRE(timestamps[1U] >= timestamps[0U]);
    };
    capture(gpu::QueueType::graphics, true);
    capture(gpu::QueueType::compute, false);
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.debug_receipts().empty());
}

void d3d12_uses_compute_queue_shader_states() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    const auto sampled = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled,
    });
    const auto storage = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage,
    });
    REQUIRE(sampled && storage);
    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{sampled.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_read,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{storage.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    const auto serial = device.submit(stream, gpu::QueueType::compute);
    REQUIRE(serial);
    REQUIRE(device.wait(serial.value()));
    REQUIRE(device.debug_receipts().empty());
    REQUIRE(device.destroy_texture(storage.value()));
    REQUIRE(device.destroy_texture(sampled.value()));
}

void d3d12_reuses_multiple_in_flight_command_contexts() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    device_ir::DeviceCommandStream stream;
    std::array<gpu::SubmissionSerial, 8U> serials{};
    for (auto& serial : serials) {
        const auto submitted = device.submit(stream, gpu::QueueType::graphics);
        REQUIRE(submitted);
        serial = submitted.value();
    }
    for (const auto serial : serials) REQUIRE(device.wait(serial));
    REQUIRE(device.debug_receipts().empty());
}

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    return std::vector<std::uint8_t>(begin, begin + sizeof(T));
}

template <typename T>
std::span<const std::uint8_t> bytes_of_vector(const std::vector<T>& values) {
    return {
        reinterpret_cast<const std::uint8_t*>(values.data()),
        values.size() * sizeof(T),
    };
}

struct D3d12PbrFixture {
    carto::production::RenderMeshArtifact artifact;
    carto::production::PbrMeshUpload upload;
};

D3d12PbrFixture authored_pbr_fixture() {
    using namespace carto;

    geometry::EditableMesh mesh;
    const auto v0 = mesh.add_vertex({-0.75, -0.75, 0.0});
    const auto v1 = mesh.add_vertex({0.75, -0.75, 0.0});
    const auto v2 = mesh.add_vertex({0.0, 0.75, 0.0});
    REQUIRE(v0 && v1 && v2);
    REQUIRE(mesh.add_face({v0.value(), v1.value(), v2.value()}));

    const auto topology = mesh.topology();
    REQUIRE(topology && topology.value().corners.size() == 3U);
    const std::map<geometry::VertexId, core::Vec2d> coordinates{
        {v0.value(), {0.0, 1.0}},
        {v1.value(), {1.0, 1.0}},
        {v2.value(), {0.5, 0.0}},
    };
    std::vector<attributes::AttributeValue> uv_values;
    uv_values.reserve(topology.value().corners.size());
    for (const auto& corner : topology.value().corners) {
        uv_values.emplace_back(coordinates.at(corner.vertex));
    }
    attributes::AttributeSet attributes({
        {attributes::AttributeDomain::corner, topology.value().corners.size()},
    });
    REQUIRE(attributes.add_layer({
        {"uv.native-pbr", attributes::AttributeDomain::corner,
         attributes::AttributeType::vec2},
        std::move(uv_values),
    }));
    attributes::UvSetTable uv_sets;
    REQUIRE(uv_sets.add({1U, "Native PBR UV0", "uv.native-pbr", false, true, false, 1U}));
    REQUIRE(mesh.set_attribute_payload(std::move(attributes), std::move(uv_sets)));

    constexpr std::string_view source_text = "cartographer-d3d12-authored-pbr-fixture-v1";
    const auto source_digest = assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(source_text.data()), source_text.size()});
    const production::AuthoritativeSource source{
        "fixture/native-pbr", core::Revision{1U}, source_digest};
    const auto artifact = production::compile_render_mesh_artifact(
        source,
        mesh,
        production::RenderMeshCompileOptions{
            .uv_set = 1U,
            .uv1_set = std::nullopt,
            .generate_tangents = true,
            .material_slot_layer = std::nullopt,
            .vertex_color_layer = std::nullopt,
            .normal_layer = std::nullopt,
        });
    if (!artifact) {
        throw TestFailure("compile authored PBR fixture: " + artifact.error().message);
    }
    const auto upload = production::pack_pbr_mesh_upload(artifact.value());
    if (!upload) {
        throw TestFailure("pack authored PBR fixture: " + upload.error().message);
    }
    return {artifact.value(), upload.value()};
}

void d3d12_compiles_dxil_draws_indexed_and_reads_texture() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().anisotropy);
    REQUIRE(device.capabilities().sampler_compare);
    REQUIRE(device.capabilities().max_texture_dimension_2d >= 16384U);
    REQUIRE(device.capabilities().max_sampler_anisotropy >= 16.0F);

    const auto oversized_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_oversized_source",
        gpu::ShaderStage::vertex,
        "main",
        "d3d12_oversized_source.hlsl",
        std::string(16U * 1024U * 1024U + 1U, ' '),
        true,
        false,
    });
    REQUIRE(!oversized_source);
    REQUIRE(oversized_source.error().code == core::ErrorCode::invalid_argument);

    const auto vertex_shader = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_test_vertex",
        gpu::ShaderStage::vertex,
        "main",
        "d3d12_test_vertex.hlsl",
        R"(
            struct Input { float4 position : POSITION; };
            struct Output { float4 position : SV_POSITION; };
            Output main(Input input) {
                Output output;
                output.position = input.position;
                return output;
            }
        )",
        true,
        false,
    });
    if (!vertex_shader) {
        throw TestFailure("DXC vertex compile: " + vertex_shader.error().message);
    }
    const auto fragment_shader = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_test_fragment",
        gpu::ShaderStage::fragment,
        "main",
        "d3d12_test_fragment.hlsl",
        R"(
            cbuffer PushConstants : register(b0) { float4 color; };
            float4 main() : SV_TARGET { return color; }
        )",
        true,
        false,
    });
    if (!fragment_shader) {
        throw TestFailure("DXC fragment compile: " + fragment_shader.error().message);
    }
    REQUIRE(vertex_shader.value().format == device::BinaryFormat::dxil);
    REQUIRE(fragment_shader.value().format == device::BinaryFormat::dxil);
    REQUIRE(vertex_shader.value().bytes.size() > 32U);
    REQUIRE(fragment_shader.value().bytes.size() > 32U);

    const auto vertex = device.create_shader(vertex_shader.value());
    const auto fragment = device.create_shader(fragment_shader.value());
    REQUIRE(vertex && fragment);

    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "d3d12_indexed_triangle";
    pipeline_descriptor.shaders = {vertex.value(), fragment.value()};
    pipeline_descriptor.cull_mode = gpu::CullMode::none;
    pipeline_descriptor.color_format = gpu::Format::rgba32_float;
    pipeline_descriptor.depth_format = gpu::Format::depth32_float;
    pipeline_descriptor.push_constant_bytes = 16U;
    const auto pipeline = device.create_pipeline(pipeline_descriptor);
    if (!pipeline) {
        std::string message = "D3D12 PSO: " + pipeline.error().message;
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        const auto append_prefix = [&message](const char* label, const auto& bytes) {
            message += " | " + std::string(label) + "=";
            const std::size_t count = std::min<std::size_t>(16U, bytes.size());
            for (std::size_t index = 0U; index < count; ++index) {
                constexpr char digits[] = "0123456789abcdef";
                const auto byte = bytes[index];
                message += digits[(byte >> 4U) & 0x0fU];
                message += digits[byte & 0x0fU];
            }
        };
        append_prefix("vs", vertex_shader.value().bytes);
        append_prefix("ps", fragment_shader.value().bytes);
        throw TestFailure(message);
    }

    const std::array<std::array<float, 4U>, 3U> vertices = {{
        {{-0.8F, -0.8F, 0.25F, 1.0F}},
        {{0.8F, -0.8F, 0.25F, 1.0F}},
        {{0.0F, 0.8F, 0.25F, 1.0F}},
    }};
    const std::array<std::uint32_t, 3U> indices = {0U, 1U, 2U};
    const auto vertex_buffer = device.create_buffer(gpu::BufferDesc{
        sizeof(vertices), gpu::MemoryClass::upload,
        gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source});
    const auto index_buffer = device.create_buffer(gpu::BufferDesc{
        sizeof(indices), gpu::MemoryClass::upload,
        gpu::BufferUsage::index | gpu::BufferUsage::transfer_source});
    REQUIRE(vertex_buffer && index_buffer);
    REQUIRE(device.write_buffer(vertex_buffer.value(), 0U, bytes_of(vertices)));
    REQUIRE(device.write_buffer(index_buffer.value(), 0U, bytes_of(indices)));

    const auto color = device.create_texture(gpu::TextureDesc{
        32U, 32U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source});
    const auto depth = device.create_texture(gpu::TextureDesc{
        32U, 32U, 1U, 1U, 1U, gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment});
    REQUIRE(color && depth);

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{color.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{depth.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read});
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read});
    stream.append(device_ir::CmdBeginRendering{color.value(), depth.value()});
    stream.append(device_ir::CmdClearColor{color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    stream.append(device_ir::CmdClearDepth{depth.value(), 1.0F});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 16U});
    stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    const std::array<float, 4U> draw_color = {1.0F, 0.25F, 0.0F, 1.0F};
    stream.append(device_ir::CmdPushConstants{bytes_of(draw_color)});
    stream.append(device_ir::CmdDrawIndexed{3U, 1U, 0U, 0, 0U});
    stream.append(device_ir::CmdEndRendering{});
    const auto serial = device.submit(stream, gpu::QueueType::graphics);
    if (!serial) throw TestFailure("D3D12 indexed draw: " + serial.error().message);
    REQUIRE(device.wait(serial.value()));

    const auto pixels = device.read_texture_rgba32f(color.value());
    if (!pixels) throw TestFailure("D3D12 texture readback: " + pixels.error().message);
    REQUIRE(pixels.value().size() == 32U * 32U * 4U);
    const std::size_t center = (16U * 32U + 16U) * 4U;
    REQUIRE(pixels.value()[center] > 0.8F);
    REQUIRE(pixels.value()[center + 1U] > 0.1F);
    REQUIRE(pixels.value()[center + 3U] > 0.8F);
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
    REQUIRE(device.debug_receipts().empty());

    REQUIRE(device.destroy_texture(depth.value()));
    REQUIRE(device.destroy_texture(color.value()));
    REQUIRE(device.destroy_buffer(index_buffer.value()));
    REQUIRE(device.destroy_buffer(vertex_buffer.value()));
    REQUIRE(device.destroy_pipeline(pipeline.value()));
    REQUIRE(device.destroy_shader(fragment.value()));
    REQUIRE(device.destroy_shader(vertex.value()));
}

std::string read_d3d12_shader(const char* name) {
#ifdef CARTO_D3D12_SHADER_DIR
    const std::filesystem::path path = std::filesystem::path(CARTO_D3D12_SHADER_DIR) / name;
#else
    const std::filesystem::path path = std::filesystem::path("libs/d3d12/shaders") / name;
#endif
    std::ifstream input(path, std::ios::binary);
    if (!input) throw TestFailure("unable to read shader source: " + path.string());
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void d3d12_executes_builtin_shader_suite_and_dispatches_compute() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().timestamp_queries);
    const std::uint64_t graphics_timestamp_frequency =
        device.timestamp_frequency(gpu::QueueType::graphics);
    REQUIRE(graphics_timestamp_frequency != 0U);

    const auto compile_and_create = [&device](
        const char* name,
        gpu::ShaderStage stage,
        const char* entry_point) -> gpu::ShaderHandle {
        const auto source = d3d12::compile_hlsl(d3d12::ShaderSource{
            name,
            stage,
            entry_point,
            name,
            read_d3d12_shader(name),
            true,
            false,
        });
        if (!source) {
            throw TestFailure(std::string("DXC built-in shader ") + name + ": " +
                              source.error().message);
        }
        const auto handle = device.create_shader(source.value());
        if (!handle) {
            throw TestFailure(std::string("D3D12 built-in shader ") + name + ": " +
                              handle.error().message);
        }
        return handle.value();
    };

    const auto pbr_vertex = compile_and_create(
        "cartographer_pbr.hlsl", gpu::ShaderStage::vertex, "VSMain");
    const auto pbr_fragment = compile_and_create(
        "cartographer_pbr.hlsl", gpu::ShaderStage::fragment, "PSMain");
    const auto shadow_vertex = compile_and_create(
        "cartographer_shadow.hlsl", gpu::ShaderStage::vertex, "VSMain");
    const auto shadow_fragment = compile_and_create(
        "cartographer_shadow.hlsl", gpu::ShaderStage::fragment, "PSMain");
    const auto temporal_compute = compile_and_create(
        "cartographer_temporal.hlsl", gpu::ShaderStage::compute, "CSMain");
    const auto tone_vertex = compile_and_create(
        "cartographer_tonemap.hlsl", gpu::ShaderStage::vertex, "VSMain");
    const auto tone_fragment = compile_and_create(
        "cartographer_tonemap.hlsl", gpu::ShaderStage::fragment, "PSMain");

    gpu::PipelineDesc pbr_descriptor;
    pbr_descriptor.debug_name = "cartographer_builtin_pbr";
    pbr_descriptor.shaders = {pbr_vertex, pbr_fragment};
    pbr_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::fragment},
        gpu::DescriptorBinding{0U, 4U, gpu::ShaderStage::fragment}};
    pbr_descriptor.sampled_texture_count = 5U;
    pbr_descriptor.cull_mode = gpu::CullMode::none;
    pbr_descriptor.color_format = gpu::Format::rgba32_float;
    pbr_descriptor.push_constant_bytes = 232U;
    pbr_descriptor.vertex_stride_bytes = 52U;
    pbr_descriptor.vertex_attributes = {
        gpu::VertexAttribute{"POSITION", 0U, gpu::VertexFormat::float4, 0U, 0U},
        gpu::VertexAttribute{"NORMAL", 0U, gpu::VertexFormat::float3, 16U, 0U},
        gpu::VertexAttribute{"TEXCOORD", 0U, gpu::VertexFormat::float2, 28U, 0U},
        gpu::VertexAttribute{"TANGENT", 0U, gpu::VertexFormat::float4, 36U, 0U},
    };
    auto oversized_pbr_descriptor = pbr_descriptor;
    oversized_pbr_descriptor.debug_name = "cartographer_oversized_pbr";
    oversized_pbr_descriptor.push_constant_bytes = 236U;
    const auto oversized_pbr_pipeline = device.create_pipeline(oversized_pbr_descriptor);
    REQUIRE(!oversized_pbr_pipeline);
    REQUIRE(oversized_pbr_pipeline.error().message.find("64 DWORDs") !=
            std::string::npos);
    const auto pbr_pipeline = device.create_pipeline(pbr_descriptor);
    if (!pbr_pipeline) {
        throw TestFailure("D3D12 built-in PBR pipeline: " + pbr_pipeline.error().message);
    }

    gpu::PipelineDesc shadow_descriptor;
    shadow_descriptor.debug_name = "cartographer_builtin_shadow";
    shadow_descriptor.shaders = {shadow_vertex, shadow_fragment};
    shadow_descriptor.cull_mode = gpu::CullMode::none;
    shadow_descriptor.color_format = gpu::Format::rgba32_float;
    shadow_descriptor.depth_format = gpu::Format::depth32_float;
    shadow_descriptor.depth_test = true;
    shadow_descriptor.depth_write = true;
    shadow_descriptor.push_constant_bytes = 128U;
    shadow_descriptor.vertex_stride_bytes = production::kPbrUploadVertexStrideBytes;
    shadow_descriptor.vertex_attributes = {
        gpu::VertexAttribute{"POSITION", 0U, gpu::VertexFormat::float4, 0U, 0U},
    };
    const auto shadow_pipeline = device.create_pipeline(shadow_descriptor);
    if (!shadow_pipeline) {
        throw TestFailure("D3D12 built-in shadow pipeline: " +
                          shadow_pipeline.error().message);
    }

    gpu::PipelineDesc tone_descriptor;
    tone_descriptor.debug_name = "cartographer_builtin_tonemap";
    tone_descriptor.shaders = {tone_vertex, tone_fragment};
    tone_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::fragment}};
    tone_descriptor.cull_mode = gpu::CullMode::none;
    tone_descriptor.color_format = gpu::Format::rgba32_float;
    tone_descriptor.depth_test = false;
    tone_descriptor.push_constant_bytes = 16U;
    const auto tone_pipeline = device.create_pipeline(tone_descriptor);
    if (!tone_pipeline) {
        throw TestFailure("D3D12 built-in tone-map pipeline: " + tone_pipeline.error().message);
    }

    const auto compute_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "d3d12_storage_compute",
        gpu::ShaderStage::compute,
        "main",
        "d3d12_storage_compute.hlsl",
        R"(
            RWTexture2D<float4> output : register(u0);
            [numthreads(8, 8, 1)]
            void main(uint3 dispatch_id : SV_DISPATCHTHREADID) {
                if (dispatch_id.x < 4 && dispatch_id.y < 4) {
                    output[dispatch_id.xy] = float4(0.25, 0.5, 0.75, 1.0);
                }
            }
        )",
        true,
        false,
    });
    REQUIRE(compute_source);
    const auto compute_shader = device.create_shader(compute_source.value());
    REQUIRE(compute_shader);
    gpu::PipelineDesc compute_descriptor;
    compute_descriptor.debug_name = "d3d12_storage_compute";
    compute_descriptor.shaders = {compute_shader.value()};
    compute_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute}};
    compute_descriptor.depth_test = false;
    const auto compute_pipeline = device.create_pipeline(compute_descriptor);
    if (!compute_pipeline) {
        throw TestFailure("D3D12 compute pipeline: " + compute_pipeline.error().message);
    }
    gpu::PipelineDesc temporal_descriptor;
    temporal_descriptor.debug_name = "cartographer_builtin_temporal";
    temporal_descriptor.shaders = {temporal_compute};
    temporal_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::compute},
    };
    temporal_descriptor.sampled_texture_count = 3U;
    temporal_descriptor.depth_test = false;
    temporal_descriptor.push_constant_bytes = 48U;
    const auto temporal_pipeline = device.create_pipeline(temporal_descriptor);
    if (!temporal_pipeline) {
        throw TestFailure("D3D12 built-in temporal pipeline: " +
                          temporal_pipeline.error().message);
    }

    const auto output = device.create_texture(gpu::TextureDesc{
        4U,
        4U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(output);
    device_ir::DeviceCommandStream compute_stream;
    compute_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{output.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    compute_stream.append(device_ir::CmdBindPipeline{compute_pipeline.value()});
    compute_stream.append(device_ir::CmdBindResources{
        {device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{output.value()}, std::nullopt}}});
    compute_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    compute_stream.append(device_ir::CmdUavBarrier{device_ir::ResourceHandle{output.value()}});
    const auto serial = device.submit(compute_stream, gpu::QueueType::compute);
    if (!serial) throw TestFailure("D3D12 compute submit: " + serial.error().message);
    REQUIRE(device.wait(serial.value()));
    const auto pixels = device.read_texture_rgba32f(output.value());
    if (!pixels) throw TestFailure("D3D12 compute readback: " + pixels.error().message);
    REQUIRE(pixels.value().size() == 4U * 4U * 4U);
    REQUIRE(pixels.value()[0U] > 0.24F && pixels.value()[0U] < 0.26F);
    REQUIRE(pixels.value()[1U] > 0.49F && pixels.value()[1U] < 0.51F);
    REQUIRE(pixels.value()[2U] > 0.74F && pixels.value()[2U] < 0.76F);
    REQUIRE(pixels.value()[3U] > 0.99F);

    gpu::SamplerDesc sampler_descriptor;
    sampler_descriptor.min_filter = gpu::Filter::linear;
    sampler_descriptor.mag_filter = gpu::Filter::linear;
    sampler_descriptor.mip_filter = gpu::MipFilter::nearest;
    const auto sampler = device.create_sampler(sampler_descriptor);
    REQUIRE(sampler);

    const std::array<std::array<std::uint8_t, 4U>, 5U> material_texels = {{
        {{255U, 128U, 64U, 255U}},   // base color, sRGB
        {{0U, 128U, 255U, 255U}},    // metallic/roughness: B metallic, G roughness
        {{255U, 0U, 255U, 255U}},    // tangent normal, DirectX Y-
        {{32U, 255U, 255U, 255U}},   // occlusion in R
        {{255U, 64U, 0U, 255U}},      // emissive, sRGB
    }};
    const std::array<gpu::Format, 5U> material_formats = {{
        gpu::Format::rgba8_srgb,
        gpu::Format::rgba8_unorm,
        gpu::Format::rgba8_unorm,
        gpu::Format::rgba8_unorm,
        gpu::Format::rgba8_srgb,
    }};
    std::array<gpu::BufferHandle, 5U> material_uploads{};
    std::array<gpu::TextureHandle, 5U> material_textures{};
    for (std::size_t index = 0U; index < material_texels.size(); ++index) {
        const auto upload = device.create_buffer(gpu::BufferDesc{
            256U,
            gpu::MemoryClass::upload,
            gpu::BufferUsage::transfer_source,
        });
        const auto texture = device.create_texture(gpu::TextureDesc{
            1U,
            1U,
            1U,
            1U,
            1U,
            material_formats.at(index),
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
        });
        REQUIRE(upload && texture);
        material_uploads.at(index) = upload.value();
        material_textures.at(index) = texture.value();
        REQUIRE(device.write_buffer(
            material_uploads.at(index), 0U, material_texels.at(index)));
    }
    device_ir::DeviceCommandStream upload_stream;
    for (std::size_t index = 0U; index < material_texels.size(); ++index) {
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{material_uploads.at(index)},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_source,
        });
        upload_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{material_textures.at(index)},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination,
        });
        upload_stream.append(device_ir::CmdCopyBufferToTexture{
            material_uploads.at(index), material_textures.at(index), 0U});
    }
    const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
    REQUIRE(upload_serial);
    REQUIRE(device.wait(upload_serial.value()));
    for (const auto upload : material_uploads) REQUIRE(device.destroy_buffer(upload));

    const D3d12PbrFixture pbr_fixture = authored_pbr_fixture();
    const auto& vertices = pbr_fixture.upload.vertices;
    const auto& indices = pbr_fixture.upload.indices;
    REQUIRE(vertices.size() == 3U && indices.size() == 3U);
    REQUIRE(pbr_fixture.upload.source_revision == pbr_fixture.artifact.source_revision);
    REQUIRE(pbr_fixture.upload.source_digest == pbr_fixture.artifact.source_digest);
    const auto vertex_buffer = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(vertices.size()) * sizeof(production::PbrUploadVertex),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source,
    });
    const auto index_buffer = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(indices.size()) * sizeof(std::uint32_t),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::index | gpu::BufferUsage::transfer_source,
    });
    const auto pbr_target = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment |
            gpu::TextureUsage::transfer_source,
    });
    const auto depth_target = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment,
    });
    const auto tone_target = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    });
    const auto pbr_timestamps = device.create_buffer(gpu::BufferDesc{
        2U * sizeof(std::uint64_t),
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(vertex_buffer && index_buffer && pbr_target && depth_target && tone_target &&
            pbr_timestamps);
    REQUIRE(device.write_buffer(vertex_buffer.value(), 0U, bytes_of_vector(vertices)));
    REQUIRE(device.write_buffer(index_buffer.value(), 0U, bytes_of_vector(indices)));

    D3D12RenderGraphParity graph_parity;
    std::array<render_graph::ResourceId, 5U> material_graph_resources{};
    for (std::size_t index = 0U; index < material_graph_resources.size(); ++index) {
        material_graph_resources.at(index) = graph_parity.add_texture(
            "pbr_material_" + std::to_string(index), material_textures.at(index),
            1U, 1U, material_formats.at(index), device_ir::ResourceState::copy_destination);
    }
    const auto pbr_graph_resource = graph_parity.add_texture(
        "pbr_hdr", pbr_target.value(), 32U, 32U, gpu::Format::rgba32_float);
    const auto depth_graph_resource = graph_parity.add_texture(
        "pbr_depth", depth_target.value(), 32U, 32U, gpu::Format::depth32_float);
    const auto tone_graph_resource = graph_parity.add_texture(
        "tone_mapped", tone_target.value(), 32U, 32U, gpu::Format::rgba32_float);
    const auto timestamps_graph_resource = graph_parity.add_buffer(
        "pbr_timestamps", pbr_timestamps.value(), 2U * sizeof(std::uint64_t));
    std::vector<render_graph::ResourceUsage> pbr_usages;
    for (const auto resource : material_graph_resources) {
        pbr_usages.push_back({resource, render_graph::Access::read,
                              render_graph::PipelineStage::fragment});
    }
    pbr_usages.push_back({pbr_graph_resource, render_graph::Access::write,
                          render_graph::PipelineStage::color_output});
    pbr_usages.push_back({depth_graph_resource, render_graph::Access::write,
                          render_graph::PipelineStage::depth_stencil});
    pbr_usages.push_back({timestamps_graph_resource, render_graph::Access::write,
                          render_graph::PipelineStage::copy});
    const auto pbr_graph_pass = graph_parity.add_pass({"pbr_hdr", std::move(pbr_usages)});
    const auto tone_graph_pass = graph_parity.add_pass({"tone_map", {
        {pbr_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::fragment},
        {tone_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::color_output},
    }});

    std::array<float, 58U> pbr_constants{};
    for (std::size_t matrix = 0U; matrix < 2U; ++matrix) {
        for (std::size_t diagonal = 0U; diagonal < 4U; ++diagonal) {
            pbr_constants[matrix * 16U + diagonal * 5U] = 1.0F;
        }
    }
    pbr_constants[34U] = 2.0F;
    pbr_constants[35U] = 0.0F;
    pbr_constants[36U] = 1.0F;
    pbr_constants[37U] = 1.0F;
    pbr_constants[38U] = 1.0F;
    pbr_constants[39U] = 1.0F;
    pbr_constants[40U] = 0.2F;
    pbr_constants[41U] = 0.1F;
    pbr_constants[43U] = 0.08F;
    pbr_constants[44U] = 1.0F;
    pbr_constants[45U] = 1.0F;
    pbr_constants[46U] = 1.0F;
    pbr_constants[47U] = 1.0F;
    pbr_constants[48U] = 0.0F;
    pbr_constants[49U] = 0.0F;
    pbr_constants[50U] = 1.0F;
    pbr_constants[51U] = 1.0F;
    pbr_constants[52U] = 4.0F;
    pbr_constants[53U] = 4.0F;
    pbr_constants[54U] = 4.0F;
    pbr_constants[55U] = 1.0F;
    pbr_constants[56U] = 0.75F;
    pbr_constants[57U] = 0.2F;
    const std::array<float, 4U> tone_constants = {0.0F, 0.0F, 0.0F, 0.0F};

    device_ir::DeviceCommandStream render_stream;
    for (const auto texture : material_textures) {
        render_stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read,
        });
    }
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{index_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::index_read,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{pbr_target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{depth_target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write,
    });
    render_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{pbr_timestamps.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    render_stream.append(device_ir::CmdWriteTimestamp{0U});
    render_stream.append(device_ir::CmdBeginRendering{pbr_target.value(), depth_target.value()});
    render_stream.append(device_ir::CmdClearColor{pbr_target.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    render_stream.append(device_ir::CmdClearDepth{depth_target.value(), 1.0F});
    render_stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    render_stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    render_stream.append(device_ir::CmdBindPipeline{pbr_pipeline.value()});
    render_stream.append(device_ir::CmdBindVertexBuffer{
        vertex_buffer.value(), 0U, production::kPbrUploadVertexStrideBytes});
    render_stream.append(device_ir::CmdBindIndexBuffer{
        index_buffer.value(), 0U, device_ir::IndexFormat::uint32});
    render_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{material_textures.at(0U)}, sampler.value()},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{material_textures.at(1U)}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 2U, device_ir::ResourceHandle{material_textures.at(2U)}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 3U, device_ir::ResourceHandle{material_textures.at(3U)}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 4U, device_ir::ResourceHandle{material_textures.at(4U)}, std::nullopt},
    }});
    render_stream.append(device_ir::CmdPushConstants{bytes_of(pbr_constants)});
    render_stream.append(device_ir::CmdDrawIndexed{
        static_cast<std::uint32_t>(indices.size()), 1U, 0U, 0, 0U});
    render_stream.append(device_ir::CmdEndRendering{});
    render_stream.append(device_ir::CmdWriteTimestamp{1U});
    render_stream.append(device_ir::CmdResolveTimestamps{
        pbr_timestamps.value(), 0U, 2U, 0U});
    device_ir::DeviceCommandStream tone_stream;
    tone_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{pbr_target.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    tone_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{tone_target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    tone_stream.append(device_ir::CmdBeginRendering{tone_target.value(), std::nullopt});
    tone_stream.append(device_ir::CmdClearColor{tone_target.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    tone_stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    tone_stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    tone_stream.append(device_ir::CmdBindPipeline{tone_pipeline.value()});
    tone_stream.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 52U});
    tone_stream.append(device_ir::CmdBindResources{{device_ir::ResourceBinding{
        0U, 0U, device_ir::ResourceHandle{pbr_target.value()}, sampler.value()}}});
    tone_stream.append(device_ir::CmdPushConstants{bytes_of(tone_constants)});
    tone_stream.append(device_ir::CmdDraw{3U, 1U, 0U, 0U});
    tone_stream.append(device_ir::CmdEndRendering{});
    graph_parity.submit(device, pbr_graph_pass, render_stream, gpu::QueueType::graphics);
    graph_parity.submit(device, tone_graph_pass, tone_stream, gpu::QueueType::graphics);
    const auto timestamp_bytes = device.read_buffer(
        pbr_timestamps.value(), 0U, 2U * sizeof(std::uint64_t));
    REQUIRE(timestamp_bytes && timestamp_bytes.value().size() == 2U * sizeof(std::uint64_t));
    std::array<std::uint64_t, 2U> timestamp_values{};
    std::memcpy(timestamp_values.data(), timestamp_bytes.value().data(),
                timestamp_bytes.value().size());
    REQUIRE(timestamp_values[1U] >= timestamp_values[0U]);
    const long double pbr_gpu_nanoseconds =
        static_cast<long double>(timestamp_values[1U] - timestamp_values[0U]) *
        1'000'000'000.0L / static_cast<long double>(graphics_timestamp_frequency);
    REQUIRE(pbr_gpu_nanoseconds >= 0.0L);
    REQUIRE(pbr_fixture.upload.resident_bytes() ==
            vertices.size() * sizeof(production::PbrUploadVertex) +
                indices.size() * sizeof(std::uint32_t));
    std::cout << "D3D12 authored PBR fixture: "
              << static_cast<std::uint64_t>(pbr_gpu_nanoseconds)
              << " gpu ns, " << pbr_fixture.upload.resident_bytes()
              << " vertex/index bytes\n";
    const auto pbr_pixels = device.read_texture_rgba32f(pbr_target.value());
    REQUIRE(pbr_pixels);
    const std::size_t center = (16U * 32U + 16U) * 4U;
    REQUIRE(pbr_pixels.value()[center] > 0.0F);
    REQUIRE(pbr_pixels.value()[center + 3U] > 0.9F);
    const auto tone_pixels = device.read_texture_rgba32f(tone_target.value());
    REQUIRE(tone_pixels);
    REQUIRE(tone_pixels.value()[center] > 0.0F && tone_pixels.value()[center] <= 1.01F);
    REQUIRE(tone_pixels.value()[center + 3U] > 0.9F);

    const auto shadow_color = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment,
        {0.0F, 0.0F, 0.0F, 1.0F},
        1.0F,
    });
    const auto shadow_depth = device.create_texture(gpu::TextureDesc{
        32U,
        32U,
        1U,
        1U,
        1U,
        gpu::Format::depth32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::depth_attachment,
        {0.0F, 0.0F, 0.0F, 1.0F},
        1.0F,
    });
    REQUIRE(shadow_color && shadow_depth);
    const auto shadow_color_graph_resource = graph_parity.add_texture(
        "shadow_color", shadow_color.value(), 32U, 32U, gpu::Format::rgba32_float);
    const auto shadow_depth_graph_resource = graph_parity.add_texture(
        "shadow_depth", shadow_depth.value(), 32U, 32U, gpu::Format::depth32_float);
    const auto shadow_graph_pass = graph_parity.add_pass({"shadow_map", {
        {shadow_color_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::color_output},
        {shadow_depth_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::depth_stencil},
    }});
    std::array<float, 32U> shadow_constants{};
    for (std::size_t matrix = 0U; matrix < 2U; ++matrix) {
        for (std::size_t diagonal = 0U; diagonal < 4U; ++diagonal) {
            shadow_constants[matrix * 16U + diagonal * 5U] = 1.0F;
        }
    }
    device_ir::DeviceCommandStream shadow_stream;
    shadow_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{shadow_color.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    shadow_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{shadow_depth.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::depth_write,
    });
    shadow_stream.append(device_ir::CmdBeginRendering{
        shadow_color.value(), shadow_depth.value()});
    shadow_stream.append(device_ir::CmdClearColor{
        shadow_color.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    shadow_stream.append(device_ir::CmdClearDepth{shadow_depth.value(), 1.0F});
    shadow_stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 32.0F, 32.0F, 0.0F, 1.0F});
    shadow_stream.append(device_ir::CmdSetScissor{0, 0, 32U, 32U});
    shadow_stream.append(device_ir::CmdBindPipeline{shadow_pipeline.value()});
    shadow_stream.append(device_ir::CmdBindVertexBuffer{
        vertex_buffer.value(), 0U, production::kPbrUploadVertexStrideBytes});
    shadow_stream.append(device_ir::CmdPushConstants{bytes_of(shadow_constants)});
    shadow_stream.append(device_ir::CmdDraw{3U, 1U, 0U, 0U});
    shadow_stream.append(device_ir::CmdEndRendering{});
    graph_parity.submit(device, shadow_graph_pass, shadow_stream, gpu::QueueType::graphics);
    const auto shadow_pixels = device.read_texture_depth32f(shadow_depth.value());
    REQUIRE(shadow_pixels);
    const std::size_t shadow_center = 16U * 32U + 16U;
    REQUIRE(shadow_pixels.value()[shadow_center] < 0.99F);
    const auto temporal_current = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.2F, 0.4F, 0.6F, 1.0F},
        1.0F,
    });
    const auto temporal_history = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.8F, 0.6F, 0.4F, 1.0F},
        1.0F,
    });
    const auto temporal_motion = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rg32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.0F, 0.0F, 0.0F, 0.0F},
        1.0F,
    });
    const auto temporal_resolved = device.create_texture(gpu::TextureDesc{
        8U,
        8U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(temporal_current && temporal_history && temporal_motion && temporal_resolved);

    const auto temporal_current_graph_resource = graph_parity.add_texture(
        "temporal_current", temporal_current.value(), 8U, 8U, gpu::Format::rgba32_float);
    const auto temporal_history_graph_resource = graph_parity.add_texture(
        "temporal_history", temporal_history.value(), 8U, 8U, gpu::Format::rgba32_float);
    const auto temporal_motion_graph_resource = graph_parity.add_texture(
        "temporal_motion", temporal_motion.value(), 8U, 8U, gpu::Format::rg32_float);
    const auto temporal_output_graph_resource = graph_parity.add_texture(
        "temporal_resolved", temporal_resolved.value(), 8U, 8U, gpu::Format::rgba32_float);
    const auto temporal_setup_graph_pass = graph_parity.add_pass({"temporal_inputs", {
        {temporal_current_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::color_output},
        {temporal_history_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::color_output},
        {temporal_motion_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::color_output},
    }});
    const auto temporal_read_graph_pass = graph_parity.add_pass({"temporal_read_transition", {
        {temporal_current_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::fragment},
        {temporal_history_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::fragment},
        {temporal_motion_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::fragment},
    }});
    const auto temporal_resolve_graph_pass = graph_parity.add_pass({"temporal_resolve", {
        {temporal_current_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::compute},
        {temporal_history_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::compute},
        {temporal_motion_graph_resource, render_graph::Access::read,
         render_graph::PipelineStage::compute},
        {temporal_output_graph_resource, render_graph::Access::write,
         render_graph::PipelineStage::compute},
    }});

    device_ir::DeviceCommandStream temporal_setup;
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_current.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_history.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    temporal_setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_motion.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    temporal_setup.append(device_ir::CmdBeginRendering{temporal_current.value(), std::nullopt});
    temporal_setup.append(device_ir::CmdClearColor{
        temporal_current.value(), {0.2F, 0.4F, 0.6F, 1.0F}});
    temporal_setup.append(device_ir::CmdEndRendering{});
    temporal_setup.append(device_ir::CmdBeginRendering{temporal_history.value(), std::nullopt});
    temporal_setup.append(device_ir::CmdClearColor{
        temporal_history.value(), {0.8F, 0.6F, 0.4F, 1.0F}});
    temporal_setup.append(device_ir::CmdEndRendering{});
    temporal_setup.append(device_ir::CmdBeginRendering{temporal_motion.value(), std::nullopt});
    temporal_setup.append(device_ir::CmdClearColor{
        temporal_motion.value(), {0.0F, 0.0F, 0.0F, 0.0F}});
    temporal_setup.append(device_ir::CmdEndRendering{});
    graph_parity.submit(
        device, temporal_setup_graph_pass, temporal_setup, gpu::QueueType::graphics);
    device_ir::DeviceCommandStream temporal_read_transition;
    temporal_read_transition.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_current.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    temporal_read_transition.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_history.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    temporal_read_transition.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_motion.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    graph_parity.submit(
        device, temporal_read_graph_pass, temporal_read_transition, gpu::QueueType::graphics);

    const std::array<float, 8U> temporal_float_constants = {
        0.5F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 0.125F, 0.125F,
    };
    // The acceptance fixture uses a cleared, explicitly static motion field.
    // history_flags.w selects the shader's zero-motion fast path; production
    // callers must set it only when that invariant is authoritative.
    const std::array<std::uint32_t, 4U> temporal_flags = {1U, 0U, 0U, 1U};
    std::array<std::uint8_t, 48U> temporal_constants{};
    std::memcpy(temporal_constants.data(), temporal_float_constants.data(),
                sizeof(temporal_float_constants));
    std::memcpy(temporal_constants.data() + sizeof(temporal_float_constants),
                temporal_flags.data(), sizeof(temporal_flags));
    device_ir::DeviceCommandStream temporal_stream;
    temporal_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_resolved.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    temporal_stream.append(device_ir::CmdBindPipeline{temporal_pipeline.value()});
    temporal_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{temporal_current.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{temporal_history.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 2U, device_ir::ResourceHandle{temporal_motion.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 3U, device_ir::ResourceHandle{temporal_resolved.value()}, std::nullopt},
    }});
    temporal_stream.append(device_ir::CmdPushConstants{bytes_of(temporal_constants)});
    temporal_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    temporal_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{temporal_resolved.value()}});
    graph_parity.submit(
        device, temporal_resolve_graph_pass, temporal_stream, gpu::QueueType::compute);
    const auto temporal_pixels = device.read_texture_rgba32f(temporal_resolved.value());
    REQUIRE(temporal_pixels);
    const std::size_t temporal_center = (4U * 8U + 4U) * 4U;
    REQUIRE(temporal_pixels.value()[temporal_center] > 0.49F &&
            temporal_pixels.value()[temporal_center] < 0.51F);
    REQUIRE(temporal_pixels.value()[temporal_center + 1U] > 0.49F &&
            temporal_pixels.value()[temporal_center + 1U] < 0.51F);
    REQUIRE(temporal_pixels.value()[temporal_center + 2U] > 0.49F &&
            temporal_pixels.value()[temporal_center + 2U] < 0.51F);
    REQUIRE(temporal_pixels.value()[temporal_center + 3U] > 0.99F);
    const auto graph_receipt = graph_parity.finish();
    REQUIRE(graph_receipt.pass_order.size() == 6U);
    REQUIRE(graph_receipt.barriers.size() == 4U);
    REQUIRE(graph_receipt.lifetimes.size() == 15U);
    const auto lifetime_for = [&graph_receipt](render_graph::ResourceId resource) {
        return std::find_if(graph_receipt.lifetimes.begin(), graph_receipt.lifetimes.end(),
            [resource](const auto& lifetime) { return lifetime.resource == resource; });
    };
    const auto pbr_lifetime = lifetime_for(pbr_graph_resource);
    REQUIRE(pbr_lifetime != graph_receipt.lifetimes.end());
    REQUIRE(pbr_lifetime->first_pass == 0U && pbr_lifetime->last_pass == 1U);
    const auto temporal_lifetime = lifetime_for(temporal_current_graph_resource);
    REQUIRE(temporal_lifetime != graph_receipt.lifetimes.end());
    REQUIRE(temporal_lifetime->first_pass == 3U && temporal_lifetime->last_pass == 5U);
    REQUIRE(!device.identity().adapter_name.empty() && !device.identity().driver.empty());
    std::cout << "D3D12 render-graph parity: " << device.identity().adapter_name
              << " driver=" << device.identity().driver
              << " passes=" << graph_receipt.pass_order.size()
              << " barriers=" << graph_receipt.barriers.size()
              << " lifetimes=" << graph_receipt.lifetimes.size()
              << " native barriers matched\n";

    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{temporal_resolved.value()}, graph_receipt);
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{temporal_motion.value()}, graph_receipt);
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{temporal_history.value()}, graph_receipt);
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{temporal_current.value()}, graph_receipt);
    REQUIRE(device.destroy_texture(temporal_resolved.value()));
    REQUIRE(device.destroy_texture(temporal_motion.value()));
    REQUIRE(device.destroy_texture(temporal_history.value()));
    REQUIRE(device.destroy_texture(temporal_current.value()));

    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{tone_target.value()}, graph_receipt);
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{depth_target.value()}, graph_receipt);
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{pbr_target.value()}, graph_receipt);
    REQUIRE(device.destroy_texture(tone_target.value()));
    REQUIRE(device.destroy_texture(depth_target.value()));
    REQUIRE(device.destroy_texture(pbr_target.value()));
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{shadow_depth.value()}, graph_receipt);
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{shadow_color.value()}, graph_receipt);
    REQUIRE(device.destroy_texture(shadow_depth.value()));
    REQUIRE(device.destroy_texture(shadow_color.value()));
    for (const auto texture : material_textures) {
        graph_parity.require_destroy_after_last_use(
            device_ir::ResourceHandle{texture}, graph_receipt);
        REQUIRE(device.destroy_texture(texture));
    }
    graph_parity.require_destroy_after_last_use(
        device_ir::ResourceHandle{pbr_timestamps.value()}, graph_receipt);
    REQUIRE(device.destroy_buffer(pbr_timestamps.value()));
    REQUIRE(device.destroy_buffer(index_buffer.value()));
    REQUIRE(device.destroy_buffer(vertex_buffer.value()));
    REQUIRE(device.destroy_sampler(sampler.value()));
    REQUIRE(device.destroy_texture(output.value()));
    REQUIRE(device.destroy_pipeline(compute_pipeline.value()));
    REQUIRE(device.destroy_pipeline(temporal_pipeline.value()));
    REQUIRE(device.destroy_shader(compute_shader.value()));
    REQUIRE(device.destroy_pipeline(tone_pipeline.value()));
    REQUIRE(device.destroy_pipeline(pbr_pipeline.value()));
    REQUIRE(device.destroy_pipeline(shadow_pipeline.value()));
    REQUIRE(device.destroy_shader(tone_fragment));
    REQUIRE(device.destroy_shader(tone_vertex));
    REQUIRE(device.destroy_shader(temporal_compute));
    REQUIRE(device.destroy_shader(shadow_fragment));
    REQUIRE(device.destroy_shader(shadow_vertex));
    REQUIRE(device.destroy_shader(pbr_fragment));
    REQUIRE(device.destroy_shader(pbr_vertex));
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 built-in shader debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

void d3d12_temporal_fp8_matches_e4m3_reference_and_motion_path() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(
        d3d12::D3D12DeviceOptions{{}, false, true});
    REQUIRE(created);
    auto& device = *created.value();

    const auto upload_fp8_texture = [&device](
        gpu::Format format,
        std::span<const std::uint8_t> pixels) -> gpu::TextureHandle {
        REQUIRE(pixels.size() <= 8U);
        const auto upload = device.create_buffer(gpu::BufferDesc{
            256U,
            gpu::MemoryClass::upload,
            gpu::BufferUsage::transfer_source,
        });
        const auto texture = device.create_texture(gpu::TextureDesc{
            2U,
            1U,
            1U,
            1U,
            1U,
            format,
            gpu::TextureDimension::texture_2d,
            gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
        });
        REQUIRE(upload && texture);
        std::array<std::uint8_t, 256U> staging{};
        std::copy(pixels.begin(), pixels.end(), staging.begin());
        REQUIRE(device.write_buffer(upload.value(), 0U, staging));

        device_ir::DeviceCommandStream stream;
        stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{upload.value()},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_source,
        });
        stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture.value()},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::copy_destination,
        });
        stream.append(device_ir::CmdCopyBufferToTexture{
            upload.value(), texture.value(), 0U});
        stream.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture.value()},
            device_ir::ResourceState::copy_destination,
            device_ir::ResourceState::shader_read,
        });
        const auto serial = device.submit(stream, gpu::QueueType::graphics);
        REQUIRE(serial && device.wait(serial.value()));
        REQUIRE(device.destroy_buffer(upload.value()));
        return texture.value();
    };

    constexpr std::array<std::uint8_t, 8U> current_pixels{{
        0x38U, 0x38U, 0x38U, 0x38U,  // 1.0 RGBA
        0x48U, 0x48U, 0x48U, 0x38U,  // 4.0 RGB, 1.0 alpha
    }};
    constexpr std::array<std::uint8_t, 8U> history_pixels{{
        0x40U, 0x40U, 0x40U, 0x38U,  // 2.0 RGBA
        0x50U, 0x50U, 0x50U, 0x38U,  // 8.0 RGB, 1.0 alpha
    }};
    constexpr std::array<std::uint8_t, 4U> moving_pixels{{
        0x00U, 0x00U,  // pixel 0 has zero motion
        0x38U, 0x00U,  // pixel 1 moves one pixel left
    }};
    constexpr std::array<std::uint8_t, 4U> zero_motion_pixels{};
    const auto current = upload_fp8_texture(
        gpu::Format::rgba8_e4m3, current_pixels);
    const auto history = upload_fp8_texture(
        gpu::Format::rgba8_e4m3, history_pixels);
    const auto moving_motion = upload_fp8_texture(
        gpu::Format::rg8_e4m3, moving_pixels);
    const auto zero_motion = upload_fp8_texture(
        gpu::Format::rg8_e4m3, zero_motion_pixels);
    const auto resolved = device.create_texture(gpu::TextureDesc{
        2U,
        1U,
        1U,
        1U,
        1U,
        gpu::Format::rgba8_e4m3,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    const auto readback = device.create_buffer(gpu::BufferDesc{
        256U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    REQUIRE(resolved && readback);

    const auto source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "cartographer_temporal_fp8_acceptance",
        gpu::ShaderStage::compute,
        "CSMain",
        "cartographer_temporal_fp8.hlsl",
        read_d3d12_shader("cartographer_temporal_fp8.hlsl"),
        true,
        false,
    });
    REQUIRE(source);
    const auto shader = device.create_shader(source.value());
    REQUIRE(shader);
    gpu::PipelineDesc descriptor;
    descriptor.debug_name = "cartographer_temporal_fp8_acceptance";
    descriptor.shaders = {shader.value()};
    descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::compute},
    };
    descriptor.sampled_texture_count = 3U;
    descriptor.depth_test = false;
    descriptor.push_constant_bytes = 48U;
    const auto pipeline = device.create_pipeline(descriptor);
    REQUIRE(pipeline);

    struct TemporalConstants {
        std::array<float, 4U> feedback_and_clamp{0.5F, 0.0F, 1.0F, 0.0F};
        std::array<float, 4U> jitter_and_inverse_extent{
            0.0F, 0.0F, 0.5F, 1.0F};
        std::array<std::uint32_t, 4U> history_flags{1U, 0U, 0U, 0U};
    };
    bool resolved_initialized = false;
    bool readback_initialized = false;
    const auto resolve_and_read = [&](gpu::TextureHandle motion,
                                      bool explicitly_zero_motion) {
        TemporalConstants constants;
        constants.history_flags[3U] = explicitly_zero_motion ? 1U : 0U;
        device_ir::DeviceCommandStream compute;
        if (!resolved_initialized) {
            compute.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{resolved.value()},
                device_ir::ResourceState::undefined,
                device_ir::ResourceState::shader_write,
            });
            resolved_initialized = true;
        }
        compute.append(device_ir::CmdBindPipeline{pipeline.value()});
        compute.append(device_ir::CmdBindResources{{
            device_ir::ResourceBinding{
                0U, 0U, device_ir::ResourceHandle{current}, std::nullopt},
            device_ir::ResourceBinding{
                0U, 1U, device_ir::ResourceHandle{history}, std::nullopt},
            device_ir::ResourceBinding{
                0U, 2U, device_ir::ResourceHandle{motion}, std::nullopt},
            device_ir::ResourceBinding{
                0U, 3U, device_ir::ResourceHandle{resolved.value()}, std::nullopt},
        }});
        compute.append(device_ir::CmdPushConstants{bytes_of(constants)});
        compute.append(device_ir::CmdDispatch{1U, 1U, 1U});
        compute.append(device_ir::CmdUavBarrier{
            device_ir::ResourceHandle{resolved.value()}});
        const auto compute_serial = device.submit(
            compute, gpu::QueueType::compute);
        REQUIRE(compute_serial && device.wait(compute_serial.value()));

        device_ir::DeviceCommandStream copy;
        copy.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resolved.value()},
            device_ir::ResourceState::shader_write,
            device_ir::ResourceState::copy_source,
        });
        if (!readback_initialized) {
            copy.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{readback.value()},
                device_ir::ResourceState::undefined,
                device_ir::ResourceState::copy_destination,
            });
            readback_initialized = true;
        }
        copy.append(device_ir::CmdCopyTextureToBuffer{
            resolved.value(), readback.value(), 0U});
        copy.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{resolved.value()},
            device_ir::ResourceState::copy_source,
            device_ir::ResourceState::shader_write,
        });
        const auto copy_serial = device.submit(copy, gpu::QueueType::graphics);
        REQUIRE(copy_serial && device.wait(copy_serial.value()));
        return device.read_buffer(readback.value(), 0U, 8U);
    };

    const auto dynamic_result = resolve_and_read(moving_motion, false);
    REQUIRE(dynamic_result && dynamic_result.value().size() == 8U);
    constexpr std::array<std::uint8_t, 8U> expected_dynamic{{
        0x3CU, 0x3CU, 0x3CU, 0x38U,  // 1.5 RGB, current alpha 1.0
        0x44U, 0x44U, 0x44U, 0x38U,  // motion selects history pixel 0: 3.0
    }};
    REQUIRE(std::equal(
        expected_dynamic.begin(), expected_dynamic.end(),
        dynamic_result.value().begin()));

    const auto static_result = resolve_and_read(zero_motion, true);
    REQUIRE(static_result && static_result.value().size() == 8U);
    constexpr std::array<std::uint8_t, 8U> expected_static{{
        0x3CU, 0x3CU, 0x3CU, 0x38U,  // 1.5 RGB, current alpha 1.0
        0x4CU, 0x4CU, 0x4CU, 0x38U,  // zero-motion path samples pixel 1: 6.0
    }};
    REQUIRE(std::equal(
        expected_static.begin(), expected_static.end(),
        static_result.value().begin()));

    REQUIRE(device.debug_receipts().empty());
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.destroy_texture(resolved.value()));
    REQUIRE(device.destroy_texture(zero_motion));
    REQUIRE(device.destroy_texture(moving_motion));
    REQUIRE(device.destroy_texture(history));
    REQUIRE(device.destroy_texture(current));
    REQUIRE(device.destroy_pipeline(pipeline.value()));
    REQUIRE(device.destroy_shader(shader.value()));
}

void d3d12_accepts_8k_temporal_resolve() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto& device = *created.value();
    REQUIRE(device.capabilities().max_texture_dimension_2d >= 8192U);
    const auto temporal_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "cartographer_8k_temporal",
        gpu::ShaderStage::compute,
        "CSMain",
        "cartographer_temporal.hlsl",
        read_d3d12_shader("cartographer_temporal.hlsl"),
        true,
        false,
    });
    REQUIRE(temporal_source);
    const auto temporal_shader = device.create_shader(temporal_source.value());
    REQUIRE(temporal_shader);
    gpu::PipelineDesc temporal_descriptor;
    temporal_descriptor.debug_name = "cartographer_8k_temporal";
    temporal_descriptor.shaders = {temporal_shader.value()};
    temporal_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 2U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 3U, gpu::ShaderStage::compute},
    };
    temporal_descriptor.sampled_texture_count = 3U;
    temporal_descriptor.depth_test = false;
    temporal_descriptor.push_constant_bytes = 48U;
    const auto temporal_pipeline = device.create_pipeline(temporal_descriptor);
    REQUIRE(temporal_pipeline);

    const auto probe_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "cartographer_8k_probe",
        gpu::ShaderStage::compute,
        "CSMain",
        "cartographer_8k_probe.hlsl",
        R"(
            Texture2D<float4> source_color : register(t0);
            RWTexture2D<float4> probe_color : register(u0);

            [numthreads(1, 1, 1)]
            void CSMain(uint3 dispatch_id : SV_DISPATCHTHREADID) {
                uint width;
                uint height;
                source_color.GetDimensions(width, height);
                probe_color[uint2(0, 0)] =
                    source_color.Load(int3(width / 2, height / 2, 0));
            }
        )",
        true,
        false,
    });
    REQUIRE(probe_source);
    const auto probe_shader = device.create_shader(probe_source.value());
    REQUIRE(probe_shader);
    gpu::PipelineDesc probe_descriptor;
    probe_descriptor.debug_name = "cartographer_8k_probe";
    probe_descriptor.shaders = {probe_shader.value()};
    probe_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
    };
    probe_descriptor.sampled_texture_count = 1U;
    probe_descriptor.depth_test = false;
    const auto probe_pipeline = device.create_pipeline(probe_descriptor);
    REQUIRE(probe_pipeline);

    const auto temporal_current = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rgba16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.2F, 0.4F, 0.6F, 1.0F},
        1.0F,
    });
    const auto temporal_history = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rgba16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.8F, 0.6F, 0.4F, 1.0F},
        1.0F,
    });
    const auto temporal_motion = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rg16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {0.0F, 0.0F, 0.0F, 0.0F},
        1.0F,
    });
    const auto temporal_resolved = device.create_texture(gpu::TextureDesc{
        8192U,
        8192U,
        1U,
        1U,
        1U,
        gpu::Format::rgba16_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::storage,
        {0.0F, 0.0F, 0.0F, 1.0F},
        1.0F,
    });
    const auto probe = device.create_texture(gpu::TextureDesc{
        1U,
        1U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    REQUIRE(temporal_current && temporal_history && temporal_motion && temporal_resolved && probe);

    device_ir::DeviceCommandStream setup;
    const auto clear_input = [&setup](gpu::TextureHandle texture, std::array<float, 4U> color) {
        setup.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture},
            device_ir::ResourceState::undefined,
            device_ir::ResourceState::render_target,
        });
        setup.append(device_ir::CmdBeginRendering{texture, std::nullopt});
        setup.append(device_ir::CmdClearColor{texture, color});
        setup.append(device_ir::CmdEndRendering{});
        setup.append(device_ir::CmdTransitionResource{
            device_ir::ResourceHandle{texture},
            device_ir::ResourceState::render_target,
            device_ir::ResourceState::shader_read,
        });
    };
    clear_input(temporal_current.value(), {0.2F, 0.4F, 0.6F, 1.0F});
    clear_input(temporal_history.value(), {0.8F, 0.6F, 0.4F, 1.0F});
    clear_input(temporal_motion.value(), {0.0F, 0.0F, 0.0F, 0.0F});
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_resolved.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{probe.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    const auto setup_serial = device.submit(setup, gpu::QueueType::graphics);
    REQUIRE(setup_serial);
    REQUIRE(device.wait(setup_serial.value()));

    const std::array<float, 8U> temporal_float_constants = {
        0.5F, 0.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F / 8192.0F, 1.0F / 8192.0F,
    };
    const std::array<std::uint32_t, 4U> temporal_flags = {1U, 0U, 0U, 0U};
    std::array<std::uint8_t, 48U> temporal_constants{};
    std::memcpy(temporal_constants.data(), temporal_float_constants.data(),
                sizeof(temporal_float_constants));
    std::memcpy(temporal_constants.data() + sizeof(temporal_float_constants),
                temporal_flags.data(), sizeof(temporal_flags));
    device_ir::DeviceCommandStream temporal_stream;
    temporal_stream.append(device_ir::CmdBindPipeline{temporal_pipeline.value()});
    temporal_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{temporal_current.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{temporal_history.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 2U, device_ir::ResourceHandle{temporal_motion.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 3U, device_ir::ResourceHandle{temporal_resolved.value()}, std::nullopt},
    }});
    temporal_stream.append(device_ir::CmdPushConstants{bytes_of(temporal_constants)});
    temporal_stream.append(device_ir::CmdDispatch{1024U, 1024U, 1U});
    temporal_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{temporal_resolved.value()}});
    temporal_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{temporal_resolved.value()},
        device_ir::ResourceState::shader_write,
        device_ir::ResourceState::shader_read,
    });
    const auto temporal_serial = device.submit(temporal_stream, gpu::QueueType::compute);
    REQUIRE(temporal_serial);
    REQUIRE(device.wait(temporal_serial.value()));

    device_ir::DeviceCommandStream probe_stream;
    probe_stream.append(device_ir::CmdBindPipeline{probe_pipeline.value()});
    probe_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{temporal_resolved.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{probe.value()}, std::nullopt},
    }});
    probe_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    probe_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{probe.value()}});
    const auto probe_serial = device.submit(probe_stream, gpu::QueueType::compute);
    if (!probe_serial) {
        throw TestFailure("D3D12 8K probe submit: " + probe_serial.error().message);
    }
    REQUIRE(probe_serial);
    REQUIRE(device.wait(probe_serial.value()));
    const auto probe_pixels = device.read_texture_rgba32f(probe.value());
    REQUIRE(probe_pixels);
    REQUIRE(probe_pixels.value().size() == 4U);
    REQUIRE(probe_pixels.value().at(0U) > 0.48F && probe_pixels.value().at(0U) < 0.52F);
    REQUIRE(probe_pixels.value().at(1U) > 0.48F && probe_pixels.value().at(1U) < 0.52F);
    REQUIRE(probe_pixels.value().at(2U) > 0.48F && probe_pixels.value().at(2U) < 0.52F);
    REQUIRE(probe_pixels.value().at(3U) > 0.99F);

    REQUIRE(device.destroy_texture(probe.value()));
    REQUIRE(device.destroy_texture(temporal_resolved.value()));
    REQUIRE(device.destroy_texture(temporal_motion.value()));
    REQUIRE(device.destroy_texture(temporal_history.value()));
    REQUIRE(device.destroy_texture(temporal_current.value()));
    REQUIRE(device.destroy_pipeline(probe_pipeline.value()));
    REQUIRE(device.destroy_shader(probe_shader.value()));
    REQUIRE(device.destroy_pipeline(temporal_pipeline.value()));
    REQUIRE(device.destroy_shader(temporal_shader.value()));
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 8K temporal debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

void d3d12_ibl_irradiance_matches_cpu_reference() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    if (!created) throw TestFailure("D3D12 IBL device: " + created.error().message);
    auto& device = *created.value();

    const auto shader_source = d3d12::compile_hlsl(d3d12::ShaderSource{
        "cartographer_ibl_irradiance",
        gpu::ShaderStage::compute,
        "CSMain",
        "cartographer_ibl_irradiance.hlsl",
        read_d3d12_shader("cartographer_ibl_irradiance.hlsl"),
        true,
        false,
    });
    if (!shader_source) throw TestFailure("D3D12 IBL shader: " + shader_source.error().message);
    const auto shader = device.create_shader(shader_source.value());
    REQUIRE(shader);
    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "cartographer_ibl_irradiance";
    pipeline_descriptor.shaders = {shader.value()};
    pipeline_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute},
    };
    pipeline_descriptor.sampled_texture_count = 1U;
    pipeline_descriptor.depth_test = false;
    const auto pipeline = device.create_pipeline(pipeline_descriptor);
    if (!pipeline) throw TestFailure("D3D12 IBL pipeline: " + pipeline.error().message);

    const std::array<std::uint8_t, 16U> environment_rgba{
        255U, 64U, 0U, 255U,
        0U, 128U, 32U, 255U,
        32U, 0U, 255U, 255U,
        255U, 255U, 255U, 255U,
    };
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[2U] = 2U;
    tga[12U] = 2U;
    tga[14U] = 2U;
    tga[16U] = 32U;
    tga[17U] = 0x28U;
    for (std::size_t offset = 0U; offset < environment_rgba.size(); offset += 4U) {
        tga.insert(tga.end(), {
            environment_rgba[offset + 2U], environment_rgba[offset + 1U],
            environment_rgba[offset], environment_rgba[offset + 3U]});
    }
    const auto decoded = assets::decode_tga_rgba8(tga);
    REQUIRE(decoded);
    const auto mip_chain = assets::generate_rgba8_mips(decoded.value(), {
        .semantic = assets::TextureSemantic::data,
        .color_space = assets::ColorSpace::linear,
    });
    REQUIRE(mip_chain);
    const auto upload_plan = d3d12::build_texture_upload_plan(mip_chain.value());
    REQUIRE(upload_plan);
    auto environment_descriptor = upload_plan.value().texture;
    environment_descriptor.usage = gpu::TextureUsage::sampled |
        gpu::TextureUsage::transfer_destination;
    const auto environment = device.create_texture(environment_descriptor);
    const auto irradiance = device.create_texture(gpu::TextureDesc{
        1U,
        1U,
        1U,
        1U,
        1U,
        gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    const auto upload = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(upload_plan.value().staging_bytes.size()),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    REQUIRE(environment && irradiance && upload);
    REQUIRE(device.write_buffer(upload.value(), 0U, upload_plan.value().staging_bytes));

    device_ir::DeviceCommandStream upload_stream;
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{environment.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    REQUIRE(d3d12::append_texture_upload_copies(
        upload_stream, upload.value(), environment.value(), upload_plan.value()));
    const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
    if (!upload_serial) throw TestFailure("D3D12 IBL upload: " + upload_serial.error().message);
    REQUIRE(device.wait(upload_serial.value()));

    device_ir::DeviceCommandStream transition_stream;
    transition_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{environment.value()},
        device_ir::ResourceState::copy_destination,
        device_ir::ResourceState::shader_read,
    });
    transition_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{irradiance.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    const auto transition_serial = device.submit(transition_stream, gpu::QueueType::graphics);
    if (!transition_serial) {
        throw TestFailure("D3D12 IBL state transition: " + transition_serial.error().message);
    }
    REQUIRE(device.wait(transition_serial.value()));

    device_ir::DeviceCommandStream ibl_stream;
    ibl_stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    ibl_stream.append(device_ir::CmdBindResources{{
        device_ir::ResourceBinding{
            0U, 0U, device_ir::ResourceHandle{environment.value()}, std::nullopt},
        device_ir::ResourceBinding{
            0U, 1U, device_ir::ResourceHandle{irradiance.value()}, std::nullopt},
    }});
    ibl_stream.append(device_ir::CmdDispatch{1U, 1U, 1U});
    ibl_stream.append(device_ir::CmdUavBarrier{
        device_ir::ResourceHandle{irradiance.value()}});
    const auto ibl_serial = device.submit(ibl_stream, gpu::QueueType::compute);
    if (!ibl_serial) throw TestFailure("D3D12 IBL dispatch: " + ibl_serial.error().message);
    REQUIRE(device.wait(ibl_serial.value()));

    const double pi = 3.14159265358979323846;
    std::array<double, 3U> weighted{};
    double total_weight = 0.0;
    for (std::uint32_t y = 0U; y < 2U; ++y) {
        const double theta = pi * (static_cast<double>(y) + 0.5) / 2.0;
        const double weight = std::max(std::cos(theta), 0.0) * std::sin(theta);
        for (std::uint32_t x = 0U; x < 2U; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * 2U + x) * 4U;
            for (std::size_t channel = 0U; channel < weighted.size(); ++channel) {
                weighted[channel] +=
                    static_cast<double>(decoded.value().pixels[offset + channel]) / 255.0 * weight;
            }
            total_weight += weight;
        }
    }
    REQUIRE(total_weight > 0.0);
    const auto pixels = device.read_texture_rgba32f(irradiance.value());
    REQUIRE(pixels && pixels.value().size() == 4U);
    for (std::size_t channel = 0U; channel < weighted.size(); ++channel) {
        const double reference = weighted[channel] * pi / total_weight;
        REQUIRE(std::abs(static_cast<double>(pixels.value()[channel]) - reference) < 0.01);
    }
    REQUIRE(std::abs(pixels.value()[3U] - 1.0F) < 0.001F);
    REQUIRE(device.debug_receipts().empty());
    REQUIRE(device.destroy_texture(irradiance.value()));
    REQUIRE(device.destroy_texture(environment.value()));
    REQUIRE(device.destroy_buffer(upload.value()));
    REQUIRE(device.destroy_pipeline(pipeline.value()));
    REQUIRE(device.destroy_shader(shader.value()));
}

LRESULT CALLBACK d3d12_test_window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

void d3d12_exercises_hidden_swapchain_resize_and_present() {
    using namespace carto;

    auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto device = std::move(created.value());

    const auto compile_and_create = [&device](
        std::string_view name,
        gpu::ShaderStage stage,
        std::string_view entry_point,
        std::string source_text) {
        const auto source = d3d12::compile_hlsl(d3d12::ShaderSource{
            std::string(name), stage, std::string(entry_point),
            std::string(name), std::move(source_text), true, false});
        if (!source) {
            throw TestFailure("D3D12 presentation shader " + std::string(name) + ": " +
                              source.error().message);
        }
        const auto shader = device->create_shader(source.value());
        if (!shader) {
            throw TestFailure("D3D12 presentation shader create " + std::string(name) +
                              ": " + shader.error().message);
        }
        return shader.value();
    };
    const auto tone_vertex = compile_and_create(
        "cartographer_tonemap.hlsl", gpu::ShaderStage::vertex, "VSMain",
        read_d3d12_shader("cartographer_tonemap.hlsl"));
    const auto tone_fragment = compile_and_create(
        "cartographer_tonemap.hlsl", gpu::ShaderStage::fragment, "PSMain",
        read_d3d12_shader("cartographer_tonemap.hlsl"));
    gpu::PipelineDesc tone_descriptor;
    tone_descriptor.debug_name = "cartographer_swapchain_tonemap";
    tone_descriptor.shaders = {tone_vertex, tone_fragment};
    tone_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::fragment}};
    tone_descriptor.sampled_texture_count = 1U;
    tone_descriptor.cull_mode = gpu::CullMode::none;
    tone_descriptor.color_format = gpu::Format::bgra8_unorm;
    tone_descriptor.depth_format = gpu::Format::unknown;
    tone_descriptor.depth_test = false;
    tone_descriptor.depth_write = false;
    tone_descriptor.push_constant_bytes = 16U;
    const auto tone_pipeline = device->create_pipeline(tone_descriptor);
    if (!tone_pipeline) {
        throw TestFailure("D3D12 swapchain tone-map pipeline: " +
                          tone_pipeline.error().message);
    }

    const auto probe_shader = compile_and_create(
        "cartographer_swapchain_pixel_probe.hlsl", gpu::ShaderStage::compute, "CSMain",
        R"(
            Texture2D<float4> source_color : register(t0);
            RWTexture2D<float4> probe_output : register(u0);
            [numthreads(1, 1, 1)]
            void CSMain(uint3 dispatch_id : SV_DISPATCHTHREADID) {
                uint width, height;
                source_color.GetDimensions(width, height);
                probe_output[uint2(0, 0)] = source_color.Load(
                    int3(width / 2, height / 2, 0));
            }
        )");
    gpu::PipelineDesc probe_descriptor;
    probe_descriptor.debug_name = "cartographer_swapchain_pixel_probe";
    probe_descriptor.shaders = {probe_shader};
    probe_descriptor.descriptor_bindings = {
        gpu::DescriptorBinding{0U, 0U, gpu::ShaderStage::compute},
        gpu::DescriptorBinding{0U, 1U, gpu::ShaderStage::compute}};
    probe_descriptor.sampled_texture_count = 1U;
    probe_descriptor.depth_test = false;
    const auto probe_pipeline = device->create_pipeline(probe_descriptor);
    if (!probe_pipeline) {
        throw TestFailure("D3D12 swapchain pixel-probe pipeline: " +
                          probe_pipeline.error().message);
    }

    const auto hdr_source = device->create_texture(gpu::TextureDesc{
        1U, 1U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment,
        {2.0F, 0.5F, 0.25F, 1.0F},
    });
    const auto probe_output = device->create_texture(gpu::TextureDesc{
        1U, 1U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::storage | gpu::TextureUsage::transfer_source,
    });
    const std::array<float, 12U> dummy_vertices{};
    const auto vertex_buffer = device->create_buffer(gpu::BufferDesc{
        sizeof(dummy_vertices), gpu::MemoryClass::upload,
        gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source});
    const auto timestamp_buffer = device->create_buffer(gpu::BufferDesc{
        2U * sizeof(std::uint64_t), gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination});
    gpu::SamplerDesc sampler_descriptor;
    sampler_descriptor.min_filter = gpu::Filter::linear;
    sampler_descriptor.mag_filter = gpu::Filter::linear;
    sampler_descriptor.mip_filter = gpu::MipFilter::nearest;
    const auto sampler = device->create_sampler(sampler_descriptor);
    REQUIRE(hdr_source && probe_output && vertex_buffer && timestamp_buffer && sampler);
    REQUIRE(device->write_buffer(vertex_buffer.value(), 0U, bytes_of(dummy_vertices)));

    device_ir::DeviceCommandStream setup;
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{hdr_source.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    setup.append(device_ir::CmdBeginRendering{hdr_source.value(), std::nullopt});
    setup.append(device_ir::CmdClearColor{hdr_source.value(), {2.0F, 0.5F, 0.25F, 1.0F}});
    setup.append(device_ir::CmdEndRendering{});
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{hdr_source.value()},
        device_ir::ResourceState::render_target,
        device_ir::ResourceState::shader_read,
    });
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{probe_output.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::shader_write,
    });
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    setup.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{timestamp_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    const auto setup_serial = device->submit(setup, gpu::QueueType::graphics);
    if (!setup_serial) {
        throw TestFailure("D3D12 presentation setup submit: " + setup_serial.error().message);
    }
    REQUIRE(device->wait(setup_serial.value()));

    const auto srgb_encode = [](float value) {
        value = std::clamp(value, 0.0F, 1.0F);
        return value <= 0.0031308F
            ? value * 12.92F
            : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    };
    const auto aces = [](float value) {
        value = std::max(value, 0.0F);
        return std::clamp(
            (value * (2.51F * value + 0.03F)) /
                (value * (2.43F * value + 0.59F) + 0.14F),
            0.0F, 1.0F);
    };
    const std::array<float, 4U> expected{
        srgb_encode(aces(2.0F)),
        srgb_encode(aces(0.5F)),
        srgb_encode(aces(0.25F)),
        1.0F,
    };

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t class_name[] = L"CartographerD3D12AcceptanceWindow";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = d3d12_test_window_proc;
    window_class.lpszClassName = class_name;
    const ATOM atom = RegisterClassW(&window_class);
    REQUIRE(atom != 0U);
    HWND window = CreateWindowExW(
        0U,
        class_name,
        L"Cartographer D3D12 acceptance",
        WS_POPUP,
        0,
        0,
        1280,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);
    REQUIRE(window != nullptr);

    {
        auto created_swapchain = d3d12::D3D12Swapchain::create(
            *device,
            d3d12::D3D12SwapchainOptions{
                reinterpret_cast<std::uintptr_t>(window),
                1280U,
                800U,
                2U,
                gpu::Format::bgra8_unorm,
                false});
        REQUIRE(created_swapchain);
        auto swapchain = std::move(created_swapchain.value());
        const auto render_frame = [&](bool measure_gpu_time = false) {
            const auto back_buffer = swapchain->acquire_back_buffer();
            REQUIRE(back_buffer);
            device_ir::DeviceCommandStream render;
            render.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{back_buffer.value()},
                device_ir::ResourceState::present,
                device_ir::ResourceState::render_target,
            });
            if (measure_gpu_time) render.append(device_ir::CmdWriteTimestamp{0U});
            render.append(device_ir::CmdBeginRendering{back_buffer.value(), std::nullopt});
            render.append(device_ir::CmdSetViewport{
                0.0F, 0.0F, static_cast<float>(swapchain->width()),
                static_cast<float>(swapchain->height()), 0.0F, 1.0F});
            render.append(device_ir::CmdSetScissor{
                0, 0, swapchain->width(), swapchain->height()});
            render.append(device_ir::CmdBindPipeline{tone_pipeline.value()});
            render.append(device_ir::CmdBindVertexBuffer{vertex_buffer.value(), 0U, 16U});
            render.append(device_ir::CmdBindResources{{device_ir::ResourceBinding{
                0U, 0U, device_ir::ResourceHandle{hdr_source.value()}, sampler.value()}}});
            const std::array<float, 4U> tone_constants{0.0F, 0.0F, 0.0F, 0.0F};
            render.append(device_ir::CmdPushConstants{bytes_of(tone_constants)});
            render.append(device_ir::CmdDraw{3U, 1U, 0U, 0U});
            render.append(device_ir::CmdEndRendering{});
            if (measure_gpu_time) {
                render.append(device_ir::CmdWriteTimestamp{1U});
                render.append(device_ir::CmdResolveTimestamps{
                    timestamp_buffer.value(), 0U, 2U, 0U});
            }
            render.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{back_buffer.value()},
                device_ir::ResourceState::render_target,
                device_ir::ResourceState::shader_read,
            });
            const auto render_serial = device->submit(render, gpu::QueueType::graphics);
            if (!render_serial) {
                throw TestFailure("D3D12 swapchain render submit: " +
                                  render_serial.error().message);
            }
            REQUIRE(device->wait(render_serial.value()));

            device_ir::DeviceCommandStream probe;
            probe.append(device_ir::CmdBindPipeline{probe_pipeline.value()});
            probe.append(device_ir::CmdBindResources{{
                device_ir::ResourceBinding{
                    0U, 0U, device_ir::ResourceHandle{back_buffer.value()}, std::nullopt},
                device_ir::ResourceBinding{
                    0U, 1U, device_ir::ResourceHandle{probe_output.value()}, std::nullopt},
            }});
            probe.append(device_ir::CmdDispatch{1U, 1U, 1U});
            probe.append(device_ir::CmdUavBarrier{
                device_ir::ResourceHandle{probe_output.value()}});
            const auto probe_serial = device->submit(probe, gpu::QueueType::compute);
            if (!probe_serial) {
                throw TestFailure("D3D12 swapchain probe submit: " +
                                  probe_serial.error().message);
            }
            REQUIRE(device->wait(probe_serial.value()));
            const auto pixel = device->read_texture_rgba32f(probe_output.value());
            REQUIRE(pixel && pixel.value().size() == 4U);
            for (std::size_t channel = 0U; channel < expected.size(); ++channel) {
                if (std::abs(pixel.value()[channel] - expected[channel]) > 0.01F) {
                    throw TestFailure(
                        "D3D12 presented backbuffer tone-map mismatch at channel " +
                        std::to_string(channel) + ": expected " +
                        std::to_string(expected[channel]) + " received " +
                        std::to_string(pixel.value()[channel]));
                }
            }
            device_ir::DeviceCommandStream finish;
            finish.append(device_ir::CmdTransitionResource{
                device_ir::ResourceHandle{back_buffer.value()},
                device_ir::ResourceState::shader_read,
                device_ir::ResourceState::present,
            });
            const auto finish_serial = device->submit(finish, gpu::QueueType::graphics);
            if (!finish_serial) {
                throw TestFailure("D3D12 swapchain present transition: " +
                                  finish_serial.error().message);
            }
            REQUIRE(device->wait(finish_serial.value()));
            REQUIRE(device->destroy_texture(back_buffer.value()));
            REQUIRE(swapchain->present(true));
            std::cout << "D3D12 presented tone-map: " << swapchain->width() << "x"
                      << swapchain->height() << " center=" << pixel.value()[0U] << ","
                      << pixel.value()[1U] << "," << pixel.value()[2U] << "\n";
            if (measure_gpu_time) {
                const auto timestamp_bytes = device->read_buffer(
                    timestamp_buffer.value(), 0U, 2U * sizeof(std::uint64_t));
                REQUIRE(timestamp_bytes &&
                        timestamp_bytes.value().size() == 2U * sizeof(std::uint64_t));
                std::array<std::uint64_t, 2U> timestamps{};
                std::memcpy(timestamps.data(), timestamp_bytes.value().data(),
                            timestamp_bytes.value().size());
                REQUIRE(timestamps[1U] >= timestamps[0U]);
                const auto frequency = device->timestamp_frequency(gpu::QueueType::graphics);
                REQUIRE(frequency != 0U);
                const long double gpu_microseconds =
                    static_cast<long double>(timestamps[1U] - timestamps[0U]) *
                    1'000'000.0L / static_cast<long double>(frequency);
                std::cout << "D3D12 8K presentation timing: "
                          << device->identity().adapter_name
                          << " driver=" << device->identity().driver
                          << " settings=8192x8192 BGRA8_UNORM flip-discard vsync-on "
                          << "GPU-tone-map="
                          << static_cast<std::uint64_t>(gpu_microseconds)
                          << " us; limit=hidden-window solid HDR test pattern, "
                          << "one measured frame\n";
            }
        };

        render_frame();
        REQUIRE(swapchain->resize(640U, 480U));
        REQUIRE(swapchain->width() == 640U);
        REQUIRE(swapchain->height() == 480U);
        render_frame();
        REQUIRE(!swapchain->resize(0U, 480U));
        REQUIRE(swapchain->resize(8192U, 8192U));
        REQUIRE(swapchain->width() == 8192U);
        REQUIRE(swapchain->height() == 8192U);
        render_frame(true);
    }
    DestroyWindow(window);
    REQUIRE(UnregisterClassW(class_name, instance) != 0);
    REQUIRE(device->destroy_texture(probe_output.value()));
    REQUIRE(device->destroy_texture(hdr_source.value()));
    REQUIRE(device->destroy_buffer(vertex_buffer.value()));
    REQUIRE(device->destroy_buffer(timestamp_buffer.value()));
    REQUIRE(device->destroy_sampler(sampler.value()));
    REQUIRE(device->destroy_pipeline(probe_pipeline.value()));
    REQUIRE(device->destroy_shader(probe_shader));
    REQUIRE(device->destroy_pipeline(tone_pipeline.value()));
    REQUIRE(device->destroy_shader(tone_fragment));
    REQUIRE(device->destroy_shader(tone_vertex));
    if (!device->debug_receipts().empty()) {
        std::string message = "D3D12 swapchain debug receipts:";
        for (const auto& receipt : device->debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

void d3d12_uploads_and_reads_specific_texture_mips() {
    using namespace carto;

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    if (!created) {
        throw TestFailure("D3D12 mip device create: " + created.error().message);
    }
    auto& device = *created.value();

    const std::array<std::uint8_t, 16U> rgba{
        255U, 0U, 0U, 255U,
        0U, 255U, 0U, 255U,
        0U, 0U, 255U, 255U,
        255U, 255U, 255U, 255U,
    };
    std::vector<std::uint8_t> tga(18U, 0U);
    tga[2U] = 2U;
    tga[12U] = 2U;
    tga[14U] = 2U;
    tga[16U] = 32U;
    tga[17U] = 0x28U;
    for (std::size_t offset = 0U; offset < rgba.size(); offset += 4U) {
        tga.insert(tga.end(), {
            rgba[offset + 2U], rgba[offset + 1U], rgba[offset], rgba[offset + 3U]});
    }
    const auto decoded = assets::decode_tga_rgba8(tga);
    REQUIRE(decoded);
    const auto chain = assets::generate_rgba8_mips(decoded.value(), {
        .semantic = assets::TextureSemantic::data,
        .color_space = assets::ColorSpace::linear,
    });
    REQUIRE(chain);
    const auto plan = d3d12::build_texture_upload_plan(chain.value());
    REQUIRE(plan);
    REQUIRE(plan.value().footprints.size() == 2U);
    REQUIRE(plan.value().footprints[0U].offset == 0U);
    REQUIRE(plan.value().footprints[0U].row_pitch == 256U);
    REQUIRE(plan.value().footprints[1U].offset == 512U);
    REQUIRE(plan.value().staging_bytes.size() == 768U);
    REQUIRE(plan.value().resident_bytes == 20U);
    auto tampered_plan = plan.value();
    tampered_plan.staging_bytes.front() ^= 1U;
    REQUIRE(!tampered_plan.validate());

    const auto upload = device.create_buffer(gpu::BufferDesc{
        static_cast<std::uint64_t>(plan.value().staging_bytes.size()),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::transfer_source,
    });
    const auto readback = device.create_buffer(gpu::BufferDesc{
        256U,
        gpu::MemoryClass::readback,
        gpu::BufferUsage::transfer_destination,
    });
    auto texture_descriptor = plan.value().texture;
    texture_descriptor.usage =
        texture_descriptor.usage | gpu::TextureUsage::transfer_source;
    const auto texture = device.create_texture(texture_descriptor);
    REQUIRE(upload && readback && texture);
    REQUIRE(device.write_buffer(upload.value(), 0U, plan.value().staging_bytes));

    device_ir::DeviceCommandStream upload_stream;
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{upload.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_source,
    });
    upload_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    REQUIRE(d3d12::append_texture_upload_copies(
        upload_stream, upload.value(), texture.value(), plan.value()));
    const auto upload_serial = device.submit(upload_stream, gpu::QueueType::copy);
    if (!upload_serial) {
        throw TestFailure("D3D12 mip upload submit: " + upload_serial.error().message);
    }
    REQUIRE(device.wait(upload_serial.value()));

    device_ir::DeviceCommandStream readback_stream;
    readback_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::copy_destination,
        device_ir::ResourceState::copy_source,
    });
    readback_stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{readback.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::copy_destination,
    });
    readback_stream.append(device_ir::CmdCopyTextureToBuffer{
        texture.value(), readback.value(), 0U, 1U});
    const auto serial = device.submit(readback_stream, gpu::QueueType::copy);
    if (!serial) {
        throw TestFailure("D3D12 mip upload/readback submit: " + serial.error().message);
    }
    REQUIRE(device.wait(serial.value()));
    const auto& terminal_mip = chain.value().levels.back().pixels;
    const auto pixels = device.read_buffer(readback.value(), 0U, terminal_mip.size());
    REQUIRE(pixels);
    if (!std::equal(terminal_mip.begin(), terminal_mip.end(), pixels.value().begin())) {
        throw TestFailure(
            "D3D12 mip-1 readback mismatch: expected " +
            std::to_string(terminal_mip[0U]) + "," +
            std::to_string(terminal_mip[1U]) + "," +
            std::to_string(terminal_mip[2U]) + "," +
            std::to_string(terminal_mip[3U]) + " received " +
            std::to_string(pixels.value()[0U]) + "," +
            std::to_string(pixels.value()[1U]) + "," +
            std::to_string(pixels.value()[2U]) + "," +
            std::to_string(pixels.value()[3U]));
    }

    device_ir::DeviceCommandStream rejected_role_flip;
    rejected_role_flip.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::copy_source,
        device_ir::ResourceState::copy_destination,
    });
    rejected_role_flip.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{texture.value()},
        device_ir::ResourceState::copy_destination,
        device_ir::ResourceState::copy_source,
    });
    const auto rejected_serial = device.submit(
        rejected_role_flip, gpu::QueueType::copy);
    REQUIRE(!rejected_serial);
    REQUIRE(rejected_serial.error().message.find("submission boundary") !=
            std::string::npos);

    REQUIRE(device.destroy_texture(texture.value()));
    REQUIRE(device.destroy_buffer(readback.value()));
    REQUIRE(device.destroy_buffer(upload.value()));
    if (!device.debug_receipts().empty()) {
        std::string message = "D3D12 mip upload debug receipts:";
        for (const auto& receipt : device.debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }
}

void d3d12_bake_graph_builds_artifact_from_gpu_projection() {
    using namespace carto;
    auto source = make_bake_mesh(false);
    auto target = make_bake_mesh(true);
    const auto source_binding = production::bind_bake_source("project/asset/high", source);
    const auto target_binding = production::bind_bake_target("project/asset/target", target, 7U);
    REQUIRE(source_binding && target_binding);
    production::BakeRecipe recipe;
    recipe.identity = "bake/target/normal";
    recipe.source = source_binding.value();
    recipe.target = target_binding.value();
    recipe.target_uv_set = 7U;
    recipe.width = 8U;
    recipe.height = 8U;
    recipe.projection_half_extent = 0.5;
    recipe.reject_projection_misses = false;
    REQUIRE(recipe.validate());

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, false});
    if (!created) throw TestFailure("D3D12 bake device: " + created.error().message);
    auto& device = *created.value();
    const auto reference = production::compile_tangent_normal_map_cpu(recipe, source, target);
    const auto gpu = d3d12::compile_tangent_normal_map_d3d12(device, recipe, source, target);
    REQUIRE(reference.succeeded());
    REQUIRE(gpu.succeeded());
    REQUIRE(gpu.projection.has_value());
    REQUIRE(gpu.ray_worker_contract.has_value());
    REQUIRE(gpu.ray_worker_contract->succeeded());
    REQUIRE(gpu.ray_worker_contract->worker.evidence_class ==
        (device.is_software_adapter()
            ? production::BakeRayEvidenceClass::software
            : production::BakeRayEvidenceClass::hardware));
    REQUIRE(gpu.projection->worker_version == d3d12::kD3D12ProjectionWorkerVersion);
    REQUIRE(gpu.bake.artifact->rgba8 == reference.artifact->rgba8);
    REQUIRE(gpu.bake.artifact->artifact_digest == reference.artifact->artifact_digest);
    REQUIRE(gpu.cpu_reference_artifact_digest == reference.artifact->artifact_digest);
    REQUIRE(gpu.projection->source_digest == recipe.source.content_digest);
    REQUIRE(gpu.projection->target_digest == recipe.target.content_digest);
    REQUIRE(gpu.projection->settings_digest == gpu.bake.artifact->settings_digest);

    TempDirectory publication_directory;
    assets::BlobStore blobs(publication_directory.path() / "blobs");
    production::ProductionArtifactStore store(blobs);
    const auto platform_profile_result =
        d3d12::d3d12_bake_platform_profile(*gpu.projection);
    REQUIRE(platform_profile_result);
    const std::string platform_profile = platform_profile_result.value();
    REQUIRE(d3d12::d3d12_bake_platform_profile(*gpu.projection).value() == platform_profile);
    const auto payload = gpu.bake.artifact->serialize();
    REQUIRE(payload);
    const auto cook_key = production::bake_production_cook_key(
        recipe, production::BakeNormalSpace::tangent, platform_profile);
    REQUIRE(cook_key);
    production::ProductionArtifactReceipt receipt;
    receipt.key = cook_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = payload.value().size();
    receipt.quality.measured_error = 0.0;
    receipt.quality.importance = 1.0;
    receipt.quality.confidence = 1.0;
    receipt.cost.gpu_milliseconds =
        static_cast<double>(gpu.projection->wall_time_nanoseconds) / 1'000'000.0;
    receipt.cost.disk_bytes = payload.value().size();
    REQUIRE(receipt.refresh_digest());
    production::AdmissionPolicy policy;
    policy.max_measured_error = 0.0;
    policy.min_confidence = 1.0;
    REQUIRE(!d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, gpu, source, target, policy, platform_profile + ".wrong"));
    const auto no_prior_artifact = store.find(receipt.key);
    REQUIRE(no_prior_artifact);
    REQUIRE(!no_prior_artifact.value().has_value());
    const auto published = d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, gpu, source, target, policy, platform_profile);
    if (!published) {
        throw TestFailure("D3D12 normal-map publication: " + published.error().message);
    }
    const auto restored = production::read_normal_map(store, recipe,
        production::BakeNormalSpace::tangent, source, target, policy,
        platform_profile);
    REQUIRE(restored);
    REQUIRE(restored.value().artifact_digest == gpu.bake.artifact->artifact_digest);

    auto tampered_projection_report = gpu;
    tampered_projection_report.projection->hits.front().hit =
        !tampered_projection_report.projection->hits.front().hit;
    REQUIRE(!tampered_projection_report.projection->output_digest_matches());
    REQUIRE(!tampered_projection_report.succeeded());
    REQUIRE(!d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, tampered_projection_report, source, target, policy,
        platform_profile));

    auto relabelled_projection_provenance = gpu;
    relabelled_projection_provenance.projection->stable_adapter_id += ".spoofed";
    REQUIRE(relabelled_projection_provenance.projection->output_digest_matches());
    REQUIRE(!relabelled_projection_provenance.projection->evidence_digest_matches());
    REQUIRE(!relabelled_projection_provenance.succeeded());
    REQUIRE(!d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, relabelled_projection_provenance, source, target, policy,
        platform_profile));

    auto tampered_report = gpu;
    tampered_report.bake.artifact->rgba8.front() ^= 0x01U;
    REQUIRE(!d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, tampered_report, source, target, policy,
        platform_profile));

    auto tampered_contract = gpu;
    tampered_contract.ray_worker_contract->worker.worker_id += ".relabelled";
    REQUIRE(!tampered_contract.succeeded());
    REQUIRE(!d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, tampered_contract, source, target, policy,
        platform_profile));
    const auto retained = production::read_normal_map(store, recipe,
        production::BakeNormalSpace::tangent, source, target, policy,
        platform_profile);
    REQUIRE(retained);
    REQUIRE(retained.value().artifact_digest == gpu.bake.artifact->artifact_digest);

    auto stale_publication_source = source;
    REQUIRE(stale_publication_source.set_vertex_position(
        geometry::VertexId{1U}, {0.1, 0.0, 0.05}));
    REQUIRE(!d3d12::publish_tangent_normal_map_d3d12(
        store, receipt, gpu, stale_publication_source, target, policy,
        platform_profile));
    REQUIRE(!production::read_normal_map(store, recipe,
        production::BakeNormalSpace::tangent, stale_publication_source, target, policy,
        platform_profile));

    auto stale_source = make_bake_mesh(false, true);
    const auto stale = d3d12::compile_tangent_normal_map_d3d12(
        device, recipe, stale_source, target);
    REQUIRE(!stale.succeeded());
    REQUIRE(!stale.projection.has_value());
    REQUIRE(std::any_of(stale.bake.diagnostics.begin(), stale.bake.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "bake_input_invalid" && diagnostic.is_error;
        }));

    auto backface_source = make_bake_mesh(false, true);
    const auto backface_binding = production::bind_bake_source(
        "project/asset/high-backface", backface_source);
    REQUIRE(backface_binding);
    auto front_only_recipe = recipe;
    front_only_recipe.identity = "bake/target/front-only";
    front_only_recipe.source = backface_binding.value();
    front_only_recipe.backface_policy = production::BakeBackfacePolicy::front_only;
    const auto backface = d3d12::compile_tangent_normal_map_d3d12(
        device, front_only_recipe, backface_source, target);
    REQUIRE(backface.succeeded());
    REQUIRE(backface.bake.projection_backface_rejections > 0U);
    REQUIRE(backface.bake.projection_misses == backface.bake.covered_texels);
    REQUIRE(backface.projection->hits.front().has_rejected_backface);

    auto crossing_source = make_bake_mesh(false, false, -0.12);
    const auto crossing_binding = production::bind_bake_source(
        "project/asset/crossing-high", crossing_source);
    REQUIRE(crossing_binding);
    production::BakeRecipe crossing_recipe = recipe;
    crossing_recipe.identity = "bake/target/cage-crossing";
    crossing_recipe.source = crossing_binding.value();
    const auto crossing_reference = production::compile_tangent_normal_map_cpu(
        crossing_recipe, crossing_source, target);
    const auto crossing_gpu = d3d12::compile_tangent_normal_map_d3d12(
        device, crossing_recipe, crossing_source, target);
    REQUIRE(crossing_reference.succeeded());
    REQUIRE(crossing_gpu.succeeded());
    const auto has_cage_warning = [](const production::BakeExecutionReport& report) {
        return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
            [](const production::BakeDiagnostic& diagnostic) {
                return diagnostic.code == "projection_cage_intersection" &&
                    !diagnostic.is_error;
            });
    };
    REQUIRE(has_cage_warning(crossing_reference));
    REQUIRE(has_cage_warning(crossing_gpu.bake));
    const auto cage_warning_count = [](const production::BakeExecutionReport& report) {
        return std::count_if(report.diagnostics.begin(), report.diagnostics.end(),
            [](const production::BakeDiagnostic& diagnostic) {
                return diagnostic.code == "projection_cage_intersection";
            });
    };
    REQUIRE(cage_warning_count(crossing_reference) ==
        cage_warning_count(crossing_gpu.bake));
}

void d3d12_ambient_occlusion_uses_gpu_rays_and_fails_closed_on_stale_inputs() {
    using namespace carto;
    auto source = make_bake_mesh(false);
    auto target = make_bake_mesh(true);
    const auto source_binding = production::bind_bake_source("project/asset/ao-high", source);
    const auto target_binding = production::bind_bake_target("project/asset/ao-target", target, 7U);
    REQUIRE(source_binding && target_binding);
    production::AmbientOcclusionRecipe recipe;
    recipe.binding.identity = "bake/target/ao";
    recipe.binding.source = source_binding.value();
    recipe.binding.target = target_binding.value();
    recipe.binding.target_uv_set = 7U;
    recipe.binding.width = 8U;
    recipe.binding.height = 8U;
    recipe.sample_count = 8U;
    recipe.max_distance = 0.5;
    recipe.origin_bias = 1e-4;
    recipe.seed = 0x6f63636c7573696fULL;
    REQUIRE(recipe.validate());

    const auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, false});
    if (!created) throw TestFailure("D3D12 AO device: " + created.error().message);
    auto& device = *created.value();
    const auto reference = production::compile_ambient_occlusion_cpu(recipe, source, target);
    const auto gpu = d3d12::compile_ambient_occlusion_d3d12(
        device, recipe, source, target);
    REQUIRE(reference.succeeded());
    REQUIRE(gpu.succeeded());
    REQUIRE(gpu.projection.has_value());
    REQUIRE(gpu.projection->worker_version == d3d12::kD3D12ProjectionWorkerVersion);
    REQUIRE(gpu.projection->hits.size() == reference.artifact->rays_cast);
    REQUIRE(gpu.bake.artifact->rgba8 == reference.artifact->rgba8);
    REQUIRE(gpu.bake.artifact->artifact_digest == reference.artifact->artifact_digest);
    REQUIRE(gpu.cpu_reference_artifact_digest == reference.artifact->artifact_digest);
    REQUIRE(gpu.projection->source_digest == recipe.binding.source.content_digest);
    REQUIRE(gpu.projection->target_digest == recipe.binding.target.content_digest);
    REQUIRE(gpu.projection->settings_digest == recipe.canonical_digest().value());

    TempDirectory publication_directory;
    assets::BlobStore blobs(publication_directory.path() / "blobs");
    production::ProductionArtifactStore store(blobs);
    const auto platform_profile_result =
        d3d12::d3d12_bake_platform_profile(*gpu.projection);
    REQUIRE(platform_profile_result);
    const std::string platform_profile = platform_profile_result.value();
    const auto payload = gpu.bake.artifact->serialize();
    REQUIRE(payload);
    const auto cook_key = production::bake_ambient_occlusion_cook_key(
        recipe, platform_profile);
    REQUIRE(cook_key);
    production::ProductionArtifactReceipt receipt;
    receipt.key = cook_key.value();
    receipt.status = production::ReceiptStatus::admitted;
    receipt.output_digest = assets::sha256(payload.value());
    receipt.output_bytes = payload.value().size();
    receipt.quality.measured_error = 0.0;
    receipt.quality.importance = 1.0;
    receipt.quality.confidence = 1.0;
    receipt.cost.gpu_milliseconds =
        static_cast<double>(gpu.projection->wall_time_nanoseconds) / 1'000'000.0;
    receipt.cost.disk_bytes = payload.value().size();
    REQUIRE(receipt.refresh_digest());
    production::AdmissionPolicy policy;
    policy.max_measured_error = 0.0;
    policy.min_confidence = 1.0;
    REQUIRE(!d3d12::publish_ambient_occlusion_map_d3d12(
        store, receipt, gpu, source, target, policy, platform_profile + ".wrong"));
    const auto no_prior_artifact = store.find(receipt.key);
    REQUIRE(no_prior_artifact);
    REQUIRE(!no_prior_artifact.value().has_value());
    REQUIRE(d3d12::publish_ambient_occlusion_map_d3d12(
        store, receipt, gpu, source, target, policy, platform_profile));
    const auto restored = production::read_ambient_occlusion_map(
        store, recipe, source, target, policy, platform_profile);
    REQUIRE(restored);
    REQUIRE(restored.value().artifact_digest == gpu.bake.artifact->artifact_digest);

    auto tampered_projection_report = gpu;
    tampered_projection_report.projection->hits.front().hit =
        !tampered_projection_report.projection->hits.front().hit;
    REQUIRE(!tampered_projection_report.projection->output_digest_matches());
    REQUIRE(!tampered_projection_report.succeeded());
    REQUIRE(!d3d12::publish_ambient_occlusion_map_d3d12(
        store, receipt, tampered_projection_report, source, target, policy, platform_profile));

    auto relabelled_projection_provenance = gpu;
    relabelled_projection_provenance.projection->driver += ".spoofed";
    REQUIRE(relabelled_projection_provenance.projection->output_digest_matches());
    REQUIRE(!relabelled_projection_provenance.projection->evidence_digest_matches());
    REQUIRE(!relabelled_projection_provenance.succeeded());
    REQUIRE(!d3d12::publish_ambient_occlusion_map_d3d12(
        store, receipt, relabelled_projection_provenance, source, target, policy,
        platform_profile));

    auto stale_source = source;
    REQUIRE(stale_source.set_vertex_position(geometry::VertexId{1U}, {0.1, 0.0, 0.05}));
    REQUIRE(!d3d12::publish_ambient_occlusion_map_d3d12(
        store, receipt, gpu, stale_source, target, policy, platform_profile));
    REQUIRE(!production::read_ambient_occlusion_map(
        store, recipe, stale_source, target, policy, platform_profile));
    const auto stale_compile = d3d12::compile_ambient_occlusion_d3d12(
        device, recipe, stale_source, target);
    REQUIRE(!stale_compile.succeeded());
    REQUIRE(!stale_compile.projection.has_value());
    REQUIRE(std::any_of(stale_compile.bake.diagnostics.begin(), stale_compile.bake.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "bake_input_invalid" && diagnostic.is_error;
        }));

    auto tampered_report = gpu;
    tampered_report.bake.artifact->rgba8.front() ^= 0x01U;
    REQUIRE(!d3d12::publish_ambient_occlusion_map_d3d12(
        store, receipt, tampered_report, source, target, policy, platform_profile));
    const auto retained = production::read_ambient_occlusion_map(
        store, recipe, source, target, policy, platform_profile);
    REQUIRE(retained);
    REQUIRE(retained.value().artifact_digest == gpu.bake.artifact->artifact_digest);
    REQUIRE(device.debug_receipts().empty());

    geometry::EditableMesh over_budget_source;
    constexpr std::uint64_t source_triangle_count = 513U;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.reserve(static_cast<std::size_t>(source_triangle_count) * 3U);
    faces.reserve(static_cast<std::size_t>(source_triangle_count));
    for (std::uint64_t index = 0U; index < source_triangle_count; ++index) {
        const std::uint64_t first_vertex = index * 3U + 1U;
        const std::uint64_t face = index + 1U;
        const double x = static_cast<double>(index) * 2.0;
        vertices.push_back(geometry::Vertex{
            geometry::VertexId{first_vertex}, core::Vec3d{x, 0.0, 0.0}});
        vertices.push_back(geometry::Vertex{
            geometry::VertexId{first_vertex + 1U}, core::Vec3d{x + 0.5, 0.0, 0.0}});
        vertices.push_back(geometry::Vertex{
            geometry::VertexId{first_vertex + 2U}, core::Vec3d{x, 0.5, 0.0}});
        faces.push_back(geometry::Face{geometry::FaceId{face}, std::vector<geometry::VertexId>{
            geometry::VertexId{first_vertex}, geometry::VertexId{first_vertex + 1U},
            geometry::VertexId{first_vertex + 2U}}});
    }
    REQUIRE(over_budget_source.insert_bulk(std::move(vertices), std::move(faces)));
    const auto over_budget_binding = production::bind_bake_source(
        "project/asset/ao-over-budget", over_budget_source);
    REQUIRE(over_budget_binding);
    auto over_budget_recipe = recipe;
    over_budget_recipe.binding.source = over_budget_binding.value();
    over_budget_recipe.binding.width = 20U;
    over_budget_recipe.binding.height = 20U;
    over_budget_recipe.sample_count = production::kMaxBakeAmbientOcclusionSamples;
    REQUIRE(over_budget_recipe.validate());
    const auto over_budget = d3d12::compile_ambient_occlusion_d3d12(
        device, over_budget_recipe, over_budget_source, target);
    REQUIRE(!over_budget.succeeded());
    REQUIRE(!over_budget.projection.has_value());
    REQUIRE(over_budget.bake.rays_cast * source_triangle_count >
        d3d12::kMaxD3D12ProjectionPairs);
    REQUIRE(std::any_of(over_budget.bake.diagnostics.begin(), over_budget.bake.diagnostics.end(),
        [](const production::BakeDiagnostic& diagnostic) {
            return diagnostic.code == "d3d12_ao_projection_failed" && diagnostic.is_error &&
                diagnostic.message.find("candidate-pair budget") != std::string::npos;
        }));
}

void d3d12_projection_worker_matches_reference_and_rejects_hostile_inputs() {
    using namespace carto;

    const auto make_digest = [](std::uint8_t seed) {
        const std::array<std::uint8_t, 1U> value{seed};
        return assets::sha256(value);
    };
    const auto make_request = [&](d3d12::ProjectionBackfacePolicy policy) {
        d3d12::D3D12ProjectionRequest request;
        request.source_digest = make_digest(1U);
        request.target_digest = make_digest(2U);
        request.settings_digest = make_digest(3U);
        request.backface_policy = policy;
        request.triangles = {
            {10U, 0U, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}},
            {20U, 0U, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}},
            {30U, 0U, {0.0, 0.0, 1.0}, {0.0, 1.0, 1.0}, {1.0, 0.0, 1.0}},
        };
        request.rays = {
            {100U, 0U, 0U, 0U, {0.25, 0.25, 1.0}, {0.0, 0.0, -1.0}, 1.0},
            {100U, 1U, 0U, 0U, {2.0, 2.0, 1.0}, {0.0, 0.0, -1.0}, 2.0},
            {100U, 2U, 0U, 0U, {0.25, 0.25, -1.0}, {0.0, 0.0, 1.0}, 2.0},
            {100U, 3U, 0U, 0U, {1.0, 0.0, 1.0}, {0.0, 0.0, -1.0}, 2.0},
        };
        return request;
    };
    const auto cpu_reference = [](const d3d12::D3D12ProjectionRequest& request) {
        std::vector<d3d12::ProjectionHit> hits;
        hits.reserve(request.rays.size());
        for (const d3d12::ProjectionRay& ray : request.rays) {
            d3d12::ProjectionHit hit;
            hit.target_face_id = ray.target_face_id;
            hit.texel_x = ray.texel_x;
            hit.texel_y = ray.texel_y;
            hit.sample_index = ray.sample_index;
            double best_distance = ray.maximum_distance;
            std::size_t best_triangle = request.triangles.size();
            double best_u = 0.0;
            double best_v = 0.0;
            bool best_backface = false;
            for (std::size_t index = 0U; index < request.triangles.size(); ++index) {
                const d3d12::ProjectionTriangle& triangle = request.triangles[index];
                const core::Vec3d edge1 = triangle.v1 - triangle.v0;
                const core::Vec3d edge2 = triangle.v2 - triangle.v0;
                const core::Vec3d p = core::cross(ray.direction, edge2);
                const double determinant = core::dot(edge1, p);
                const bool rejected_backface =
                    request.backface_policy == d3d12::ProjectionBackfacePolicy::front_only &&
                    determinant < -1.0e-12;
                if (request.backface_policy == d3d12::ProjectionBackfacePolicy::front_only) {
                    if (determinant <= 1.0e-12 && !rejected_backface) continue;
                } else if (std::abs(determinant) <= 1.0e-12) {
                    continue;
                }
                const double inverse = 1.0 / determinant;
                const core::Vec3d from_v0 = ray.origin - triangle.v0;
                const double u = core::dot(from_v0, p) * inverse;
                if (u < 0.0 || u > 1.0) continue;
                const core::Vec3d q = core::cross(from_v0, edge1);
                const double v = core::dot(ray.direction, q) * inverse;
                if (v < 0.0 || u + v > 1.0) continue;
                const double distance = core::dot(edge2, q) * inverse;
                if (distance <= 0.0 || distance > ray.maximum_distance) continue;
                if (rejected_backface) {
                    if (!hit.has_rejected_backface || distance < hit.rejected_distance) {
                        hit.has_rejected_backface = true;
                        hit.rejected_source_face_id = triangle.source_face_id;
                        hit.rejected_source_fan_index = triangle.fan_index;
                        hit.rejected_distance = static_cast<float>(distance);
                        hit.rejected_barycentric_u = static_cast<float>(u);
                        hit.rejected_barycentric_v = static_cast<float>(v);
                    }
                    continue;
                }
                if (best_triangle != request.triangles.size() && distance >= best_distance) {
                    continue;
                }
                best_triangle = index;
                best_distance = distance;
                best_u = u;
                best_v = v;
                best_backface = determinant < 0.0;
            }
            if (best_triangle != request.triangles.size()) {
                const d3d12::ProjectionTriangle& triangle =
                    request.triangles[best_triangle];
                hit.hit = true;
                hit.source_face_id = triangle.source_face_id;
                hit.source_fan_index = triangle.fan_index;
                hit.backface = best_backface;
                hit.distance = static_cast<float>(best_distance);
                hit.barycentric_u = static_cast<float>(best_u);
                hit.barycentric_v = static_cast<float>(best_v);
            }
            hits.push_back(hit);
        }
        return hits;
    };
    const auto compare_reference = [&](
        const d3d12::D3D12ProjectionRequest& request,
        const d3d12::D3D12ProjectionReport& report) {
        const std::vector<d3d12::ProjectionHit> expected = cpu_reference(request);
        REQUIRE(report.hits.size() == expected.size());
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            const auto& actual_hit = report.hits[index];
            const auto& expected_hit = expected[index];
            REQUIRE(actual_hit.target_face_id == expected_hit.target_face_id);
            REQUIRE(actual_hit.texel_x == expected_hit.texel_x);
            REQUIRE(actual_hit.texel_y == expected_hit.texel_y);
            REQUIRE(actual_hit.hit == expected_hit.hit);
            REQUIRE(actual_hit.source_face_id == expected_hit.source_face_id);
            REQUIRE(actual_hit.source_fan_index == expected_hit.source_fan_index);
            REQUIRE(actual_hit.backface == expected_hit.backface);
            REQUIRE(actual_hit.has_rejected_backface == expected_hit.has_rejected_backface);
            REQUIRE(actual_hit.rejected_source_face_id == expected_hit.rejected_source_face_id);
            REQUIRE(actual_hit.rejected_source_fan_index == expected_hit.rejected_source_fan_index);
            if (actual_hit.hit) {
                REQUIRE(std::abs(actual_hit.distance - expected_hit.distance) < 1.0e-5F);
                REQUIRE(std::abs(actual_hit.barycentric_u - expected_hit.barycentric_u) <
                        1.0e-5F);
                REQUIRE(std::abs(actual_hit.barycentric_v - expected_hit.barycentric_v) <
                        1.0e-5F);
            }
            if (actual_hit.has_rejected_backface) {
                REQUIRE(std::abs(actual_hit.rejected_distance - expected_hit.rejected_distance) <
                        1.0e-5F);
                REQUIRE(std::abs(actual_hit.rejected_barycentric_u -
                        expected_hit.rejected_barycentric_u) < 1.0e-5F);
                REQUIRE(std::abs(actual_hit.rejected_barycentric_v -
                        expected_hit.rejected_barycentric_v) < 1.0e-5F);
            }
        }
    };

    auto both_sides_request = make_request(d3d12::ProjectionBackfacePolicy::both_sides);
    REQUIRE(both_sides_request.validate());
    auto device_result = d3d12::D3D12Device::create();
    if (!device_result) {
        throw TestFailure("D3D12 projection worker device: " + device_result.error().message);
    }
    auto& device = *device_result.value();
    const auto both_sides = d3d12::run_d3d12_projection_worker(
        device, both_sides_request);
    if (!both_sides) {
        throw TestFailure("D3D12 projection worker: " + both_sides.error().message);
    }
    REQUIRE(both_sides.value().candidate_triangle_tests == 12U);
    REQUIRE(both_sides.value().hits[0U].hit);
    REQUIRE(both_sides.value().hits[0U].source_face_id == 10U);
    REQUIRE(!both_sides.value().hits[1U].hit);
    REQUIRE(both_sides.value().hits[2U].hit);
    REQUIRE(both_sides.value().hits[2U].backface);
    REQUIRE(both_sides.value().hits[3U].hit);
    REQUIRE(!both_sides.value().hits[3U].backface);
    REQUIRE(both_sides.value().hits[0U].distance == 1.0F);
    REQUIRE(!both_sides.value().request_digest.is_zero());
    REQUIRE(!both_sides.value().shader_source_digest.is_zero());
    REQUIRE(!both_sides.value().shader_binary_digest.is_zero());
    REQUIRE(!both_sides.value().compiler_invocation_digest.empty());
    REQUIRE(!both_sides.value().adapter_name.empty());
    REQUIRE(!both_sides.value().stable_adapter_id.empty());
    std::cout << "D3D12 projection adapter: " << both_sides.value().adapter_name
              << " [" << both_sides.value().stable_adapter_id << "] driver="
              << both_sides.value().driver << '\n';
    compare_reference(both_sides_request, both_sides.value());

    const auto repeated = d3d12::run_d3d12_projection_worker(device, both_sides_request);
    REQUIRE(repeated);
    REQUIRE(repeated.value().request_digest == both_sides.value().request_digest);
    REQUIRE(repeated.value().output_digest == both_sides.value().output_digest);
    auto changed_source_digest = both_sides_request;
    changed_source_digest.source_digest = make_digest(9U);
    const auto provenance_variant = d3d12::run_d3d12_projection_worker(
        device, changed_source_digest);
    REQUIRE(provenance_variant);
    REQUIRE(provenance_variant.value().request_digest != both_sides.value().request_digest);
    REQUIRE(provenance_variant.value().output_digest == both_sides.value().output_digest);

    auto front_only_request = make_request(d3d12::ProjectionBackfacePolicy::front_only);
    const auto front_only = d3d12::run_d3d12_projection_worker(device, front_only_request);
    if (!front_only) {
        throw TestFailure("D3D12 front-only projection worker: " +
                          front_only.error().message);
    }
    REQUIRE(front_only.value().hits[0U].hit);
    REQUIRE(front_only.value().hits[2U].hit);
    REQUIRE(front_only.value().hits[2U].source_face_id == 30U);
    REQUIRE(!front_only.value().hits[2U].backface);
    REQUIRE(front_only.value().hits[2U].has_rejected_backface);
    REQUIRE(front_only.value().hits[2U].rejected_source_face_id == 10U);
    compare_reference(front_only_request, front_only.value());

    auto missing_digest = both_sides_request;
    missing_digest.settings_digest = {};
    REQUIRE(!missing_digest.validate());
    auto unsorted_triangles = both_sides_request;
    std::swap(unsorted_triangles.triangles[0U], unsorted_triangles.triangles[1U]);
    REQUIRE(!unsorted_triangles.validate());
    auto duplicate_ray_identity = both_sides_request;
    duplicate_ray_identity.rays[1U].texel_x = duplicate_ray_identity.rays[0U].texel_x;
    REQUIRE(!duplicate_ray_identity.validate());
    auto non_normalized_ray = both_sides_request;
    non_normalized_ray.rays[0U].direction = {0.0, 0.0, -2.0};
    REQUIRE(!non_normalized_ray.validate());
    auto non_finite_triangle = both_sides_request;
    non_finite_triangle.triangles[0U].v0.x =
        std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!non_finite_triangle.validate());
    auto over_budget = both_sides_request;
    over_budget.maximum_candidate_pairs = 7U;
    REQUIRE(!over_budget.validate());
    auto invalid_policy = both_sides_request;
    invalid_policy.backface_policy =
        static_cast<d3d12::ProjectionBackfacePolicy>(255U);
    REQUIRE(!invalid_policy.validate());
    auto collapsed_float_triangle = both_sides_request;
    collapsed_float_triangle.triangles[0U] = {
        10U, 0U,
        {1.0e9, 0.0, 0.0},
        {1.0e9 + 1.0, 0.0, 0.0},
        {1.0e9, 1.0, 0.0},
    };
    REQUIRE(!collapsed_float_triangle.validate());
    auto underflowed_float_distance = both_sides_request;
    underflowed_float_distance.rays[0U].maximum_distance = 1.0e-100;
    REQUIRE(!underflowed_float_distance.validate());
    auto no_source_triangles = both_sides_request;
    no_source_triangles.triangles.clear();
    REQUIRE(!no_source_triangles.validate());

    REQUIRE(!device.device_lost());
    REQUIRE(device.debug_receipts().empty());
}

} // namespace

int main() {
    try {
        d3d12_device_reports_native_identity_and_owns_resources();
        std::cout << "PASS D3D12 native identity and resource ownership\n";
        d3d12_lifetimes_recycle_descriptors_and_defer_resource_destruction();
        std::cout << "PASS D3D12 descriptor reuse and deferred destruction\n";
        d3d12_device_loss_is_fail_closed_and_recoverable();
        std::cout << "PASS D3D12 device-loss recovery boundary\n";
        d3d12_captures_native_timestamp_queries();
        std::cout << "PASS D3D12 native timestamp queries\n";
        d3d12_uses_compute_queue_shader_states();
        std::cout << "PASS D3D12 compute queue shader states\n";
        d3d12_reuses_multiple_in_flight_command_contexts();
        std::cout << "PASS D3D12 in-flight command contexts\n";
        d3d12_compiles_dxil_draws_indexed_and_reads_texture();
        std::cout << "PASS D3D12 DXIL indexed draw and texture readback\n";
        d3d12_uploads_and_reads_specific_texture_mips();
        std::cout << "PASS D3D12 mip-specific texture upload and readback\n";
        d3d12_projection_worker_matches_reference_and_rejects_hostile_inputs();
        std::cout << "PASS D3D12 deterministic ray projection worker and hostile inputs\n";
        d3d12_executes_builtin_shader_suite_and_dispatches_compute();
        std::cout << "PASS D3D12 built-in PBR/tone-map/shadow/temporal and compute dispatch\n";
        d3d12_temporal_fp8_matches_e4m3_reference_and_motion_path();
        std::cout << "PASS D3D12 FP8 temporal numerical and motion-path readback\n";
        d3d12_ibl_irradiance_matches_cpu_reference();
        std::cout << "PASS D3D12 bounded IBL irradiance CPU parity\n";
        d3d12_accepts_8k_temporal_resolve();
        std::cout << "PASS D3D12 8K temporal resolve acceptance\n";
        d3d12_exercises_hidden_swapchain_resize_and_present();
        std::cout << "PASS D3D12 hidden swapchain resize and present\n";
        d3d12_bake_graph_builds_artifact_from_gpu_projection();
        std::cout << "PASS D3D12 BakeGraph artifact parity and stale-source rejection\n";
        d3d12_ambient_occlusion_uses_gpu_rays_and_fails_closed_on_stale_inputs();
        std::cout << "PASS D3D12 ambient-occlusion GPU parity and stale-source rejection\n";
        std::cout << "PASS D3D12 SculptStack preview parity and stale-source rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL D3D12 native identity and resource ownership: "
                  << error.what() << '\n';
        return 1;
    }
}
