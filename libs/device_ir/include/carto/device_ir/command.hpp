#pragma once

#include <carto/core/result.hpp>
#include <carto/gpu/rhi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace carto::device_ir {

enum class ResourceState {
    undefined,
    copy_source,
    copy_destination,
    vertex_read,
    index_read,
    constant_read,
    shader_read,
    shader_write,
    render_target,
    depth_write,
    depth_read,
    present,
};

enum class IndexFormat {
    uint16,
    uint32,
};

using ResourceHandle = std::variant<gpu::BufferHandle, gpu::TextureHandle>;

struct CmdBeginLabel {
    std::string name;
};

struct CmdEndLabel {};

struct CmdTransitionResource {
    ResourceHandle resource;
    ResourceState before = ResourceState::undefined;
    ResourceState after = ResourceState::undefined;
};

struct CmdUavBarrier {
    ResourceHandle resource;
};

struct CmdCopyBuffer {
    gpu::BufferHandle source;
    gpu::BufferHandle destination;
    std::uint64_t source_offset = 0U;
    std::uint64_t destination_offset = 0U;
    std::uint64_t bytes = 0U;
};

struct CmdCopyBufferToTexture {
    gpu::BufferHandle source;
    gpu::TextureHandle destination;
    std::uint64_t source_offset = 0U;
    std::uint32_t mip_level = 0U;
};

struct CmdCopyTextureToBuffer {
    gpu::TextureHandle source;
    gpu::BufferHandle destination;
    std::uint64_t destination_offset = 0U;
    std::uint32_t mip_level = 0U;
};

struct CmdCopyTexture {
    gpu::TextureHandle source;
    gpu::TextureHandle destination;
};

struct CmdBlitTexture {
    gpu::TextureHandle source;
    gpu::TextureHandle destination;
};

struct CmdClearColor {
    gpu::TextureHandle target;
    std::array<float, 4U> color{0.0F, 0.0F, 0.0F, 0.0F};
};

struct CmdClearDepth {
    gpu::TextureHandle target;
    float depth = 1.0F;
};

struct CmdBeginRendering {
    gpu::TextureHandle color_target;
    std::optional<gpu::TextureHandle> depth_target;
};

struct CmdEndRendering {};

struct CmdSetViewport {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float min_depth = 0.0F;
    float max_depth = 1.0F;
};

struct CmdSetScissor {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct CmdBindPipeline {
    gpu::PipelineHandle pipeline;
};

struct CmdBindVertexBuffer {
    gpu::BufferHandle buffer;
    std::uint64_t offset = 0U;
    std::uint32_t stride_bytes = 0U;
};

struct CmdBindIndexBuffer {
    gpu::BufferHandle buffer;
    std::uint64_t offset = 0U;
    IndexFormat format = IndexFormat::uint32;
};

struct ResourceBinding {
    std::uint32_t set = 0U;
    std::uint32_t binding = 0U;
    ResourceHandle resource;
    std::optional<gpu::SamplerHandle> sampler;
};

struct CmdBindResources {
    std::vector<ResourceBinding> bindings;
};

struct CmdPushConstants {
    std::vector<std::uint8_t> bytes;
};

struct CmdDraw {
    std::uint32_t vertex_count = 0U;
    std::uint32_t instance_count = 1U;
    std::uint32_t first_vertex = 0U;
    std::uint32_t first_instance = 0U;
};

struct CmdDrawIndexed {
    std::uint32_t index_count = 0U;
    std::uint32_t instance_count = 1U;
    std::uint32_t first_index = 0U;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0U;
};

struct CmdDispatch {
    std::uint32_t group_count_x = 0U;
    std::uint32_t group_count_y = 0U;
    std::uint32_t group_count_z = 0U;
};

struct CmdWriteTimestamp {
    std::uint32_t query = 0U;
};

struct CmdResolveTimestamps {
    gpu::BufferHandle destination;
    std::uint32_t first_query = 0U;
    std::uint32_t query_count = 0U;
    std::uint64_t destination_offset = 0U;
};

struct CmdWaitTimeline {
    std::uint64_t serial = 0U;
};

struct CmdSignalTimeline {
    std::uint64_t serial = 0U;
};

struct CmdPresent {
    gpu::TextureHandle source;
};

using Command = std::variant<
    CmdBeginLabel,
    CmdEndLabel,
    CmdTransitionResource,
    CmdUavBarrier,
    CmdCopyBuffer,
    CmdCopyBufferToTexture,
    CmdCopyTextureToBuffer,
    CmdCopyTexture,
    CmdBlitTexture,
    CmdClearColor,
    CmdClearDepth,
    CmdBeginRendering,
    CmdEndRendering,
    CmdSetViewport,
    CmdSetScissor,
    CmdBindPipeline,
    CmdBindVertexBuffer,
    CmdBindIndexBuffer,
    CmdBindResources,
    CmdPushConstants,
    CmdDraw,
    CmdDrawIndexed,
    CmdDispatch,
    CmdWriteTimestamp,
    CmdResolveTimestamps,
    CmdWaitTimeline,
    CmdSignalTimeline,
    CmdPresent>;

class DeviceCommandStream {
public:
    template <typename T>
    void append(T command) {
        commands_.emplace_back(std::move(command));
    }

    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string dump() const;

private:
    std::vector<Command> commands_;
};

} // namespace carto::device_ir
