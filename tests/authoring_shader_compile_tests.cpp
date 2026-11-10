#include "authoring_shader.hpp"
#include <carto/d3d12/device.hpp>
#include <iostream>
#include <stdexcept>

// Deliberately CPU-only: DXC emits DXIL, but no device, queue or window exists.
int main() {
    try {
        for (unsigned mode = 0; mode < 3; ++mode) {
            const auto code =
                (mode ? std::string("#define CARTO_FUSED_IDENTITY 1\n") : std::string{}) +
                (mode == 2 ? std::string("#define CARTO_AUTHORING_SURFACE 1\n") +
                                 carto_authoring_surface_source
                           : std::string{}) +
                carto_authoring_pbr_source;
            for (auto [stage, entry] :
                 {std::pair{carto::gpu::ShaderStage::vertex, "VSMain"},
                  std::pair{carto::gpu::ShaderStage::fragment, "PSMain"},
                  std::pair{carto::gpu::ShaderStage::fragment, "ShadowMain"}}) {
                auto shader = carto::d3d12::compile_hlsl(
                    {"authoring.cpu-compile", stage, entry, "authoring.hlsl", code, true, false});
                if (!shader)
                    throw std::runtime_error(shader.error().message);
                std::cout << "mode " << mode << ' ' << entry << " DXIL compiled\n";
            }
        }
        static_assert(216 / 4 + 9 + 1 <= 64);
        static_assert(208 / 4 + 9 + 1 <= 64);
        auto display = carto::d3d12::compile_hlsl(
            {"authoring.display.cpu-compile", carto::gpu::ShaderStage::fragment, "PSMain",
             "display.hlsl", carto_authoring_display_source, true, false});
        if (!display)
            throw std::runtime_error(display.error().message);
        std::cout << "CPU DXC shader compilation passed; no GPU instantiated or executed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
