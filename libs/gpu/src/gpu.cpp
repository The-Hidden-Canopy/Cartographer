#include <carto/gpu/rhi.hpp>

#include <set>
#include <string>
#include <tuple>

namespace carto::gpu {

namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

} // namespace

bool is_depth_format(Format format) noexcept {
    return format == Format::depth32_float || format == Format::depth24_stencil8;
}

bool is_srgb_format(Format format) noexcept {
    return format == Format::rgba8_srgb || format == Format::bgra8_srgb;
}

bool is_float_format(Format format) noexcept {
    return format == Format::rgba16_float || format == Format::rgba32_float ||
           format == Format::rg16_float || format == Format::rg32_float ||
           format == Format::r16_float || format == Format::r32_float ||
           format == Format::depth32_float;
}

core::Result<void> validate(const BufferDesc& descriptor) {
    if (descriptor.bytes == 0U) {
        return core::Result<void>::failure(invalid("GPU buffer size must be non-zero"));
    }
    if (descriptor.usage == BufferUsage::none) {
        return core::Result<void>::failure(invalid("GPU buffer usage must not be empty"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const TextureDesc& descriptor) {
    if (descriptor.width == 0U || descriptor.height == 0U || descriptor.depth == 0U ||
        descriptor.layers == 0U || descriptor.mip_levels == 0U) {
        return core::Result<void>::failure(invalid("GPU texture extent and mip counts must be non-zero"));
    }
    if (descriptor.format == Format::unknown) {
        return core::Result<void>::failure(invalid("GPU texture format must be explicit"));
    }
    if (descriptor.usage == TextureUsage::none) {
        return core::Result<void>::failure(invalid("GPU texture usage must not be empty"));
    }
    switch (descriptor.dimension) {
    case TextureDimension::texture_2d:
        if (descriptor.depth != 1U || descriptor.layers != 1U) {
            return core::Result<void>::failure(
                invalid("2D texture descriptors require depth and layers equal to one"));
        }
        break;
    case TextureDimension::texture_2d_array:
        if (descriptor.depth != 1U) {
            return core::Result<void>::failure(
                invalid("2D array texture descriptors require depth equal to one"));
        }
        break;
    case TextureDimension::cube:
        if (descriptor.depth != 1U || descriptor.width != descriptor.height ||
            descriptor.layers != 6U) {
            return core::Result<void>::failure(
                invalid("cube texture descriptors require square six-layer images"));
        }
        break;
    }
    if (has_usage(descriptor.usage, TextureUsage::color_attachment) &&
        is_depth_format(descriptor.format)) {
        return core::Result<void>::failure(
            validation("depth formats cannot be used as color attachments"));
    }
    if (has_usage(descriptor.usage, TextureUsage::depth_attachment) &&
        !is_depth_format(descriptor.format)) {
        return core::Result<void>::failure(
            validation("depth attachments require an explicit depth format"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const SamplerDesc& descriptor) {
    if (!std::isfinite(descriptor.max_anisotropy) || descriptor.max_anisotropy < 1.0F) {
        return core::Result<void>::failure(
            invalid("sampler anisotropy must be finite and at least one"));
    }
    if (!std::isfinite(descriptor.min_lod) || !std::isfinite(descriptor.max_lod) ||
        descriptor.min_lod < 0.0F || descriptor.max_lod < descriptor.min_lod) {
        return core::Result<void>::failure(invalid("sampler LOD bounds are invalid"));
    }
    if (!descriptor.anisotropy && descriptor.max_anisotropy != 1.0F) {
        return core::Result<void>::failure(
            validation("sampler max anisotropy is non-one while anisotropy is disabled"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const ShaderDesc& descriptor) {
    if (descriptor.debug_name.empty() || descriptor.entry_point.empty()) {
        return core::Result<void>::failure(invalid("shader name and entry point are required"));
    }
    const auto stage_bits = static_cast<std::uint32_t>(descriptor.stage);
    if (stage_bits != static_cast<std::uint32_t>(ShaderStage::vertex) &&
        stage_bits != static_cast<std::uint32_t>(ShaderStage::fragment) &&
        stage_bits != static_cast<std::uint32_t>(ShaderStage::compute)) {
        return core::Result<void>::failure(invalid("shader stage must identify one stage"));
    }
    if (descriptor.bytecode.empty()) {
        return core::Result<void>::failure(invalid("shader bytecode must not be empty"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const PipelineDesc& descriptor) {
    if (descriptor.debug_name.empty() || descriptor.shaders.empty()) {
        return core::Result<void>::failure(invalid("pipeline name and shader stages are required"));
    }
    for (const ShaderHandle shader : descriptor.shaders) {
        if (!shader) {
            return core::Result<void>::failure(invalid("pipeline contains an invalid shader handle"));
        }
    }
    if (descriptor.sample_count == 0U || descriptor.color_format == Format::unknown) {
        return core::Result<void>::failure(invalid("pipeline sample count and color format are required"));
    }
    if (descriptor.depth_test && !is_depth_format(descriptor.depth_format)) {
        return core::Result<void>::failure(
            validation("depth-tested pipelines require an explicit depth format"));
    }
    std::set<std::pair<std::uint32_t, std::uint32_t>> bindings;
    for (const auto& binding : descriptor.descriptor_bindings) {
        if (binding.stages == ShaderStage::none ||
            !bindings.emplace(binding.set, binding.binding).second) {
            return core::Result<void>::failure(
                validation("pipeline descriptor bindings must be unique and staged"));
        }
    }
    if (descriptor.push_constant_bytes > 256U) {
        return core::Result<void>::failure(
            invalid("pipeline push constants exceed the public safety bound"));
    }
    if (descriptor.vertex_attributes.size() > 16U) {
        return core::Result<void>::failure(
            invalid("pipeline vertex input layout exceeds the public attribute bound"));
    }
    std::set<std::tuple<std::uint32_t, std::string, std::uint32_t>> attributes;
    for (const auto& attribute : descriptor.vertex_attributes) {
        if (attribute.semantic.empty() || attribute.semantic.size() > 64U ||
            attribute.input_slot > 31U ||
            !attributes.emplace(
                attribute.input_slot, attribute.semantic, attribute.semantic_index).second ||
            vertex_format_bytes(attribute.format) == 0U) {
            return core::Result<void>::failure(
                validation("pipeline vertex attributes must be named, unique, and supported"));
        }
        const std::uint64_t end = static_cast<std::uint64_t>(attribute.offset_bytes) +
            vertex_format_bytes(attribute.format);
        if (descriptor.vertex_stride_bytes == 0U ||
            end > descriptor.vertex_stride_bytes) {
            return core::Result<void>::failure(
                validation("pipeline vertex attributes must fit the declared vertex stride"));
        }
    }
    if (descriptor.vertex_stride_bytes != 0U && descriptor.vertex_attributes.empty()) {
        return core::Result<void>::failure(
            validation("a declared vertex stride requires explicit vertex attributes"));
    }
    return core::Result<void>::success();
}

core::Result<void> validate(const DeviceCapabilities& capabilities) {
    if (!capabilities.graphics_queue || capabilities.max_texture_dimension_2d == 0U ||
        capabilities.max_color_attachments == 0U ||
        !std::isfinite(capabilities.max_sampler_anisotropy) ||
        capabilities.max_sampler_anisotropy < 1.0F) {
        return core::Result<void>::failure(
            validation("device capabilities do not describe a usable graphics adapter"));
    }
    if (capabilities.fp16_color_attachment && !capabilities.fp16_storage) {
        // This is allowed by Vulkan, but the distinction must remain explicit rather than
        // being silently collapsed into one HDR capability.
        return core::Result<void>::success();
    }
    return core::Result<void>::success();
}

static_assert(sizeof(BufferHandle) == sizeof(std::uint64_t));
static_assert(sizeof(TextureHandle) == sizeof(std::uint64_t));

} // namespace carto::gpu
