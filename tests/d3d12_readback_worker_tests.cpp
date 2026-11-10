#include "authoring_surface_fixture.hpp"
#include "../apps/desktop/studio_scene_camera.hpp"
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/d3d12/device.hpp>

#include <chrono>
#include <thread>
#include <string_view>
#include <iostream>
#include <memory>

int main(int argc, char** argv) {
    try {
        using namespace carto;
        if (argc > 1 && std::string_view(argv[1]) == "resident") {
            auto created = d3d12::D3D12Device::create({{}, false, false, std::chrono::seconds(5)});
            if (!created) { std::cout << "SKIP: D3D12 device unavailable\n"; return 77; }
            auto device = std::shared_ptr<d3d12::D3D12Device>(std::move(created.value()));
            check(!device->is_software_adapter(), "resident worker must use a hardware adapter");
            std::cout << "resident adapter=" << device->identity().adapter_name << "\n";
            auto f = fixture();
            auto second_source = f.document;
            const auto stable_source = f.document;
            auto req = request(f);
            req.temporal.enabled = true;
            req.temporal.session = 9;
            auto& worker = d3d12::ReadbackViewportWorker::process_owned();
            check(!worker.in_flight(), "resident worker starts idle");
            const auto run = [&](d3d12::ReadbackViewportIdentity id, project::ProjectDocument source) {
                const auto start = std::chrono::steady_clock::now();
                auto begun = worker.begin_resident(id, std::move(source), {}, req, device);
                check(begun && begun.value() && worker.in_flight(), "resident job must reserve its slot");
                while (!worker.device_requested()) {
                    auto status = worker.poll();
                    check(worker.in_flight(), status.error.empty() ? "preparation failed" : status.error.c_str());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                check(!worker.device_in_use(), "preparation must not take the shared device");
                static_cast<void>(device->completed_serial());
                const auto granted = std::chrono::steady_clock::now();
                check(worker.grant_device() && worker.device_in_use(), "device handoff must be explicit");
                d3d12::ReadbackViewportPoll status;
                do {
                    status = worker.poll();
                    if (worker.in_flight()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (worker.in_flight());
                const auto finished = std::chrono::steady_clock::now();
                std::cout << "resident sample=" << id.request_id
                          << " prepare_ms=" << std::chrono::duration<double, std::milli>(granted - start).count()
                          << " exclusive_ms=" << std::chrono::duration<double, std::milli>(finished - granted).count()
                          << " total_ms=" << std::chrono::duration<double, std::milli>(finished - start).count() << '\n';
                return status;
            };
            auto completed = run({9, 17}, std::move(f.document));
            check(completed.state == d3d12::ReadbackViewportPoll::State::ready && completed.frame,
                  completed.error.empty() ? "resident worker failed" : completed.error.c_str());
            auto& frame = *completed.frame;
            check(completed.identity == d3d12::ReadbackViewportIdentity{9, 17} && frame.resident_image &&
                  frame.resident_image->device == device && frame.resident_image->lifetime &&
                  frame.image.width == req.width && frame.image.height == req.height &&
                  frame.image.display_rgba8.empty() && frame.image_transfer_bytes == 0,
                  "resident result must retain the requested same-device image without readback");
            check(device->native_texture_handle(frame.resident_image->texture).has_value(),
                  "target must remain resident after producer retirement");
            const auto first_target = frame.target_generation;
            const auto first_texture = frame.resident_image->texture;
            auto prior_image = *frame.resident_image;
            check(!frame.temporal_history_eligible, "first temporal frame must start without history");
            okay(device->wait_idle());
            check(frame.deferred_timing && frame.deferred_timing->resolve(),
                  "producer timing must resolve after a confirmed consumer fence");
            completed.frame.reset();
            auto warm = run({9, 18}, std::move(second_source));
            check(warm.state == d3d12::ReadbackViewportPoll::State::ready && warm.frame &&
                  warm.frame->resident_image && warm.frame->target_generation == first_target &&
                  warm.frame->resident_image->texture == first_texture && warm.frame->temporal_history_eligible,
                  warm.error.empty() ? "warm request must reuse target and temporal history" : warm.error.c_str());
            okay(device->wait_idle());
            warm.frame.reset();
            for (std::uint64_t sample = 19; sample < 23; ++sample) {
                auto repeated = run({9, sample}, stable_source);
                check(repeated.state == d3d12::ReadbackViewportPoll::State::ready && repeated.frame &&
                      repeated.frame->resident_image && repeated.frame->target_generation == first_target &&
                      repeated.frame->resident_image->texture == first_texture && repeated.frame->temporal_history_eligible,
                      repeated.error.empty() ? "repeated request lost resident history" : repeated.error.c_str());
                okay(device->wait_idle());
                repeated.frame.reset();
            }
            req.width = 96;
            req.height = 72;
            auto resized = run({9, 23}, stable_source);
            check(resized.state == d3d12::ReadbackViewportPoll::State::ready && resized.frame &&
                  resized.frame->resident_image && resized.frame->target_generation > first_target &&
                  !resized.frame->temporal_history_eligible && device->native_texture_handle(first_texture),
                  "resize must reset history and retain the old target while its presentation lease exists");
            okay(device->wait_idle());
            prior_image.lifetime.reset();
            check(!device->native_texture_handle(first_texture),
                  "old target must retire after its last presentation lease is released");
            resized.frame.reset();
            editor::EditorCamera navigation;
            check(navigation.set_orientation(0.2, -0.15) && navigation.frame({}, 2.0, 4.0 / 3.0),
                  "editor navigation fixture must be valid");
            req.camera.reset();
            req.view = take(studio::authoring_view_from_editor_camera(navigation, 4.0 / 3.0));
            req.temporal.enabled = false;
            auto navigated = run({9, 24}, stable_source);
            check(navigated.state == d3d12::ReadbackViewportPoll::State::ready && navigated.frame &&
                  navigated.frame->resident_image, "navigation view must produce a resident image");
            const auto pitch = (req.width * 4U + 255U) & ~255U;
            const auto buffer = take(device->create_buffer({std::uint64_t(pitch) * req.height,
                gpu::MemoryClass::readback, gpu::BufferUsage::transfer_destination}));
            device_ir::DeviceCommandStream commands;
            commands.append(device_ir::CmdTransitionResource{{buffer}, device_ir::ResourceState::undefined,
                                                               device_ir::ResourceState::copy_destination});
            commands.append(device_ir::CmdTransitionResource{{navigated.frame->resident_image->texture},
                device_ir::ResourceState::shader_read, device_ir::ResourceState::copy_source});
            commands.append(device_ir::CmdCopyTextureToBuffer{navigated.frame->resident_image->texture, buffer, 0});
            commands.append(device_ir::CmdTransitionResource{{navigated.frame->resident_image->texture},
                device_ir::ResourceState::copy_source, device_ir::ResourceState::shader_read});
            okay(device->wait(take(device->submit(commands))));
            const auto pixels = take(device->read_buffer(buffer, 0, std::uint64_t(pitch) * req.height));
            const auto center = std::size_t(req.height / 2U) * pitch + std::size_t(req.width / 2U) * 4U;
            check(pixels[center] > pixels[center + 1U] && pixels[center] > pixels[center + 2U],
                  "editor-camera resident view must show the front red fixture at its target");
            okay(device->destroy_buffer(buffer));
            navigated.frame.reset();
            const auto before_cancel = device->last_submitted_serial();
            auto canceled = worker.begin_resident({9, 25}, stable_source, {}, req, device);
            check(canceled && canceled.value(), "pre-grant job must start");
            while (!worker.device_requested()) {
                auto status = worker.poll();
                check(worker.in_flight(), status.error.empty() ? "pre-grant preparation failed" : status.error.c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(!worker.device_in_use(), "waiting for a grant must not occupy the shared device");
            worker.invalidate();
            d3d12::ReadbackViewportPoll withdrawn;
            do {
                withdrawn = worker.poll();
                if (worker.in_flight()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (worker.in_flight());
            check(withdrawn.state == d3d12::ReadbackViewportPoll::State::stale &&
                  !withdrawn.frame && device->last_submitted_serial() == before_cancel,
                  "pre-grant cancellation must withdraw the job without submitting GPU work");
            auto next = fixture();
            auto next_request = request(next);
            auto next_started = worker.begin_resident({9, 26}, std::move(next.document), {}, next_request, device);
            check(next_started && next_started.value() && worker.in_flight(),
                  "cancellation job must reserve the worker before stopping");
            while (!worker.device_requested()) {
                auto status = worker.poll();
                check(worker.in_flight(), status.error.empty() ? "cancellation preparation failed" : status.error.c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            check(worker.grant_device() && worker.device_in_use(),
                  "cancellation must start after the exclusive handoff");
            worker.request_stop();
            while (!worker.shutdown_ready()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(!worker.in_flight(), "resident shutdown must retire its thread before returning the device");
            okay(device->wait_idle());
            std::cout << "same-device resident worker retired and retained its GPU image\n";
            return 0;
        }
        d3d12::detail::ReadbackCancellation cancellation;
        check(!cancellation.requested(), "new job must not start canceled");
        cancellation.request();
        cancellation.request();
        check(cancellation.requested(), "cancellation must be sticky and idempotent");
        check(d3d12::detail::valid_readback_rgba(1, 1, 4) &&
              !d3d12::detail::valid_readback_rgba(0, 1, 0) &&
              !d3d12::detail::valid_readback_rgba(1, 1, 3) &&
              !d3d12::detail::valid_readback_rgba(1, 1, 5) &&
              !d3d12::detail::valid_readback_rgba(1025, 1024, 1025U * 1024U * 4U),
              "readback identity requires bounded nonempty exact RGBA8 data");
        using Gate = d3d12::detail::ReadbackViewportGate;
        Gate gate;
        const d3d12::ReadbackViewportIdentity first{3, 10}, next{3, 11};
        check(gate.begin(first) == Gate::Admission::started && gate.publishable(first),
              "first identity must reserve the only slot");
        check(gate.begin(next) == Gate::Admission::busy && !gate.publishable(first) &&
              gate.occupied(), "busy newer request makes old result stale without releasing its slot");
        gate.retire(); // Simulates confirmed thread exit, never cancellation.
        check(!gate.occupied() && gate.begin(next) == Gate::Admission::started,
              "confirmed retirement permits the latest request");
        gate.retire(); // Simulates a thread-start failure before work begins.
        check(!gate.occupied() && gate.begin(first) == Gate::Admission::started,
              "start failure must release an unused reservation");
        gate.stop();
        check(gate.occupied() && !gate.publishable(first) &&
              gate.begin(next) == Gate::Admission::stopped,
              "stop cancels publication, not running ownership");
        gate.retire();
        check(!gate.occupied() && gate.stopping(), "shutdown remains terminal after retirement");
        const auto pixels_before = pixels(1, 1, 1, {{0, 0, 255, 255}});
        auto pixels_after = pixels(1, 1, 1, {{255, 0, 0, 255}});
        pixels_after.record.revision = pixels_before.record.revision.next();
        auto library = std::make_shared<production::AuthoringTextureLibrary>();
        okay(library->admit(pixels_before.record, pixels_before.encoded));
        auto document = fixture().document;
        production::AuthoringViewportRequest request;
        request.width = request.height = 1;
        request.textures = library;
        auto prepared = take(d3d12::prepare_readback_snapshot_request(document, {}, request));
        check(prepared.textures && prepared.textures.get() != library.get(),
              "prepared request must own an independent texture snapshot");
        okay(library->admit(pixels_after.record, pixels_after.encoded));
        check(bool(prepared.textures->prepare(pixels_before.record, assets::TextureSemantic::base_color,
                                              assets::ColorSpace::srgb)) &&
              !prepared.textures->prepare(pixels_after.record, assets::TextureSemantic::base_color,
                                           assets::ColorSpace::srgb),
              "source replacement must not mutate the prepared request");

        auto& worker = d3d12::ReadbackViewportWorker::process_owned();
        check(worker.shutdown_ready(), "idle worker must permit shutdown");
        worker.invalidate();
        check(worker.shutdown_ready(), "idle invalidation must not create ownership");
        check(worker.poll().state == d3d12::ReadbackViewportPoll::State::idle,
              "worker starts idle");
        worker.request_stop();
        check(worker.shutdown_ready(), "idle stop must not start a worker");
        check(!worker.begin({1, 1}, std::move(document), {}, std::move(request)),
              "stop must be terminal and must reject a new job");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
