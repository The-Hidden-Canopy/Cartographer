// Pending native GPU integration suite. Compilation is CPU-only; execution needs a GPU slot.
#include "authoring_viewport_fixture.hpp"
#include <carto/d3d12/authoring_temporal.hpp>
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/production/authoring_temporal.hpp>
#include <iostream>

namespace {
void compare_temporal(const production::AuthoringViewportImage &expected,
                      const production::AuthoringViewportImage &actual) {
    check(expected.objects == actual.objects && expected.faces == actual.faces,
          "temporal preserves current coverage and picking identities");
    check(expected.linear_hdr.size() == actual.linear_hdr.size(),
          "native temporal full image extent");
    for (std::size_t p = 0; p < expected.linear_hdr.size(); ++p) {
        for (auto pair : {std::pair{expected.linear_hdr[p].x, actual.linear_hdr[p].x},
                          std::pair{expected.linear_hdr[p].y, actual.linear_hdr[p].y},
                          std::pair{expected.linear_hdr[p].z, actual.linear_hdr[p].z}})
            check(std::isfinite(pair.second) &&
                      std::abs(pair.first - pair.second) < 3e-4 + std::abs(pair.first) * 3e-4,
                  "native motion plus resolve full-image HDR matches CPU oracle");
        for (unsigned c = 0; c < 3; ++c)
            check(std::abs(int(expected.display_rgba8[p * 4 + c]) -
                           int(actual.display_rgba8[p * 4 + c])) <= 1,
                  "resolved HDR display has one exposure and selected tone map");
    }
}
} // namespace
int main() {
    try {
        auto unique = take(d3d12::D3D12Device::create({{}, false, true}));
        auto device = std::shared_ptr<d3d12::D3D12Device>(std::move(unique));
        auto renderer = take(d3d12::AuthoringViewport::create_on_device(device));
        auto raw = take(d3d12::AuthoringViewport::create_on_device(device));
        auto f = fixture();
        auto source = std::make_shared<const project::ProjectDocument>(f.document);
        auto r = request(f);
        r.width = 65;
        r.height = 49;
        r.temporal.enabled = true;
        r.temporal.session = 1;
        production::AuthoringTemporalHistory reference;
        unsigned cases = 0;
        auto step = [&](const char *label, bool reset_reference = false) {
            if (reset_reference)
                reference.clear();
            auto current_request = r;
            current_request.temporal.enabled = false;
            const auto current = take(raw->render(source, current_request));
            auto expected = take(reference.resolve(*source, r, current.image, r.temporal.camera_cut,
                                                   r.temporal.settings));
            auto actual = take(renderer->render(source, r));
            compare_temporal(expected.image, actual.image);
            check(actual.temporal_resident_bytes >= std::uint64_t(r.width) * r.height * 104,
                  "bounded native history budget reported");
            check(actual.gpu_temporal_motion_ms >= 0 && actual.gpu_temporal_resolve_ms >= 0 &&
                      actual.gpu_temporal_history_ms >= 0,
                  "distinct producer resolve and GPU history-copy timings");
            std::cout << "native-temporal case=" << label
                      << " eligible=" << actual.temporal_history_eligible
                      << " reset=" << actual.temporal_reset
                      << " upload_bytes=" << actual.temporal_upload_bytes
                      << " motion_ms=" << actual.gpu_temporal_motion_ms
                      << " resolve_ms=" << actual.gpu_temporal_resolve_ms
                      << " history_ms=" << actual.gpu_temporal_history_ms << '\n';
            ++cases;
            return actual;
        };
        check(!step("cold").temporal_history_eligible, "initial history rejects");
        const auto warm = step("warm");
        check(warm.temporal_history_eligible && warm.temporal_upload_bytes == 1024,
              "second frame changes support palette only");
        check(step("persistent").temporal_upload_bytes == 0,
              "stable native palette residency avoids repeated upload");
        r.camera.reset();
        r.view = warm.image.view;
        r.view.position.x += .01;
        check(step("subpixel-camera").temporal_history_eligible, "small camera motion reprojects");
        r.transform_overrides[f.front] = {{.2, 0, 0}, {0, 0, 0, 1}, {-1.1, .9, 1}};
        check(step("mirrored-nonuniform-object").temporal_history_eligible,
              "affine object motion reprojects");
        r.transform_overrides.clear();
        step("undo-preview");
        r.exposure_ev = 1.5;
        r.tone_mode = render::ToneMapMode::neutral;
        check(step("display-only").temporal_history_eligible,
              "exposure and tone map stay after linear history");
        r.shadows.enabled = true;
        r.shadows.resolution = 64;
        check(!step("shadow-quality").temporal_history_eligible,
              "shadow quality change resets native history");
        r.transform_overrides[f.front] = {{.1, 0, 0}};
        check(!step("moving-shadow-caster").temporal_history_eligible,
              "unsupported shadow motion resets receiver history");
        r.transform_overrides[f.light] = {{}, {0, .05, 0, std::sqrt(1 - .05 * .05)}};
        check(!step("moving-light").temporal_history_eligible,
              "light direction changes reject history");
        r.transform_overrides.clear();
        r.shadows.enabled = false;
        step("restore-lighting");
        r.temporal.camera_cut = true;
        check(!step("cut").temporal_history_eligible, "explicit camera cut rejects");
        r.temporal.camera_cut = false;
        r.width = 63;
        step("resize");
        auto suspended = r;
        suspended.width = 0;
        static_cast<void>(take(renderer->render(source, suspended)));
        step("resume", true);
        r.temporal.session = 2;
        check(!step("reopen-session", true).temporal_history_eligible,
              "same source reopen rejects old session");
        mutate(f.document, [&](auto &tx) { okay(tx.set_object_transform(f.front, {{.1, 0, 0}})); });
        source = std::make_shared<const project::ProjectDocument>(f.document);
        check(!step("source-edit").temporal_history_eligible, "durable edit resets source history");
        auto bad = r;
        bad.temporal.settings.feedback = 2;
        check(!renderer->render(source, bad), "invalid temporal settings fail visibly");
        const auto recovered = step("recover-after-failure", true);
        // Runtime publication remains resident. This test alone adds a same-queue
        // copy consumer and waits its fence; it is not native UI visual acceptance.
        auto resident = take(renderer->render_resident(source, r, false));
        check(resident.resident_image && resident.deferred_timing &&
                  resident.image_transfer_bytes == 0 && resident.image.linear_hdr.empty(),
              "native temporal display stays resident without numeric readback");
        const auto pitch = (r.width * 4U + 255U) & ~255U;
        const auto byte_count = std::uint64_t(pitch) * r.height;
        const auto consumer_buffer = take(device->create_buffer(
            {byte_count, gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination}));
        device_ir::DeviceCommandStream consumer;
        consumer.append(
            device_ir::CmdTransitionResource{{consumer_buffer},
                                             device_ir::ResourceState::undefined,
                                             device_ir::ResourceState::copy_destination});
        consumer.append(device_ir::CmdTransitionResource{{resident.resident_image->texture},
                                                         device_ir::ResourceState::shader_read,
                                                         device_ir::ResourceState::copy_source});
        consumer.append(device_ir::CmdCopyTextureToBuffer{resident.resident_image->texture,
                                                          consumer_buffer, 0});
        consumer.append(device_ir::CmdTransitionResource{{resident.resident_image->texture},
                                                         device_ir::ResourceState::copy_source,
                                                         device_ir::ResourceState::shader_read});
        const auto consumer_serial = take(device->submit(consumer));
        okay(device->wait(consumer_serial));
        const auto consumed = take(device->read_buffer(consumer_buffer, 0, byte_count));
        for (unsigned y = 0; y < r.height; ++y)
            for (unsigned x = 0; x < r.width; ++x)
                for (unsigned c = 0; c < 4; ++c) {
                    const auto expected =
                        recovered.image.display_rgba8[(std::size_t(y) * r.width + x) * 4 + c];
                    const auto actual = consumed[std::size_t(y) * pitch + x * 4 + c];
                    check(std::abs(int(actual) - int(expected)) <= 1,
                          "same-queue consumer sees resolved native display pixels");
                }
        okay(device->destroy_buffer(consumer_buffer));
        auto timing = take(resident.deferred_timing->resolve());
        check(timing.temporal_motion_ms >= 0 && timing.temporal_resolve_ms >= 0 &&
                  timing.temporal_history_ms >= 0,
              "deferred timestamps include all temporal stages");
        renderer.reset();
        raw.reset();
        resident.resident_image.reset();
        resident.deferred_timing.reset();
        for (const auto &message : device->debug_receipts())
            std::cerr << message << '\n';
        check(device->debug_receipts().empty(),
              "native temporal lifetime transition descriptor debug validation clean");
        std::cout << "PASS " << cases
                  << " native temporal cases plus resident/deferred lifetime. Native window visual "
                     "checks remain separate.\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
