// Based on the native 4K acceptance fixture in Cartographer commit 19864cfc.
// This is a resident-target smoke test, not visual or prop-quality acceptance.
#include "authoring_viewport_fixture.hpp"
#include <carto/d3d12/authoring_viewport.hpp>
#include <chrono>
#include <iostream>

int main() {
    try {
        auto f = fixture();
        auto source = std::make_shared<const carto::project::ProjectDocument>(std::move(f.document));
        auto renderer = take(carto::d3d12::AuthoringViewport::create());
        carto::production::AuthoringViewportRequest request;
        request.width = 3840;
        request.height = 2161;
        request.temporal.enabled = true;
        request.temporal.session = 1;
        check(!renderer->render_resident(source, request), "oversized viewport must reject");
        request.height = 2160;
        const auto start = std::chrono::steady_clock::now();
        auto frame = take(renderer->render_resident(source, request));
        const auto host_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        check(bool(frame.resident_image), "4K native resident texture");
        check(frame.image.width == 3840 && frame.image.height == 2160, "4K extent preserved");
        std::cout << "4K native temporal resident target passed; oversized rejected; device="
                  << frame.device_name << "; host_ms=" << host_ms
                  << "; raster_gpu_ms=" << frame.gpu_milliseconds
                  << "; temporal_motion_gpu_ms=" << frame.gpu_temporal_motion_ms
                  << "; temporal_resolve_gpu_ms=" << frame.gpu_temporal_resolve_ms
                  << "; logical_resident_bytes=" << frame.logical_resident_bytes
                  << "; temporal_resident_bytes=" << frame.temporal_resident_bytes << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
