#pragma once

#include <carto/gpu/handles.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
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

enum class TextureDimension {
    texture_2d,
    texture_2d_array,
    cube,
};

enum class Format {
    unknown,
    rgba8_unorm,
    rgba8_srgb,
    bgra8_unorm,
    bgra8_srgb,
    rgba16_float,
    rgba32_float,
    rg16_float,
    rg32_float,
    r16_float,
    r32_float,
    r32_uint,
    depth32_float,
    depth24_stencil8,
};

enum class TextureUsage : std::uint32_t {
    none = 0U,
    sampled = 1U << 0U,
    storage = 1U << 1U,
    color_attachment = 1U << 2U,
    depth_attachment = 1U << 3U,
    transfer_source = 1U << 4U,
    transfer_destination = 1U << 5U,
};

[[nodiscard]] constexpr TextureUsage operator|(
    TextureUsage left,
    TextureUsage right) noexcept {
    return static_cast<TextureUsage>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr TextureUsage operator&(
    TextureUsage left,
    TextureUsage right) noexcept {
    return static_cast<TextureUsage>(
        static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool has_usage(
    TextureUsage value,
    TextureUsage flag) noexcept {
    return (value & flag) == flag;
}

enum class BufferUsage : std::uint32_t {
    none = 0U,
    vertex = 1U << 0U,
    index = 1U << 1U,
    uniform = 1U << 2U,
    storage = 1U << 3U,
    indirect = 1U << 4U,
    transfer_source = 1U << 5U,
    transfer_destination = 1U << 6U,
};

[[nodiscard]] constexpr BufferUsage operator|(
    BufferUsage left,
    BufferUsage right) noexcept {
    return static_cast<BufferUsage>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr BufferUsage operator&(
    BufferUsage left,
    BufferUsage right) noexcept {
    return static_cast<BufferUsage>(
        static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool has_usage(
    BufferUsage value,
    BufferUsage flag) noexcept {
    return (value & flag) == flag;
}

enum class Filter { nearest, linear };
enum class MipFilter { nearest, linear };
enum class AddressMode { repeat, mirror, clamp_edge, clamp_border };
enum class CompareOp {
    never,
    less,
    equal,
    less_or_equal,
    greater,
    not_equal,
    greater_or_equal,
    always,
};

enum class ShaderStage : std::uint32_t {
    none = 0U,
    vertex = 1U << 0U,
    fragment = 1U << 1U,
    compute = 1U << 2U,
};

[[nodiscard]] constexpr ShaderStage operator|(
    ShaderStage left,
    ShaderStage right) noexcept {
    return static_cast<ShaderStage>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool has_stage(
    ShaderStage value,
    ShaderStage stage) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(stage)) != 0U;
}

enum class PrimitiveTopology { triangle_list, triangle_strip, line_list };
enum class PolygonMode { fill, line, point };
enum class CullMode { none, front, back, front_and_back };
enum class FrontFace { counter_clockwise, clockwise };
enum class BlendFactor { zero, one, source_alpha, one_minus_source_alpha };
enum class BlendOp { add, subtract, reverse_subtract, minimum, maximum };

struct BufferDesc {
    std::uint64_t bytes = 0;
    MemoryClass memory = MemoryClass::device_local;
    BufferUsage usage = BufferUsage::none;
};

struct TextureDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t layers = 1;
    std::uint32_t mip_levels = 1;
    Format format = Format::unknown;
    TextureDimension dimension = TextureDimension::texture_2d;
    TextureUsage usage = TextureUsage::none;
};

struct SamplerDesc {
    Filter min_filter = Filter::linear;
    Filter mag_filter = Filter::linear;
    MipFilter mip_filter = MipFilter::linear;
    AddressMode u = AddressMode::clamp_edge;
    AddressMode v = AddressMode::clamp_edge;
    AddressMode w = AddressMode::clamp_edge;
    bool anisotropy = false;
    float max_anisotropy = 1.0F;
    bool compare = false;
    CompareOp compare_op = CompareOp::less_or_equal;
    float min_lod = 0.0F;
    float max_lod = std::numeric_limits<float>::max();
};

struct ShaderDesc {
    std::string debug_name;
    ShaderStage stage = ShaderStage::none;
    std::string entry_point = "main";
    std::vector<std::uint8_t> bytecode;
};

struct DescriptorBinding {
    std::uint32_t set = 0U;
    std::uint32_t binding = 0U;
    ShaderStage stages = ShaderStage::none;

    [[nodiscard]] constexpr auto operator<=>(const DescriptorBinding&) const noexcept = default;
};

struct PipelineDesc {
    std::string debug_name;
    std::vector<ShaderHandle> shaders;
    std::vector<DescriptorBinding> descriptor_bindings;
    PrimitiveTopology topology = PrimitiveTopology::triangle_list;
    PolygonMode polygon_mode = PolygonMode::fill;
    CullMode cull_mode = CullMode::back;
    FrontFace front_face = FrontFace::counter_clockwise;
    bool depth_test = true;
    bool depth_write = true;
    CompareOp depth_compare = CompareOp::less_or_equal;
    bool blend = false;
    BlendFactor source_color = BlendFactor::one;
    BlendFactor destination_color = BlendFactor::zero;
    BlendOp color_blend = BlendOp::add;
    std::uint32_t sample_count = 1U;
    Format color_format = Format::rgba16_float;
    Format depth_format = Format::depth32_float;
    std::uint32_t push_constant_bytes = 0U;
};

struct RenderTargetDesc {
    TextureDesc color;
    bool has_depth = true;
};

struct DeviceCapabilities {
    bool graphics_queue = false;
    bool compute_queue = false;
    bool copy_queue = false;
    bool presentation = false;
    bool descriptor_indexing = false;
    bool anisotropy = false;
    bool sampler_compare = false;
    bool timestamp_queries = false;
    bool fp16_color_attachment = false;
    bool fp16_storage = false;
    bool ray_tracing_pipeline = false;
    bool acceleration_structure = false;
    std::uint32_t max_texture_dimension_2d = 0U;
    float max_sampler_anisotropy = 1.0F;
    std::uint32_t max_push_constant_bytes = 0U;
    std::uint32_t max_color_attachments = 0U;
};

[[nodiscard]] bool is_depth_format(Format format) noexcept;
[[nodiscard]] bool is_srgb_format(Format format) noexcept;
[[nodiscard]] bool is_float_format(Format format) noexcept;
[[nodiscard]] core::Result<void> validate(const BufferDesc& descriptor);
[[nodiscard]] core::Result<void> validate(const TextureDesc& descriptor);
[[nodiscard]] core::Result<void> validate(const SamplerDesc& descriptor);
[[nodiscard]] core::Result<void> validate(const ShaderDesc& descriptor);
[[nodiscard]] core::Result<void> validate(const PipelineDesc& descriptor);
[[nodiscard]] core::Result<void> validate(const DeviceCapabilities& capabilities);

using BufferRegistry = Registry<BufferTag, BufferDesc>;
using TextureRegistry = Registry<TextureTag, TextureDesc>;
using SamplerRegistry = Registry<SamplerTag, SamplerDesc>;
using ShaderRegistry = Registry<ShaderTag, ShaderDesc>;
using PipelineRegistry = Registry<PipelineTag, PipelineDesc>;

} // namespace carto::gpu
