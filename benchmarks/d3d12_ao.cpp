#include <carto/d3d12/bake_graph.hpp>
#include <carto/production/bake_graph.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Distribution {
    double minimum_ms{0.0};
    double median_ms{0.0};
    double p95_ms{0.0};
    double mean_ms{0.0};
};

void require(bool condition, std::string message) {
    if (!condition) throw std::runtime_error(std::move(message));
}

std::uint64_t parse_u64(std::string_view text, std::string_view label) {
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
        std::string(label) + " must be an unsigned integer");
    return value;
}

std::uint32_t parse_u32(std::string_view text, std::string_view label) {
    const std::uint64_t value = parse_u64(text, label);
    require(value <= std::numeric_limits<std::uint32_t>::max(),
        std::string(label) + " exceeds the supported range");
    return static_cast<std::uint32_t>(value);
}

double parse_positive_double(std::string_view text, std::string_view label) {
    const std::string token(text);
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    require(end == token.c_str() + token.size() && std::isfinite(value) && value > 0.0,
        std::string(label) + " must be finite and positive");
    return value;
}

Distribution summarize(std::vector<double> samples) {
    require(!samples.empty(), "benchmark sample set is empty");
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&samples](double rank) {
        const double position = rank * static_cast<double>(samples.size() - 1U);
        const std::size_t lower = static_cast<std::size_t>(std::floor(position));
        const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
        const double fraction = position - static_cast<double>(lower);
        return samples[lower] + (samples[upper] - samples[lower]) * fraction;
    };
    double total = 0.0;
    for (const double sample : samples) total += sample;
    return Distribution{
        samples.front(), percentile(0.50), percentile(0.95),
        total / static_cast<double>(samples.size())};
}

void print_diagnostics(const carto::production::AmbientOcclusionExecutionReport& report) {
    for (const auto& diagnostic : report.diagnostics) {
        std::cerr << "diagnostic code=" << diagnostic.code
                  << " message=" << std::quoted(diagnostic.message) << '\n';
    }
}

void run(int argc, char** argv) {
    require(argc == 12,
        "usage: cartographer_d3d12_ao_bench <project.carto> <mesh-id> <uv-set-id> "
        "<width> <height> <tile-u> <tile-v> <samples> <max-distance> <seed> <repetitions>");

    const std::string project_path(argv[1]);
    const std::uint64_t mesh_id = parse_u64(argv[2], "mesh id");
    const std::uint64_t uv_set_id = parse_u64(argv[3], "UV set id");
    const std::uint32_t width = parse_u32(argv[4], "texture width");
    const std::uint32_t height = parse_u32(argv[5], "texture height");
    const std::uint32_t tile_u = parse_u32(argv[6], "tile U");
    const std::uint32_t tile_v = parse_u32(argv[7], "tile V");
    const std::uint32_t samples = parse_u32(argv[8], "sample count");
    const double max_distance = parse_positive_double(argv[9], "maximum distance");
    const std::uint64_t seed = parse_u64(argv[10], "seed");
    const std::uint64_t repetitions_u64 = parse_u64(argv[11], "repetitions");
    require(mesh_id != 0U && repetitions_u64 >= 3U && repetitions_u64 <= 100U,
        "mesh id must be nonzero and repetitions must be in [3, 100]");
    const std::size_t repetitions = static_cast<std::size_t>(repetitions_u64);

    const auto loaded = carto::project::ProjectDocument::load(project_path);
    require(static_cast<bool>(loaded), loaded ? "" : loaded.error().message);
    const carto::project::ProjectDocument& document = loaded.value();
    const auto mesh = document.meshes().find(mesh_id);
    require(mesh != document.meshes().end(), "requested mesh does not exist in the project");

    const std::string mesh_identity = document.source_namespace() + "/mesh/" +
        std::to_string(mesh_id);
    const auto source = carto::production::bind_bake_source(mesh_identity, mesh->second);
    const auto target = carto::production::bind_bake_target(
        mesh_identity, mesh->second, uv_set_id);
    require(static_cast<bool>(source), source ? "" : source.error().message);
    require(static_cast<bool>(target), target ? "" : target.error().message);

    carto::production::AmbientOcclusionRecipe recipe;
    recipe.binding.identity = "bench/ao/mesh/" + std::to_string(mesh_id) +
        "/uv/" + std::to_string(uv_set_id) + "/tile/" +
        std::to_string(tile_u) + "/" + std::to_string(tile_v);
    recipe.binding.source = source.value();
    recipe.binding.target = target.value();
    recipe.binding.target_uv_set = uv_set_id;
    recipe.binding.width = width;
    recipe.binding.height = height;
    recipe.binding.tile_u = tile_u;
    recipe.binding.tile_v = tile_v;
    recipe.sample_count = samples;
    recipe.max_distance = max_distance;
    recipe.origin_bias = 1e-4;
    recipe.seed = seed;
    const auto recipe_validation = recipe.validate();
    require(static_cast<bool>(recipe_validation), recipe_validation
        ? "" : recipe_validation.error().message);

    const auto device_result = carto::d3d12::D3D12Device::create();
    require(static_cast<bool>(device_result),
        device_result ? "" : device_result.error().message);
    carto::d3d12::D3D12Device& device = *device_result.value();

    constexpr std::size_t kWarmupRuns = 2U;
    std::vector<double> cpu_samples;
    std::vector<double> d3d12_total_samples;
    std::vector<double> d3d12_worker_samples;
    cpu_samples.reserve(repetitions);
    d3d12_total_samples.reserve(repetitions);
    d3d12_worker_samples.reserve(repetitions);
    std::string platform_profile;
    std::string adapter_name;
    std::string adapter_id;
    std::string driver;
    carto::assets::Sha256Digest artifact_digest;
    std::uint64_t covered_texels = 0U;
    std::uint64_t rays_cast = 0U;

    for (std::size_t iteration = 0U; iteration < kWarmupRuns + repetitions; ++iteration) {
        const auto cpu_started = Clock::now();
        const auto cpu = carto::production::compile_ambient_occlusion_cpu(
            recipe, mesh->second, mesh->second);
        const auto cpu_finished = Clock::now();
        if (!cpu.succeeded() || !cpu.artifact.has_value()) {
            print_diagnostics(cpu);
            throw std::runtime_error("CPU AO reference rejected the benchmark request");
        }

        const auto d3d12_started = Clock::now();
        const auto d3d12 = carto::d3d12::compile_ambient_occlusion_d3d12(
            device, recipe, mesh->second, mesh->second);
        const auto d3d12_finished = Clock::now();
        if (!d3d12.succeeded() || !d3d12.projection.has_value() ||
            !d3d12.bake.artifact.has_value()) {
            print_diagnostics(d3d12.bake);
            throw std::runtime_error("D3D12 AO worker rejected the benchmark request");
        }
        require(d3d12.bake.artifact->artifact_digest == cpu.artifact->artifact_digest,
            "D3D12 AO output differs from the CPU reference artifact");

        const auto profile = carto::d3d12::d3d12_bake_platform_profile(
            *d3d12.projection);
        require(static_cast<bool>(profile), profile ? "" : profile.error().message);
        if (platform_profile.empty()) {
            platform_profile = profile.value();
            adapter_name = d3d12.projection->adapter_name;
            adapter_id = d3d12.projection->stable_adapter_id;
            driver = d3d12.projection->driver;
            artifact_digest = d3d12.bake.artifact->artifact_digest;
            covered_texels = d3d12.bake.artifact->covered_texels;
            rays_cast = d3d12.bake.artifact->rays_cast;
        } else {
            require(profile.value() == platform_profile,
                "D3D12 worker profile changed during the benchmark");
            require(d3d12.bake.artifact->artifact_digest == artifact_digest &&
                cpu.artifact->artifact_digest == artifact_digest,
                "AO output changed across deterministic benchmark runs");
        }

        if (iteration >= kWarmupRuns) {
            cpu_samples.push_back(std::chrono::duration<double, std::milli>(
                cpu_finished - cpu_started).count());
            d3d12_total_samples.push_back(std::chrono::duration<double, std::milli>(
                d3d12_finished - d3d12_started).count());
            d3d12_worker_samples.push_back(static_cast<double>(
                d3d12.projection->wall_time_nanoseconds) / 1'000'000.0);
        }
    }

    const auto latest = carto::project::ProjectDocument::load(project_path);
    require(static_cast<bool>(latest), latest ? "" : latest.error().message);
    require(latest.value().source_namespace() == document.source_namespace(),
        "project source namespace changed during the benchmark");
    const auto latest_mesh = latest.value().meshes().find(mesh_id);
    require(latest_mesh != latest.value().meshes().end(),
        "benchmark mesh disappeared from the project");
    require(static_cast<bool>(carto::production::validate_bake_sources(
            recipe.binding, latest_mesh->second, latest_mesh->second)),
        "benchmark source mesh changed during the measurement");

    const Distribution cpu = summarize(std::move(cpu_samples));
    const Distribution total = summarize(std::move(d3d12_total_samples));
    const Distribution worker = summarize(std::move(d3d12_worker_samples));
    const auto digest = recipe.canonical_digest();
    require(static_cast<bool>(digest), digest ? "" : digest.error().message);
    const double rays_cast_double = static_cast<double>(rays_cast);
    const double cpu_rays_per_second = rays_cast_double * 1000.0 / cpu.median_ms;
    const double d3d12_total_rays_per_second = rays_cast_double * 1000.0 / total.median_ms;
    const double d3d12_worker_rays_per_second = rays_cast_double * 1000.0 / worker.median_ms;

    std::cout << std::fixed << std::setprecision(3)
              << "ao_benchmark"
              << " evidence=carto.d3d12.cpu-reference-parity"
              << " source=" << std::quoted(recipe.binding.source.identity)
              << " source_revision=" << recipe.binding.source.revision.value()
              << " source_digest=" << recipe.binding.source.content_digest.hex()
              << " mesh=" << mesh_id
              << " uv_set=" << uv_set_id
              << " tile=" << tile_u << ',' << tile_v
              << " resolution=" << width << 'x' << height
              << " samples=" << samples
              << " covered_texels=" << covered_texels
              << " rays_cast=" << rays_cast
              << " repetitions=" << repetitions
              << " warmup_runs=" << kWarmupRuns
              << " cpu_median_ms=" << cpu.median_ms
              << " cpu_p95_ms=" << cpu.p95_ms
              << " cpu_rays_per_second=" << cpu_rays_per_second
              << " d3d12_total_median_ms=" << total.median_ms
              << " d3d12_total_p95_ms=" << total.p95_ms
              << " d3d12_total_rays_per_second=" << d3d12_total_rays_per_second
              << " d3d12_worker_median_elapsed_ms=" << worker.median_ms
              << " d3d12_worker_p95_elapsed_ms=" << worker.p95_ms
              << " d3d12_worker_rays_per_second=" << d3d12_worker_rays_per_second
              << " adapter=" << std::quoted(adapter_name)
              << " adapter_id=" << std::quoted(adapter_id)
              << " driver=" << std::quoted(driver)
              << " platform_profile=" << std::quoted(platform_profile)
              << " recipe_digest=" << digest.value().hex()
              << " artifact_digest=" << artifact_digest.hex()
              << " performance_claim=none\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        run(argc, argv);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
