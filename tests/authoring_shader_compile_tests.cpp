#include "authoring_shader.hpp"
#include <carto/d3d12/device.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

// Deliberately CPU-only: DXC emits DXIL, but no device, queue or window exists.
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--expect-missing-validator") {
            const auto shader = carto::d3d12::compile_hlsl(
                {"validator.boundary", carto::gpu::ShaderStage::vertex, "VSMain", "boundary.hlsl",
                 "float4 VSMain(uint i:SV_VertexID):SV_POSITION{return float4(0,0,0,1);}",
                 true, false});
            if (shader || shader.error().message.find("DXC validator") == std::string::npos)
                throw std::runtime_error("missing paired DXC validator must fail explicitly");
            std::cout << "missing paired DXC validator rejected\n";
            return 0;
        }
        for (unsigned mode = 0; mode < 4; ++mode) {
            const auto code =
                (mode ? std::string("#define CARTO_FUSED_IDENTITY 1\n") : std::string{}) +
                (mode == 3 ? std::string("#define CARTO_SCENE_REFRACTION 1\n") : std::string{}) +
                (mode >= 2 ? std::string("#define CARTO_AUTHORING_SURFACE 1\n") +
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
                if (mode == 0 && stage == carto::gpu::ShaderStage::vertex) {
                    if (!carto::d3d12::validate_dxil(shader.value().bytes))
                        throw std::runtime_error("signed shader rejected by cache validation");
                    auto unsigned_bytes = shader.value().bytes;
                    if (unsigned_bytes.size() < 20U)
                        throw std::runtime_error("DXIL container is too short");
                    std::fill(unsigned_bytes.begin() + 4, unsigned_bytes.begin() + 20, 0U);
                    if (carto::d3d12::validate_dxil(unsigned_bytes))
                        throw std::runtime_error("unsigned DXIL was accepted for cache reuse");
                }
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
