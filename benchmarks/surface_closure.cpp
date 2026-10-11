#include <carto/geometry/primitives.hpp>
#include <carto/surface/core.hpp>

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {

using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        const auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
        require(static_cast<bool>(editable), "failed to create benchmark box");
        const auto compiled = editable.value().compile();
        require(static_cast<bool>(compiled), "failed to compile benchmark box");
        const auto domain = carto::surface::build_surface_domain(
            compiled.value(), carto::surface::SurfaceDomainId{1U}, "benchmark", "concrete");
        require(static_cast<bool>(domain), "failed to build benchmark domain");

        std::cout << "mode,target_triangles,source_triangles,compile_ms,rms_radiance_error,"
                     "max_radiance_error,specular_error,visibility_error,digest\n";
        std::cout << std::fixed << std::setprecision(6);
        constexpr std::array<std::uint32_t, 4U> targets = {12U, 6U, 3U, 1U};
        for (const std::uint32_t target : targets) {
            for (const auto mode : {carto::surface::RepresentationMode::geometry_only,
                                     carto::surface::RepresentationMode::fidelity_closure}) {
                carto::surface::SurfaceClosureCompileOptions options;
                options.target_triangles = target;
                const auto started = Clock::now();
                const auto result = carto::surface::compile_surface_representation(
                    compiled.value(), domain.value(), options, mode);
                const auto finished = Clock::now();
                if (!result) throw std::runtime_error(result.error().message);
                const double milliseconds = std::chrono::duration<double, std::milli>(
                    finished - started).count();
                const auto& error = result.value().error;
                std::cout << (mode == carto::surface::RepresentationMode::fidelity_closure
                                  ? "fidelity_closure"
                                  : "geometry_only")
                          << ',' << target << ',' << compiled.value().indices.size() / 3U << ','
                          << milliseconds << ',' << error.rms_radiance_error << ','
                          << error.max_radiance_error << ',' << error.specular_error << ','
                          << error.visibility_error << ',' << result.value().digest << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "surface closure benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
