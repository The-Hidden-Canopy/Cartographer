#include <carto/device_cpu/cpu_device.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace carto::device_cpu {

namespace {

using core::Diagnostic;
using core::ErrorCode;

Diagnostic invalid(std::string message) {
    return Diagnostic(ErrorCode::invalid_argument, std::move(message));
}

Diagnostic validation(std::string message) {
    return Diagnostic(ErrorCode::validation_failed, std::move(message));
}

Diagnostic unsupported(std::string message) {
    return Diagnostic(ErrorCode::unsupported, std::move(message));
}

Diagnostic out_of_memory(std::string message) {
    return Diagnostic(ErrorCode::invalid_state, std::move(message));
}

bool add_overflows(std::uint64_t left, std::uint64_t right) {
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

bool multiply_overflows(std::uint64_t left, std::uint64_t right) {
    return left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left;
}

bool finite(const std::array<float, 4U>& color) {
    for (const float value : color) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

bool is_color_format(gpu::Format format) {
    return !gpu::is_depth_format(format) && format != gpu::Format::unknown;
}

std::uint64_t color_channels_bytes(gpu::Format format) {
    switch (format) {
    case gpu::Format::rgba8_unorm:
    case gpu::Format::rgba8_srgb:
    case gpu::Format::bgra8_unorm:
    case gpu::Format::bgra8_srgb:
        return 4U;
    case gpu::Format::rgba16_float:
        return 8U;
    case gpu::Format::rgba32_float:
        return 16U;
    case gpu::Format::rg16_float:
        return 4U;
    case gpu::Format::rg32_float:
        return 8U;
    case gpu::Format::r16_float:
        return 2U;
    case gpu::Format::r32_float:
    case gpu::Format::r32_uint:
    case gpu::Format::depth32_float:
        return 4U;
    case gpu::Format::depth24_stencil8:
    case gpu::Format::unknown:
        return 0U;
    }
    return 0U;
}

bool compare_depth(gpu::CompareOp operation, float left, float right) {
    switch (operation) {
    case gpu::CompareOp::never: return false;
    case gpu::CompareOp::less: return left < right;
    case gpu::CompareOp::equal: return left == right;
    case gpu::CompareOp::less_or_equal: return left <= right;
    case gpu::CompareOp::greater: return left > right;
    case gpu::CompareOp::not_equal: return left != right;
    case gpu::CompareOp::greater_or_equal: return left >= right;
    case gpu::CompareOp::always: return true;
    }
    return false;
}

float blend_factor(gpu::BlendFactor factor, float source_alpha) {
    switch (factor) {
    case gpu::BlendFactor::zero: return 0.0F;
    case gpu::BlendFactor::one: return 1.0F;
    case gpu::BlendFactor::source_alpha: return source_alpha;
    case gpu::BlendFactor::one_minus_source_alpha: return 1.0F - source_alpha;
    }
    return 1.0F;
}

std::optional<float> address_coordinate(float value, gpu::AddressMode mode) {
    if (!std::isfinite(value)) return std::nullopt;
    switch (mode) {
    case gpu::AddressMode::clamp_edge:
        return std::clamp(value, 0.0F, 1.0F);
    case gpu::AddressMode::clamp_border:
        if (value < 0.0F || value > 1.0F) return std::nullopt;
        return value;
    case gpu::AddressMode::repeat:
        return value - std::floor(value);
    case gpu::AddressMode::mirror: {
        const float cell = std::floor(value);
        const float wrapped = value - cell;
        return std::fmod(std::abs(cell), 2.0F) >= 1.0F
            ? 1.0F - wrapped : wrapped;
    }
    }
    return std::nullopt;
}

} // namespace

CpuDevice::CpuDevice(CpuDeviceOptions options) : options_(options) {
    identity_.backend = device::BackendKind::cpu;
    identity_.adapter_name = "Cartographer CPU Reference Device";
    identity_.stable_adapter_id = "carto.cpu.reference.v1";
    identity_.feature_level = "cartographer-device-ir-v1";
    capabilities_.graphics_queue = true;
    capabilities_.compute_queue = true;
    capabilities_.copy_queue = true;
    capabilities_.timestamp_queries = true;
    capabilities_.max_texture_dimension_2d = options_.max_texture_dimension_2d;
    capabilities_.max_sampler_anisotropy = 16.0F;
    capabilities_.max_color_attachments = 8U;
}

device::BackendKind CpuDevice::backend() const noexcept {
    return device::BackendKind::cpu;
}

const device::DeviceIdentity& CpuDevice::identity() const noexcept {
    return identity_;
}

const gpu::DeviceCapabilities& CpuDevice::capabilities() const noexcept {
    return capabilities_;
}

core::Result<gpu::BufferHandle> CpuDevice::create_buffer(const gpu::BufferDesc& descriptor) {
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::BufferHandle>::failure(result.error());
    }
    if (descriptor.bytes > options_.max_allocation_bytes) {
        return core::Result<gpu::BufferHandle>::failure(out_of_memory(
            "CPU reference buffer exceeds its declared allocation budget"));
    }
    BufferResource resource{descriptor, std::vector<std::uint8_t>(
        static_cast<std::size_t>(descriptor.bytes), 0U)};
    auto handle = buffers_.insert(std::move(resource));
    if (handle) resource_states_.emplace(
        device_ir::ResourceHandle{handle.value()}, device_ir::ResourceState::undefined);
    return handle;
}

core::Result<gpu::TextureHandle> CpuDevice::create_texture(const gpu::TextureDesc& descriptor) {
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::TextureHandle>::failure(result.error());
    }
    if (descriptor.dimension != gpu::TextureDimension::texture_2d ||
        descriptor.depth != 1U || descriptor.layers != 1U || descriptor.mip_levels != 1U) {
        return core::Result<gpu::TextureHandle>::failure(unsupported(
            "CPU reference device v1 supports only single-level 2D textures"));
    }
    if (descriptor.format != gpu::Format::rgba32_float &&
        descriptor.format != gpu::Format::depth32_float) {
        return core::Result<gpu::TextureHandle>::failure(unsupported(
            "CPU reference device v1 supports only RGBA32F and D32F textures"));
    }
    if (descriptor.width > options_.max_texture_dimension_2d ||
        descriptor.height > options_.max_texture_dimension_2d) {
        return core::Result<gpu::TextureHandle>::failure(out_of_memory(
            "CPU reference texture exceeds its maximum dimension"));
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(descriptor.width) *
        static_cast<std::uint64_t>(descriptor.height);
    const std::uint64_t bytes_per_pixel = color_channels_bytes(descriptor.format);
    if (pixels == 0U || bytes_per_pixel == 0U ||
        multiply_overflows(pixels, bytes_per_pixel) ||
        pixels * bytes_per_pixel > options_.max_allocation_bytes) {
        return core::Result<gpu::TextureHandle>::failure(out_of_memory(
            "CPU reference texture exceeds its declared allocation budget"));
    }

    TextureResource resource;
    resource.descriptor = descriptor;
    if (gpu::is_depth_format(descriptor.format)) {
        resource.depth.assign(static_cast<std::size_t>(pixels), 1.0F);
    } else {
        if (multiply_overflows(pixels, 4U)) {
            return core::Result<gpu::TextureHandle>::failure(out_of_memory(
                "CPU reference color texture element count overflows"));
        }
        resource.color.assign(static_cast<std::size_t>(pixels * 4U), 0.0F);
    }
    auto handle = textures_.insert(std::move(resource));
    if (handle) resource_states_.emplace(
        device_ir::ResourceHandle{handle.value()}, device_ir::ResourceState::undefined);
    return handle;
}

core::Result<gpu::SamplerHandle> CpuDevice::create_sampler(const gpu::SamplerDesc& descriptor) {
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::SamplerHandle>::failure(result.error());
    }
    return samplers_.insert(SamplerResource{descriptor});
}

core::Result<gpu::ShaderHandle> CpuDevice::create_shader(const device::ShaderBinary& binary) {
    if (auto result = device::validate(binary); !result) {
        return core::Result<gpu::ShaderHandle>::failure(result.error());
    }
    if (binary.format != device::BinaryFormat::cpu_ir) {
        return core::Result<gpu::ShaderHandle>::failure(unsupported(
            "CPU reference device requires the CPU IR shader format"));
    }
    gpu::ShaderDesc descriptor;
    descriptor.debug_name = binary.debug_name;
    descriptor.stage = binary.stage;
    descriptor.entry_point = binary.entry_point;
    descriptor.bytecode = binary.bytes;
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::ShaderHandle>::failure(result.error());
    }
    return shaders_.insert(ShaderResource{binary, std::move(descriptor)});
}

core::Result<gpu::PipelineHandle> CpuDevice::create_pipeline(
    const gpu::PipelineDesc& descriptor) {
    if (auto result = gpu::validate(descriptor); !result) {
        return core::Result<gpu::PipelineHandle>::failure(result.error());
    }
    if (descriptor.topology != gpu::PrimitiveTopology::triangle_list ||
        descriptor.polygon_mode != gpu::PolygonMode::fill ||
        descriptor.cull_mode != gpu::CullMode::none || descriptor.sample_count != 1U ||
        descriptor.color_format != gpu::Format::rgba32_float ||
        descriptor.depth_format != gpu::Format::depth32_float ||
        descriptor.push_constant_bytes > 16U ||
        (descriptor.blend && descriptor.color_blend != gpu::BlendOp::add)) {
        return core::Result<gpu::PipelineHandle>::failure(unsupported(
            "CPU reference device v1 supports only unculled single-sample triangle pipelines"));
    }
    bool has_vertex_shader = false;
    bool has_fragment_shader = false;
    for (const gpu::ShaderHandle shader : descriptor.shaders) {
        const auto result = shaders_.resolve(shader);
        if (!result) {
            return core::Result<gpu::PipelineHandle>::failure(result.error());
        }
        const gpu::ShaderStage stage = result.value()->descriptor.stage;
        if (stage == gpu::ShaderStage::vertex) has_vertex_shader = true;
        if (stage == gpu::ShaderStage::fragment) has_fragment_shader = true;
        if (stage == gpu::ShaderStage::compute) {
            return core::Result<gpu::PipelineHandle>::failure(unsupported(
                "CPU reference device v1 does not execute compute pipelines"));
        }
    }
    if (!has_vertex_shader || !has_fragment_shader) {
        return core::Result<gpu::PipelineHandle>::failure(validation(
            "CPU reference graphics pipelines require vertex and fragment shaders"));
    }
    bool samples_rgba32f = false;
    if (!descriptor.descriptor_bindings.empty()) {
        if (descriptor.descriptor_bindings.size() != 1U ||
            descriptor.descriptor_bindings.front().set != 0U ||
            descriptor.descriptor_bindings.front().binding != 0U ||
            !gpu::has_stage(descriptor.descriptor_bindings.front().stages,
                            gpu::ShaderStage::fragment)) {
            return core::Result<gpu::PipelineHandle>::failure(unsupported(
                "CPU reference device v1 supports only one fragment RGBA32F binding at set 0 binding 0"));
        }
        samples_rgba32f = true;
    }
    return pipelines_.insert(PipelineResource{descriptor, samples_rgba32f});
}

core::Result<void> CpuDevice::destroy_buffer(gpu::BufferHandle handle) {
    const auto result = buffers_.remove(handle);
    if (result) resource_states_.erase(device_ir::ResourceHandle{handle});
    return result;
}

core::Result<void> CpuDevice::destroy_texture(gpu::TextureHandle handle) {
    const auto result = textures_.remove(handle);
    if (result) resource_states_.erase(device_ir::ResourceHandle{handle});
    return result;
}

core::Result<void> CpuDevice::destroy_sampler(gpu::SamplerHandle handle) {
    return samplers_.remove(handle);
}

core::Result<void> CpuDevice::destroy_shader(gpu::ShaderHandle handle) {
    return shaders_.remove(handle);
}

core::Result<void> CpuDevice::destroy_pipeline(gpu::PipelineHandle handle) {
    return pipelines_.remove(handle);
}

core::Result<void> CpuDevice::write_buffer(
    gpu::BufferHandle handle,
    std::uint64_t offset,
    std::span<const std::uint8_t> bytes) {
    auto buffer = buffers_.resolve(handle);
    if (!buffer) return core::Result<void>::failure(buffer.error());
    if (add_overflows(offset, static_cast<std::uint64_t>(bytes.size())) ||
        offset + bytes.size() > buffer.value()->bytes.size()) {
        return core::Result<void>::failure(invalid("buffer write exceeds the resource"));
    }
    std::copy(bytes.begin(), bytes.end(), buffer.value()->bytes.begin() +
        static_cast<std::ptrdiff_t>(offset));
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> CpuDevice::read_buffer(
    gpu::BufferHandle handle,
    std::uint64_t offset,
    std::uint64_t bytes) const {
    auto buffer = buffers_.resolve(handle);
    if (!buffer) return core::Result<std::vector<std::uint8_t>>::failure(buffer.error());
    if (add_overflows(offset, bytes) || offset + bytes > buffer.value()->bytes.size()) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            invalid("buffer read exceeds the resource"));
    }
    const auto begin = buffer.value()->bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(bytes);
    return core::Result<std::vector<std::uint8_t>>::success({begin, end});
}

core::Result<std::vector<float>> CpuDevice::read_texture_rgba32f(
    gpu::TextureHandle handle) const {
    auto texture = textures_.resolve(handle);
    if (!texture) return core::Result<std::vector<float>>::failure(texture.error());
    if (!is_color_format(texture.value()->descriptor.format)) {
        return core::Result<std::vector<float>>::failure(
            invalid("texture readback requires a color texture"));
    }
    return core::Result<std::vector<float>>::success(texture.value()->color);
}

core::Result<void> CpuDevice::preflight(
    const device_ir::DeviceCommandStream& stream) const {
    for (const auto& command : stream.commands()) {
        const auto result = std::visit(
            [this](const auto& value) -> core::Result<void> {
                using T = std::decay_t<decltype(value)>;
                auto resolve_resource = [this](const device_ir::ResourceHandle& resource) {
                    return std::visit(
                        [this](const auto handle) -> core::Result<void> {
                            using Handle = std::decay_t<decltype(handle)>;
                            if constexpr (std::is_same_v<Handle, gpu::BufferHandle>) {
                                auto result = buffers_.resolve(handle);
                                if (!result) return core::Result<void>::failure(result.error());
                            } else {
                                auto result = textures_.resolve(handle);
                                if (!result) return core::Result<void>::failure(result.error());
                            }
                            return core::Result<void>::success();
                        },
                        resource);
                };
                if constexpr (std::is_same_v<T, device_ir::CmdTransitionResource> ||
                              std::is_same_v<T, device_ir::CmdUavBarrier>) {
                    return resolve_resource(value.resource);
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyBuffer>) {
                    if (auto result = buffers_.resolve(value.source); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                    if (auto result = buffers_.resolve(value.destination); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyBufferToTexture>) {
                    if (auto result = buffers_.resolve(value.source); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                    if (auto result = textures_.resolve(value.destination); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyTextureToBuffer>) {
                    if (auto result = textures_.resolve(value.source); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                    if (auto result = buffers_.resolve(value.destination); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyTexture> ||
                                     std::is_same_v<T, device_ir::CmdBlitTexture>) {
                    if (auto result = textures_.resolve(value.source); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                    if (auto result = textures_.resolve(value.destination); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdClearColor> ||
                                     std::is_same_v<T, device_ir::CmdClearDepth>) {
                    if (auto result = textures_.resolve(value.target); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdBeginRendering>) {
                    if (auto result = textures_.resolve(value.color_target); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                    if (value.depth_target.has_value()) {
                        if (auto result = textures_.resolve(*value.depth_target); !result) {
                            return core::Result<void>::failure(result.error());
                        }
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindPipeline>) {
                    if (auto result = pipelines_.resolve(value.pipeline); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindVertexBuffer>) {
                    if (auto result = buffers_.resolve(value.buffer); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindIndexBuffer>) {
                    if (auto result = buffers_.resolve(value.buffer); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindResources>) {
                    for (const auto& binding : value.bindings) {
                        if (auto result = resolve_resource(binding.resource); !result) {
                            return result;
                        }
                        if (binding.sampler.has_value()) {
                            if (auto result = samplers_.resolve(*binding.sampler); !result) {
                                return core::Result<void>::failure(result.error());
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdResolveTimestamps>) {
                    if (auto result = buffers_.resolve(value.destination); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                } else if constexpr (std::is_same_v<T, device_ir::CmdPresent>) {
                    if (auto result = textures_.resolve(value.source); !result) {
                        return core::Result<void>::failure(result.error());
                    }
                }
                return core::Result<void>::success();
            },
            command);
        if (!result) return result;
    }
    return core::Result<void>::success();
}

core::Result<gpu::SubmissionSerial> CpuDevice::submit(
    const device_ir::DeviceCommandStream& stream,
    gpu::QueueType queue) {
    if ((queue == gpu::QueueType::graphics && !capabilities_.graphics_queue) ||
        (queue == gpu::QueueType::compute && !capabilities_.compute_queue) ||
        (queue == gpu::QueueType::copy && !capabilities_.copy_queue)) {
        return core::Result<gpu::SubmissionSerial>::failure(unsupported(
            "CPU reference device does not expose the requested queue"));
    }
    if (auto result = stream.validate(); !result) {
        return core::Result<gpu::SubmissionSerial>::failure(result.error());
    }
    if (auto result = preflight(stream); !result) {
        return core::Result<gpu::SubmissionSerial>::failure(result.error());
    }
    if (submitted_serial_ == std::numeric_limits<gpu::SubmissionSerial>::max()) {
        return core::Result<gpu::SubmissionSerial>::failure(core::Diagnostic(
            ErrorCode::invalid_state, "CPU device submission serial exhausted"));
    }

    const auto require_state = [this](
        const device_ir::ResourceHandle& resource,
        device_ir::ResourceState expected) -> core::Result<void> {
        const auto current = resource_states_.find(resource);
        if (current == resource_states_.end() || current->second != expected) {
            return core::Result<void>::failure(validation(
                "CPU command uses a resource in the wrong state"));
        }
        return core::Result<void>::success();
    };

    ExecutionState state;
    for (const auto& command : stream.commands()) {
        const auto result = std::visit(
            [this, &state, &require_state](const auto& value) -> core::Result<void> {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, device_ir::CmdBeginLabel> ||
                              std::is_same_v<T, device_ir::CmdEndLabel> ||
                              std::is_same_v<T, device_ir::CmdUavBarrier>) {
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindResources>) {
                    state.sampled_texture.reset();
                    if (value.bindings.empty()) return core::Result<void>::success();
                    if (value.bindings.size() != 1U ||
                        value.bindings.front().set != 0U ||
                        value.bindings.front().binding != 0U ||
                        !std::holds_alternative<gpu::TextureHandle>(
                            value.bindings.front().resource)) {
                        return core::Result<void>::failure(unsupported(
                            "CPU reference device v1 supports one sampled texture at set 0 binding 0"));
                    }
                    const auto texture = std::get<gpu::TextureHandle>(
                        value.bindings.front().resource);
                    if (auto result = require_state(
                            device_ir::ResourceHandle{texture},
                            device_ir::ResourceState::shader_read); !result) {
                        return result;
                    }
                    auto resolved = textures_.resolve(texture);
                    if (!resolved || resolved.value()->descriptor.format != gpu::Format::rgba32_float) {
                        return core::Result<void>::failure(unsupported(
                            "CPU reference sampling currently requires an RGBA32F texture"));
                    }
                    if (value.bindings.front().sampler.has_value()) {
                        if (auto result = samplers_.resolve(
                                *value.bindings.front().sampler); !result) {
                            return core::Result<void>::failure(result.error());
                        }
                    }
                    state.sampled_texture = value.bindings.front();
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdTransitionResource>) {
                    const auto current = resource_states_.find(value.resource);
                    const auto state_value = current == resource_states_.end()
                        ? device_ir::ResourceState::undefined
                        : current->second;
                    if (state_value != value.before) {
                        return core::Result<void>::failure(validation(
                            "resource transition does not match the device state"));
                    }
                    resource_states_[value.resource] = value.after;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyBuffer>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) {
                        return result;
                    }
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) {
                        return result;
                    }
                    auto source = buffers_.resolve(value.source);
                    auto destination = buffers_.resolve(value.destination);
                    if (add_overflows(value.source_offset, value.bytes) ||
                        add_overflows(value.destination_offset, value.bytes) ||
                        value.source_offset + value.bytes > source.value()->bytes.size() ||
                        value.destination_offset + value.bytes > destination.value()->bytes.size()) {
                        return core::Result<void>::failure(invalid(
                            "buffer copy exceeds a resource"));
                    }
                    std::memmove(
                        destination.value()->bytes.data() + value.destination_offset,
                        source.value()->bytes.data() + value.source_offset,
                        static_cast<std::size_t>(value.bytes));
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyBufferToTexture>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) {
                        return result;
                    }
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) {
                        return result;
                    }
                    auto source = buffers_.resolve(value.source);
                    auto destination = textures_.resolve(value.destination);
                    if (!source || !destination ||
                        destination.value()->descriptor.format != gpu::Format::rgba32_float) {
                        return core::Result<void>::failure(unsupported(
                            "CPU buffer-to-texture copies currently require an RGBA32F destination"));
                    }
                    const std::uint64_t bytes = static_cast<std::uint64_t>(
                        destination.value()->color.size()) * sizeof(float);
                    if (add_overflows(value.source_offset, bytes) ||
                        value.source_offset + bytes > source.value()->bytes.size()) {
                        return core::Result<void>::failure(invalid(
                            "buffer-to-texture copy exceeds its source buffer"));
                    }
                    std::memcpy(destination.value()->color.data(),
                                source.value()->bytes.data() + value.source_offset,
                                static_cast<std::size_t>(bytes));
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyTextureToBuffer>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) {
                        return result;
                    }
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) {
                        return result;
                    }
                    auto source = textures_.resolve(value.source);
                    auto destination = buffers_.resolve(value.destination);
                    if (!source || !destination ||
                        source.value()->descriptor.format != gpu::Format::rgba32_float) {
                        return core::Result<void>::failure(unsupported(
                            "CPU texture-to-buffer copies currently require an RGBA32F source"));
                    }
                    const std::uint64_t bytes = static_cast<std::uint64_t>(
                        source.value()->color.size()) * sizeof(float);
                    if (add_overflows(value.destination_offset, bytes) ||
                        value.destination_offset + bytes > destination.value()->bytes.size()) {
                        return core::Result<void>::failure(invalid(
                            "texture-to-buffer copy exceeds its destination buffer"));
                    }
                    std::memcpy(destination.value()->bytes.data() + value.destination_offset,
                                source.value()->color.data(),
                                static_cast<std::size_t>(bytes));
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdCopyTexture>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::copy_source); !result) {
                        return result;
                    }
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) {
                        return result;
                    }
                    auto source = textures_.resolve(value.source);
                    auto destination = textures_.resolve(value.destination);
                    if (!source || !destination ||
                        source.value()->descriptor.width != destination.value()->descriptor.width ||
                        source.value()->descriptor.height != destination.value()->descriptor.height ||
                        source.value()->descriptor.format != destination.value()->descriptor.format) {
                        return core::Result<void>::failure(validation(
                            "CPU texture copies require matching extents and formats"));
                    }
                    destination.value()->color = source.value()->color;
                    destination.value()->depth = source.value()->depth;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBlitTexture>) {
                    return core::Result<void>::failure(unsupported(
                        "CPU reference device v1 does not execute filtered texture blits"));
                } else if constexpr (std::is_same_v<T, device_ir::CmdClearColor>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.target},
                            device_ir::ResourceState::render_target); !result) {
                        return result;
                    }
                    auto target = textures_.resolve(value.target);
                    if (!is_color_format(target.value()->descriptor.format)) {
                        return core::Result<void>::failure(validation(
                            "color clear requires a color texture"));
                    }
                    for (std::size_t index = 0U; index < target.value()->color.size(); index += 4U) {
                        std::copy(value.color.begin(), value.color.end(),
                                  target.value()->color.begin() +
                                      static_cast<std::ptrdiff_t>(index));
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdClearDepth>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.target},
                            device_ir::ResourceState::depth_write); !result) {
                        return result;
                    }
                    auto target = textures_.resolve(value.target);
                    if (!gpu::is_depth_format(target.value()->descriptor.format)) {
                        return core::Result<void>::failure(validation(
                            "depth clear requires a depth texture"));
                    }
                    std::fill(target.value()->depth.begin(), target.value()->depth.end(), value.depth);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBeginRendering>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.color_target},
                            device_ir::ResourceState::render_target); !result) {
                        return result;
                    }
                    if (value.depth_target.has_value()) {
                        const auto depth_state = resource_states_.find(
                            device_ir::ResourceHandle{*value.depth_target});
                        if (depth_state == resource_states_.end() ||
                            (depth_state->second != device_ir::ResourceState::depth_write &&
                             depth_state->second != device_ir::ResourceState::depth_read)) {
                            return core::Result<void>::failure(validation(
                                "CPU rendering uses a depth resource in the wrong state"));
                        }
                    }
                    auto color = textures_.resolve(value.color_target);
                    if (!is_color_format(color.value()->descriptor.format)) {
                        return core::Result<void>::failure(validation(
                            "rendering requires a color target"));
                    }
                    if (value.depth_target.has_value()) {
                        auto depth = textures_.resolve(*value.depth_target);
                        if (!depth || !gpu::is_depth_format(depth.value()->descriptor.format) ||
                            depth.value()->descriptor.width != color.value()->descriptor.width ||
                            depth.value()->descriptor.height != color.value()->descriptor.height) {
                            return core::Result<void>::failure(validation(
                                "CPU rendering requires a matching depth target"));
                        }
                    }
                    state.color_target = value.color_target;
                    state.depth_target = value.depth_target;
                    state.viewport = Viewport{
                        0.0F,
                        0.0F,
                        static_cast<float>(color.value()->descriptor.width),
                        static_cast<float>(color.value()->descriptor.height),
                        0.0F,
                        1.0F,
                    };
                    state.scissor = Scissor{
                        0,
                        0,
                        color.value()->descriptor.width,
                        color.value()->descriptor.height,
                    };
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdEndRendering>) {
                    state.color_target.reset();
                    state.depth_target.reset();
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdSetViewport>) {
                    state.viewport = Viewport{
                        value.x, value.y, value.width, value.height,
                        value.min_depth, value.max_depth};
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdSetScissor>) {
                    state.scissor = Scissor{value.x, value.y, value.width, value.height};
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindPipeline>) {
                    state.pipeline = value.pipeline;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindVertexBuffer>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.buffer},
                            device_ir::ResourceState::vertex_read); !result) {
                        return result;
                    }
                    state.vertex_buffer = value;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdBindIndexBuffer>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.buffer},
                            device_ir::ResourceState::index_read); !result) {
                        return result;
                    }
                    state.index_buffer = value;
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdPushConstants>) {
                    if (value.bytes.size() >= sizeof(state.push_color)) {
                        std::memcpy(state.push_color.data(), value.bytes.data(), sizeof(state.push_color));
                        if (!finite(state.push_color)) {
                            return core::Result<void>::failure(invalid(
                                "CPU reference push color must be finite"));
                        }
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdDraw>) {
                    return core::Result<void>::failure(unsupported(
                        "CPU reference device v1 requires indexed triangle draws"));
                } else if constexpr (std::is_same_v<T, device_ir::CmdDrawIndexed>) {
                    return draw_indexed(value, state);
                } else if constexpr (std::is_same_v<T, device_ir::CmdDispatch>) {
                    return core::Result<void>::failure(unsupported(
                        "CPU reference compute execution is not implemented in v1"));
                } else if constexpr (std::is_same_v<T, device_ir::CmdWriteTimestamp>) {
                    timestamps_[value.query] = static_cast<std::uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count());
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdResolveTimestamps>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.destination},
                            device_ir::ResourceState::copy_destination); !result) {
                        return result;
                    }
                    auto destination = buffers_.resolve(value.destination);
                    const std::uint64_t bytes = static_cast<std::uint64_t>(value.query_count) *
                        sizeof(std::uint64_t);
                    if (add_overflows(value.destination_offset, bytes) ||
                        value.destination_offset + bytes > destination.value()->bytes.size()) {
                        return core::Result<void>::failure(invalid(
                            "timestamp resolve exceeds its destination"));
                    }
                    for (std::uint32_t index = 0U; index < value.query_count; ++index) {
                        const auto timestamp = timestamps_.contains(value.first_query + index)
                            ? timestamps_.at(value.first_query + index) : 0U;
                        std::memcpy(
                            destination.value()->bytes.data() + value.destination_offset +
                                static_cast<std::uint64_t>(index) * sizeof(std::uint64_t),
                            &timestamp,
                            sizeof(timestamp));
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdWaitTimeline>) {
                    if (value.serial > completed_serial_) {
                        return core::Result<void>::failure(core::Diagnostic(
                            ErrorCode::invalid_state,
                            "CPU reference timeline wait is ahead of completion"));
                    }
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdSignalTimeline>) {
                    signaled_serial_ = std::max(signaled_serial_, value.serial);
                    return core::Result<void>::success();
                } else if constexpr (std::is_same_v<T, device_ir::CmdPresent>) {
                    if (auto result = require_state(
                            device_ir::ResourceHandle{value.source},
                            device_ir::ResourceState::present); !result) {
                        return result;
                    }
                    return core::Result<void>::failure(unsupported(
                        "CPU reference device does not provide presentation"));
                }
                return core::Result<void>::success();
            },
            command);
        if (!result) return core::Result<gpu::SubmissionSerial>::failure(result.error());
    }

    submitted_serial_ += gpu::SubmissionSerial{1};
    completed_serial_ = submitted_serial_;
    return core::Result<gpu::SubmissionSerial>::success(submitted_serial_);
}

gpu::SubmissionSerial CpuDevice::completed_serial() const noexcept {
    return completed_serial_;
}

core::Result<void> CpuDevice::wait(gpu::SubmissionSerial serial) {
    if (serial == 0U || serial > completed_serial_) {
        return core::Result<void>::failure(core::Diagnostic(
            ErrorCode::invalid_state, "CPU reference wait requested an incomplete serial"));
    }
    return core::Result<void>::success();
}

core::Result<void> CpuDevice::draw_indexed(
    const device_ir::CmdDrawIndexed& command,
    ExecutionState& state) {
    if (!state.color_target.has_value() || !state.pipeline.has_value() ||
        !state.vertex_buffer.has_value() || !state.index_buffer.has_value()) {
        return core::Result<void>::failure(validation(
            "indexed draw requires rendering, pipeline, vertex, and index bindings"));
    }
    if (command.instance_count != 1U) {
        return core::Result<void>::failure(unsupported(
            "CPU reference device v1 supports one draw instance"));
    }
    const auto pipeline = pipelines_.resolve(*state.pipeline);
    const auto vertices = buffers_.resolve(state.vertex_buffer->buffer);
    const auto indices = buffers_.resolve(state.index_buffer->buffer);
    if (!pipeline || !vertices || !indices) {
        return core::Result<void>::failure(validation("indexed draw binding became stale"));
    }
    const std::uint64_t index_width = state.index_buffer->format == device_ir::IndexFormat::uint16
        ? sizeof(std::uint16_t) : sizeof(std::uint32_t);
    for (std::uint32_t triangle = 0U; triangle < command.index_count; triangle += 3U) {
        std::array<ClipVertex, 3U> clip_vertices{};
        for (std::uint32_t corner = 0U; corner < 3U; ++corner) {
            const std::uint64_t index_position = static_cast<std::uint64_t>(command.first_index) +
                triangle + corner;
            if (multiply_overflows(index_position, index_width) ||
                add_overflows(state.index_buffer->offset, index_position * index_width) ||
                state.index_buffer->offset + index_position * index_width + index_width >
                    indices.value()->bytes.size()) {
                return core::Result<void>::failure(invalid("indexed draw reads past its index buffer"));
            }
            std::uint32_t raw_index = 0U;
            const auto* index_bytes = indices.value()->bytes.data() +
                state.index_buffer->offset + index_position * index_width;
            if (state.index_buffer->format == device_ir::IndexFormat::uint16) {
                std::uint16_t short_index = 0U;
                std::memcpy(&short_index, index_bytes, sizeof(short_index));
                raw_index = short_index;
            } else {
                std::memcpy(&raw_index, index_bytes, sizeof(raw_index));
            }
            const std::int64_t signed_vertex = static_cast<std::int64_t>(raw_index) +
                static_cast<std::int64_t>(command.vertex_offset);
            if (signed_vertex < 0) {
                return core::Result<void>::failure(invalid("indexed draw has a negative vertex index"));
            }
            const std::uint64_t vertex_position = static_cast<std::uint64_t>(signed_vertex);
            if (multiply_overflows(vertex_position, state.vertex_buffer->stride_bytes) ||
                add_overflows(state.vertex_buffer->offset,
                              vertex_position * state.vertex_buffer->stride_bytes) ||
                state.vertex_buffer->offset + vertex_position * state.vertex_buffer->stride_bytes +
                    sizeof(float) * 4U > vertices.value()->bytes.size()) {
                return core::Result<void>::failure(invalid("indexed draw reads past its vertex buffer"));
            }
            std::memcpy(
                clip_vertices[corner].position.data(),
                vertices.value()->bytes.data() + state.vertex_buffer->offset +
                    vertex_position * state.vertex_buffer->stride_bytes,
                sizeof(float) * 4U);
            if (state.vertex_buffer->stride_bytes >= sizeof(float) * 6U) {
                std::memcpy(
                    clip_vertices[corner].uv.data(),
                    vertices.value()->bytes.data() + state.vertex_buffer->offset +
                        vertex_position * state.vertex_buffer->stride_bytes + sizeof(float) * 4U,
                    sizeof(float) * 2U);
            }
            for (const float coordinate : clip_vertices[corner].position) {
                if (!std::isfinite(coordinate)) {
                    return core::Result<void>::failure(invalid(
                        "CPU reference vertex positions must be finite"));
                    }
            }
            for (const float coordinate : clip_vertices[corner].uv) {
                if (!std::isfinite(coordinate)) {
                    return core::Result<void>::failure(invalid(
                        "CPU reference vertex UVs must be finite"));
                }
            }
        }
        if (auto result = rasterize_triangle(state, clip_vertices); !result) return result;
    }
    return core::Result<void>::success();
}

core::Result<void> CpuDevice::rasterize_triangle(
    ExecutionState& state,
    const std::array<ClipVertex, 3U>& vertices) {
    auto target = textures_.resolve(*state.color_target);
    auto pipeline = pipelines_.resolve(*state.pipeline);
    if (!target || !pipeline) return core::Result<void>::failure(validation(
        "CPU raster target or pipeline became stale"));
    if (target.value()->descriptor.format != pipeline.value()->descriptor.color_format) {
        return core::Result<void>::failure(validation(
            "CPU pipeline color format does not match the render target"));
    }
    if (pipeline.value()->descriptor.depth_test && !state.depth_target.has_value()) {
        return core::Result<void>::failure(validation(
            "depth-tested CPU pipeline requires a depth target"));
    }
    if (pipeline.value()->descriptor.depth_test) {
        auto depth_target = textures_.resolve(*state.depth_target);
        if (!depth_target || depth_target.value()->descriptor.format !=
                pipeline.value()->descriptor.depth_format) {
            return core::Result<void>::failure(validation(
                "CPU pipeline depth format does not match the depth target"));
        }
        if (pipeline.value()->descriptor.depth_write) {
            const auto state_it = resource_states_.find(
                device_ir::ResourceHandle{*state.depth_target});
            if (state_it == resource_states_.end() ||
                state_it->second != device_ir::ResourceState::depth_write) {
                return core::Result<void>::failure(validation(
                    "CPU depth writes require a depth-write resource state"));
            }
        }
    }

    if (pipeline.value()->samples_rgba32f && !state.sampled_texture.has_value()) {
        return core::Result<void>::failure(validation(
            "sampled CPU pipeline requires a bound texture"));
    }

    struct ScreenVertex {
        float x;
        float y;
        float z;
        float u;
        float v;
        float inverse_w;
    };
    std::array<ScreenVertex, 3U> screen{};
    for (std::size_t index = 0U; index < 3U; ++index) {
        const float w = vertices[index].position[3];
        if (std::abs(w) <= std::numeric_limits<float>::epsilon()) {
            return core::Result<void>::failure(validation(
                "CPU rasterizer rejects a vertex with zero clip-space w"));
        }
        const float ndc_x = vertices[index].position[0] / w;
        const float ndc_y = vertices[index].position[1] / w;
        const float ndc_z = vertices[index].position[2] / w;
        screen[index] = {
            state.viewport.x + (ndc_x * 0.5F + 0.5F) * state.viewport.width,
            state.viewport.y + (0.5F - ndc_y * 0.5F) * state.viewport.height,
            state.viewport.min_depth + ndc_z *
                (state.viewport.max_depth - state.viewport.min_depth),
            vertices[index].uv[0],
            vertices[index].uv[1],
            1.0F / w,
        };
    }
    const auto edge = [](ScreenVertex a, ScreenVertex b, float x, float y) {
        return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
    };
    const float area = edge(screen[0], screen[1], screen[2].x, screen[2].y);
    if (std::abs(area) <= 1e-6F) return core::Result<void>::success();

    const float min_x = std::min({screen[0].x, screen[1].x, screen[2].x});
    const float max_x = std::max({screen[0].x, screen[1].x, screen[2].x});
    const float min_y = std::min({screen[0].y, screen[1].y, screen[2].y});
    const float max_y = std::max({screen[0].y, screen[1].y, screen[2].y});
    const auto width = static_cast<std::int32_t>(target.value()->descriptor.width);
    const auto height = static_cast<std::int32_t>(target.value()->descriptor.height);
    const auto left = std::max<std::int32_t>(
        0, std::max(state.scissor.x, static_cast<std::int32_t>(std::floor(min_x))));
    const auto right = std::min<std::int32_t>(
        width - 1,
        std::min(state.scissor.x + static_cast<std::int32_t>(state.scissor.width) - 1,
                 static_cast<std::int32_t>(std::ceil(max_x))));
    const auto top = std::max<std::int32_t>(
        0, std::max(state.scissor.y, static_cast<std::int32_t>(std::floor(min_y))));
    const auto bottom = std::min<std::int32_t>(
        height - 1,
        std::min(state.scissor.y + static_cast<std::int32_t>(state.scissor.height) - 1,
                 static_cast<std::int32_t>(std::ceil(max_y))));
    if (left > right || top > bottom) return core::Result<void>::success();

    const bool positive = area > 0.0F;
    for (std::int32_t y = top; y <= bottom; ++y) {
        for (std::int32_t x = left; x <= right; ++x) {
            const float px = static_cast<float>(x) + 0.5F;
            const float py = static_cast<float>(y) + 0.5F;
            const float w0 = edge(screen[1], screen[2], px, py);
            const float w1 = edge(screen[2], screen[0], px, py);
            const float w2 = edge(screen[0], screen[1], px, py);
            if ((positive && (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F)) ||
                (!positive && (w0 > 0.0F || w1 > 0.0F || w2 > 0.0F))) {
                continue;
            }
            const float reciprocal_area = 1.0F / area;
            const float depth = (w0 * screen[0].z + w1 * screen[1].z + w2 * screen[2].z) *
                reciprocal_area;
            const float perspective_weight =
                w0 * screen[0].inverse_w + w1 * screen[1].inverse_w +
                w2 * screen[2].inverse_w;
            const std::size_t pixel = static_cast<std::size_t>(y) *
                target.value()->descriptor.width + static_cast<std::size_t>(x);
            if (pipeline.value()->descriptor.depth_test) {
                auto depth_target = textures_.resolve(*state.depth_target);
                if (!depth_target || depth_target.value()->depth.size() <= pixel) {
                    return core::Result<void>::failure(validation(
                        "CPU depth target does not cover the color target"));
                }
                if (!compare_depth(pipeline.value()->descriptor.depth_compare,
                                   depth, depth_target.value()->depth[pixel])) {
                    continue;
                }
                if (pipeline.value()->descriptor.depth_write) {
                    depth_target.value()->depth[pixel] = depth;
                }
            }
            auto output = state.push_color;
            if (pipeline.value()->samples_rgba32f) {
                if (std::abs(perspective_weight) <= std::numeric_limits<float>::epsilon()) {
                    return core::Result<void>::failure(validation(
                        "CPU sampled rasterization produced a zero perspective weight"));
                }
                const float u = (w0 * screen[0].u * screen[0].inverse_w +
                                 w1 * screen[1].u * screen[1].inverse_w +
                                 w2 * screen[2].u * screen[2].inverse_w) / perspective_weight;
                const float v = (w0 * screen[0].v * screen[0].inverse_w +
                                 w1 * screen[1].v * screen[1].inverse_w +
                                 w2 * screen[2].v * screen[2].inverse_w) / perspective_weight;
                const auto sampled = textures_.resolve(
                    std::get<gpu::TextureHandle>(state.sampled_texture->resource));
                if (!sampled || sampled.value()->descriptor.format != gpu::Format::rgba32_float) {
                    return core::Result<void>::failure(validation(
                        "CPU sampled rasterization lost its RGBA32F texture"));
                }
                gpu::SamplerDesc sampler;
                if (state.sampled_texture->sampler.has_value()) {
                    const auto resolved_sampler = samplers_.resolve(
                        *state.sampled_texture->sampler);
                    if (!resolved_sampler) {
                        return core::Result<void>::failure(resolved_sampler.error());
                    }
                    sampler = resolved_sampler.value()->descriptor;
                }
                const auto sample = sample_rgba32f(*sampled.value(), sampler, u, v);
                for (std::size_t channel = 0U; channel < output.size(); ++channel) {
                    output[channel] *= sample[channel];
                }
            }
            if (pipeline.value()->descriptor.blend) {
                const std::size_t base = pixel * 4U;
                const float source_factor = blend_factor(
                    pipeline.value()->descriptor.source_color, output[3]);
                const float destination_factor = blend_factor(
                    pipeline.value()->descriptor.destination_color, output[3]);
                for (std::size_t channel = 0U; channel < 4U; ++channel) {
                    output[channel] = output[channel] * source_factor +
                        target.value()->color[base + channel] * destination_factor;
                }
            }
            const std::size_t base = pixel * 4U;
            std::copy(output.begin(), output.end(),
                      target.value()->color.begin() + static_cast<std::ptrdiff_t>(base));
        }
    }
    return core::Result<void>::success();
}

std::array<float, 4U> CpuDevice::sample_rgba32f(
    const TextureResource& texture,
    const gpu::SamplerDesc& sampler,
    float u,
    float v) const {
    const auto addressed_u = address_coordinate(u, sampler.u);
    const auto addressed_v = address_coordinate(v, sampler.v);
    if (!addressed_u.has_value() || !addressed_v.has_value()) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }

    const auto width = texture.descriptor.width;
    const auto height = texture.descriptor.height;
    const auto texel = [&texture, width](std::uint32_t x, std::uint32_t y) {
        const std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4U;
        return std::array<float, 4U>{
            texture.color[base], texture.color[base + 1U],
            texture.color[base + 2U], texture.color[base + 3U]};
    };
    const float x = *addressed_u * static_cast<float>(width - 1U);
    const float y = *addressed_v * static_cast<float>(height - 1U);
    if (sampler.mag_filter == gpu::Filter::nearest || width == 1U || height == 1U) {
        const auto ix = static_cast<std::uint32_t>(std::clamp(
            std::floor(x + 0.5F), 0.0F, static_cast<float>(width - 1U)));
        const auto iy = static_cast<std::uint32_t>(std::clamp(
            std::floor(y + 0.5F), 0.0F, static_cast<float>(height - 1U)));
        return texel(ix, iy);
    }

    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const auto x1 = std::min(x0 + 1U, width - 1U);
    const auto y1 = std::min(y0 + 1U, height - 1U);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto top_left = texel(x0, y0);
    const auto top_right = texel(x1, y0);
    const auto bottom_left = texel(x0, y1);
    const auto bottom_right = texel(x1, y1);
    std::array<float, 4U> result{};
    for (std::size_t channel = 0U; channel < result.size(); ++channel) {
        const float top = top_left[channel] * (1.0F - fx) + top_right[channel] * fx;
        const float bottom = bottom_left[channel] * (1.0F - fx) +
            bottom_right[channel] * fx;
        result[channel] = top * (1.0F - fy) + bottom * fy;
    }
    return result;
}

} // namespace carto::device_cpu
