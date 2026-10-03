#include <carto/d3d12/device.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using carto::d3d12::D3D12Device;
using carto::gpu::BufferHandle;
using carto::gpu::TextureHandle;

struct Options {
    std::uint32_t extent = 1024U;
    std::size_t iterations = 32U;
};

struct Timing {
    double cpu_microseconds = 0.0;
    double gpu_microseconds = 0.0;
};

template <typename T>
T expect(carto::core::Result<T> result, std::string_view operation) {
    if (!result) {
        throw std::runtime_error(
            std::string(operation) + ": " + result.error().message);
    }
    return std::move(result).value();
}

void expect_void(const auto& result, std::string_view operation) {
    if (!result) {
        throw std::runtime_error(
            std::string(operation) + ": " + result.error().message);
    }
}

std::string read_shader(std::string_view name) {
#ifdef CARTO_D3D12_SHADER_DIR
    const auto path = std::filesystem::path(CARTO_D3D12_SHADER_DIR) / name;
#else
    const auto path = std::filesystem::path("libs/d3d12/shaders") / name;
#endif
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to read shader: " + path.string());
    return std::string(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(&value);
    return std::vector<std::uint8_t>(begin, begin + sizeof(T));
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto next_value = [&](std::string_view option) {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(option) + " requires a value");
            }
            return std::string_view(argv[++index]);
        };
        if (argument == "--extent") {
            options.extent = static_cast<std::uint32_t>(
                std::stoul(std::string(next_value(argument))));
        } else if (argument == "--iterations") {
            options.iterations = static_cast<std::size_t>(
                std::stoull(std::string(next_value(argument))));
        } else if (argument == "--help") {
            std::cout << "usage: cartographer_d3d12_kernel_bench"
                         " [--extent pixels] [--iterations count]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + std::string(argument));
        }
    }
    if (options.extent < 8U || options.extent > 8192U || options.extent % 8U != 0U) {
        throw std::runtime_error("extent must be a multiple of 8 in [8, 8192]");
    }
    if (options.iterations == 0U || options.iterations > 4096U) {
        throw std::runtime_error("iterations must be in [1, 4096]");
    }
    return options;
}

std::array<std::uint64_t, 2U> read_timestamps(
    D3D12Device& device,
    BufferHandle readback) {
    const auto bytes = expect(
        device.read_buffer(readback, 0U, 2U * sizeof(std::uint64_t)),
        "read timestamp resolve");
    std::array<std::uint64_t, 2U> timestamps{};
    std::memcpy(timestamps.data(), bytes.data(), sizeof(timestamps));
    if (timestamps[1U] < timestamps[0U]) {
        throw std::runtime_error("GPU timestamp interval moved backwards");
    }
    return timestamps;
}

void append_temporal_dispatch(
    carto::device_ir::DeviceCommandStream& stream,
    carto::gpu::PipelineHandle pipeline,
    TextureHandle current,
    TextureHandle history,
    TextureHandle motion,
    TextureHandle resolved,
    const std::vector<std::uint8_t>& constants,
    std::uint32_t groups) {
    stream.append(carto::device_ir::CmdBindPipeline{pipeline});
    stream.append(carto::device_ir::CmdBindResources{{
        carto::device_ir::ResourceBinding{
            0U, 0U, carto::device_ir::ResourceHandle{current}, std::nullopt},
        carto::device_ir::ResourceBinding{
            0U, 1U, carto::device_ir::ResourceHandle{history}, std::nullopt},
        carto::device_ir::ResourceBinding{
            0U, 2U, carto::device_ir::ResourceHandle{motion}, std::nullopt},
        carto::device_ir::ResourceBinding{
            0U, 3U, carto::device_ir::ResourceHandle{resolved}, std::nullopt},
    }});
    stream.append(carto::device_ir::CmdPushConstants{constants});
    stream.append(carto::device_ir::CmdDispatch{groups, groups, 1U});
}

Timing run_temporal_batch(
    D3D12Device& device,
    carto::gpu::PipelineHandle pipeline,
    TextureHandle current,
    TextureHandle history,
    TextureHandle motion,
    TextureHandle resolved,
    BufferHandle timestamp_readback,
    const std::vector<std::uint8_t>& constants,
    std::uint32_t groups,
    std::size_t iterations,
    bool initialize_states) {
    carto::device_ir::DeviceCommandStream stream;
    if (initialize_states) {
        stream.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{current},
            carto::device_ir::ResourceState::undefined,
            carto::device_ir::ResourceState::shader_read,
        });
        stream.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{history},
            carto::device_ir::ResourceState::undefined,
            carto::device_ir::ResourceState::shader_read,
        });
        stream.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{motion},
            carto::device_ir::ResourceState::undefined,
            carto::device_ir::ResourceState::shader_read,
        });
        stream.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{resolved},
            carto::device_ir::ResourceState::undefined,
            carto::device_ir::ResourceState::shader_write,
        });
        stream.append(carto::device_ir::CmdTransitionResource{
            carto::device_ir::ResourceHandle{timestamp_readback},
            carto::device_ir::ResourceState::undefined,
            carto::device_ir::ResourceState::copy_destination,
        });
    }
    stream.append(carto::device_ir::CmdWriteTimestamp{0U});
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        append_temporal_dispatch(
            stream, pipeline, current, history, motion, resolved, constants, groups);
        stream.append(carto::device_ir::CmdUavBarrier{
            carto::device_ir::ResourceHandle{resolved}});
    }
    stream.append(carto::device_ir::CmdWriteTimestamp{1U});
    stream.append(carto::device_ir::CmdResolveTimestamps{
        timestamp_readback, 0U, 2U, 0U});

    const auto started = std::chrono::steady_clock::now();
    const auto submitted = device.submit(stream, carto::gpu::QueueType::compute);
    if (!submitted) {
        std::ostringstream message;
        message << "submit temporal workload: " << submitted.error().message;
        for (const auto& receipt : device.debug_receipts()) {
            message << " | " << receipt;
        }
        throw std::runtime_error(message.str());
    }
    const auto serial = submitted.value();
    expect_void(device.wait(serial), "wait temporal workload");
    const auto finished = std::chrono::steady_clock::now();
    const auto timestamps = read_timestamps(device, timestamp_readback);
    const double frequency = static_cast<double>(
        device.timestamp_frequency(carto::gpu::QueueType::compute));
    if (frequency <= 0.0) throw std::runtime_error("compute timestamp frequency is zero");
    return Timing{
        std::chrono::duration<double, std::micro>(finished - started).count(),
        static_cast<double>(timestamps[1U] - timestamps[0U]) * 1.0e6 / frequency,
    };
}

void destroy_temporal_resources(
    D3D12Device& device,
    carto::gpu::ShaderHandle shader,
    carto::gpu::PipelineHandle pipeline,
    TextureHandle current,
    TextureHandle history,
    TextureHandle motion,
    TextureHandle resolved,
    BufferHandle timestamp_readback) {
    expect_void(device.destroy_buffer(timestamp_readback), "destroy timestamp readback");
    expect_void(device.destroy_texture(resolved), "destroy temporal resolved texture");
    expect_void(device.destroy_texture(motion), "destroy temporal motion texture");
    expect_void(device.destroy_texture(history), "destroy temporal history texture");
    expect_void(device.destroy_texture(current), "destroy temporal current texture");
    expect_void(device.destroy_pipeline(pipeline), "destroy temporal pipeline");
    expect_void(device.destroy_shader(shader), "destroy temporal shader");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        auto created = D3D12Device::create(
            carto::d3d12::D3D12DeviceOptions{{}, false, true});
        const auto device = expect(std::move(created), "create D3D12 device");
        if (!device->capabilities().timestamp_queries) {
            throw std::runtime_error("D3D12 timestamp queries are unavailable");
        }

        const auto shader = expect(
            carto::d3d12::compile_hlsl(carto::d3d12::ShaderSource{
                "cartographer_temporal_bench",
                carto::gpu::ShaderStage::compute,
                "CSMain",
                "cartographer_temporal.hlsl",
                read_shader("cartographer_temporal.hlsl"),
                true,
                false,
            }),
            "compile temporal shader");
        const auto shader_handle = expect(device->create_shader(shader), "create temporal shader");

        carto::gpu::PipelineDesc pipeline_descriptor;
        pipeline_descriptor.debug_name = "cartographer_temporal_bench";
        pipeline_descriptor.shaders = {shader_handle};
        pipeline_descriptor.descriptor_bindings = {
            carto::gpu::DescriptorBinding{0U, 0U, carto::gpu::ShaderStage::compute},
            carto::gpu::DescriptorBinding{0U, 1U, carto::gpu::ShaderStage::compute},
            carto::gpu::DescriptorBinding{0U, 2U, carto::gpu::ShaderStage::compute},
            carto::gpu::DescriptorBinding{0U, 3U, carto::gpu::ShaderStage::compute},
        };
        pipeline_descriptor.sampled_texture_count = 3U;
        pipeline_descriptor.depth_test = false;
        pipeline_descriptor.push_constant_bytes = 48U;
        const auto pipeline = expect(
            device->create_pipeline(pipeline_descriptor), "create temporal pipeline");

        const auto make_texture = [&](carto::gpu::Format format,
                                      carto::gpu::TextureUsage usage,
                                      std::array<float, 4U> clear) {
            return expect(device->create_texture(carto::gpu::TextureDesc{
                options.extent,
                options.extent,
                1U,
                1U,
                1U,
                format,
                carto::gpu::TextureDimension::texture_2d,
                usage,
                clear,
                1.0F,
            }), "create temporal texture");
        };
        const auto current = make_texture(
            carto::gpu::Format::rgba32_float,
            carto::gpu::TextureUsage::sampled,
            {0.15F, 0.25F, 0.35F, 1.0F});
        const auto history = make_texture(
            carto::gpu::Format::rgba32_float,
            carto::gpu::TextureUsage::sampled,
            {0.35F, 0.25F, 0.15F, 1.0F});
        const auto motion = make_texture(
            carto::gpu::Format::rg32_float,
            carto::gpu::TextureUsage::sampled,
            {0.0F, 0.0F, 0.0F, 0.0F});
        const auto resolved = make_texture(
            carto::gpu::Format::rgba32_float,
            carto::gpu::TextureUsage::storage,
            {0.0F, 0.0F, 0.0F, 1.0F});
        const auto timestamp_readback = expect(device->create_buffer(carto::gpu::BufferDesc{
            2U * sizeof(std::uint64_t),
            carto::gpu::MemoryClass::readback,
            carto::gpu::BufferUsage::transfer_destination,
        }), "create timestamp readback");

        struct TemporalConstants {
            std::array<float, 4U> feedback_and_clamp{0.85F, 0.0F, 1.0F, 0.0F};
            std::array<float, 4U> jitter_and_inverse_extent{
                0.0F, 0.0F, 1.0F / static_cast<float>(options.extent),
                1.0F / static_cast<float>(options.extent)};
            std::array<std::uint32_t, 4U> history_flags{1U, 0U, 0U, 0U};
        };
        const auto constants = bytes_of(TemporalConstants{});
        const std::uint32_t groups = options.extent / 8U;

        // Warm the shader, allocator, descriptor heaps, and driver state before
        // collecting any samples. A single dispatch is not enough to leave the
        // P5200's P8 power-save state, so use a bounded workload warmup and
        // report its size with the receipt.
        const std::size_t warmup_iterations = std::max<std::size_t>(
            16U, std::min(options.iterations, std::size_t{32U}));
        static_cast<void>(run_temporal_batch(
            *device, pipeline, current, history, motion, resolved, timestamp_readback,
            constants, groups, warmup_iterations, true));

        const Timing batched = run_temporal_batch(
            *device, pipeline, current, history, motion, resolved, timestamp_readback,
            constants, groups, options.iterations, false);

        double per_submit_cpu = 0.0;
        double per_submit_gpu = 0.0;
        for (std::size_t iteration = 0U; iteration < options.iterations; ++iteration) {
            const Timing sample = run_temporal_batch(
                *device, pipeline, current, history, motion, resolved, timestamp_readback,
                constants, groups, 1U, false);
            per_submit_cpu += sample.cpu_microseconds;
            per_submit_gpu += sample.gpu_microseconds;
        }

        const double pixels = static_cast<double>(options.extent) *
            static_cast<double>(options.extent) * static_cast<double>(options.iterations);
        std::cout << std::fixed << std::setprecision(3)
                  << "kernel,extent,iterations,warmup_dispatches,submissions,pixels,cpu_total_us,"
                     "cpu_us_per_dispatch,gpu_total_us,gpu_us_per_dispatch,gpu_mpix_per_s,"
                     "timestamp_frequency\n"
                  << "temporal_batched," << options.extent << ',' << options.iterations << ','
                  << warmup_iterations << ",1,"
                  << pixels << ',' << batched.cpu_microseconds << ','
                  << batched.cpu_microseconds / static_cast<double>(options.iterations) << ','
                  << batched.gpu_microseconds << ','
                  << batched.gpu_microseconds / static_cast<double>(options.iterations) << ','
                  << pixels / batched.gpu_microseconds << ','
                  << device->timestamp_frequency(carto::gpu::QueueType::compute) << '\n'
                  << "temporal_per_submit," << options.extent << ',' << options.iterations << ','
                  << warmup_iterations << ',' << options.iterations << ',' << pixels << ','
                  << per_submit_cpu << ','
                  << per_submit_cpu / static_cast<double>(options.iterations) << ','
                  << per_submit_gpu << ','
                  << per_submit_gpu / static_cast<double>(options.iterations) << ','
                  << pixels / per_submit_gpu << ','
                  << device->timestamp_frequency(carto::gpu::QueueType::compute) << '\n';

        if (!device->debug_receipts().empty()) {
            std::ostringstream message;
            message << "D3D12 debug receipts:";
            for (const auto& receipt : device->debug_receipts()) message << " | " << receipt;
            throw std::runtime_error(message.str());
        }
        destroy_temporal_resources(
            *device, shader_handle, pipeline, current, history, motion, resolved,
            timestamp_readback);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 kernel benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
