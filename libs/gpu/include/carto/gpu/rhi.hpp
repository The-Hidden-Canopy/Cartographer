#pragma once

#include <carto/gpu/handles.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace carto::gpu {

class Device;
class Buffer;
class Texture;
class Sampler;
class Shader;
class Pipeline;
class CommandList;
class Queue;
class Fence;
class Surface;
class Swapchain;

enum class QueueType {
    graphics,
    compute,
    copy,
};

enum class MemoryClass {
    device_local,
    upload,
    readback,
};

enum class Format {
    unknown,
    rgba8_unorm,
    bgra8_unorm,
    rgba16_float,
    r32_uint,
    depth24_stencil8,
};

struct BufferDesc {
    std::uint64_t bytes = 0;
    MemoryClass memory = MemoryClass::device_local;
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t layers = 1;
    std::uint32_t mip_levels = 1;
    Format format = Format::unknown;
};

struct SamplerDesc {
    bool linear = true;
    bool clamp_to_edge = true;
};

struct ShaderDesc {
    std::string debug_name;
    std::vector<std::uint8_t> bytecode;
};

struct PipelineDesc {
    std::string debug_name;
};

struct RenderTargetDesc {
    TextureDesc color;
    bool has_depth = true;
};

using BufferRegistry = Registry<BufferTag, BufferDesc>;
using TextureRegistry = Registry<TextureTag, TextureDesc>;
using SamplerRegistry = Registry<SamplerTag, SamplerDesc>;
using ShaderRegistry = Registry<ShaderTag, ShaderDesc>;
using PipelineRegistry = Registry<PipelineTag, PipelineDesc>;

} // namespace carto::gpu
