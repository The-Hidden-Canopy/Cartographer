#include "authoring_environment_fixture.hpp"
#include "authoring_shadow_fixture.hpp"
#include "authoring_surface_fixture.hpp"
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/project/mesh_digest.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
void graphics_table_limits(d3d12::D3D12Device &device) {
    const std::string source =
        "float4 VSMain(uint v:SV_VertexID):SV_POSITION{return float4(0,0,0,1);}\n"
        "float4 PSMain():SV_TARGET{return float4(0,0,0,1);}";
    std::vector<gpu::ShaderHandle> shaders;
    for (auto [stage, entry] : {std::pair{gpu::ShaderStage::vertex, "VSMain"},
                                std::pair{gpu::ShaderStage::fragment, "PSMain"}})
        shaders.push_back(take(device.create_shader(take(d3d12::compile_hlsl(
            {"shadow.table.limit", stage, entry, "table_limit.hlsl", source, true, false})))));
    gpu::PipelineDesc desc;
    desc.debug_name = "shadow.table.limit";
    desc.shaders = shaders;
    desc.depth_test = desc.depth_write = false;
    desc.sampled_texture_count = 9;
    desc.push_constant_bytes = 216;
    for (unsigned i = 0; i < 9; ++i)
        desc.descriptor_bindings.push_back({0, i, gpu::ShaderStage::fragment});
    const auto pipeline = take(device.create_pipeline(desc));
    device_ir::DeviceCommandStream forbidden_graphics;
    forbidden_graphics.append(device_ir::CmdBindPipeline{pipeline});
    const auto compute_draw=device.submit(forbidden_graphics,gpu::QueueType::compute);
    check(!compute_draw && compute_draw.error().message.find("graphics queue")!=std::string::npos,
          "compute-only queue must still reject graphics commands");
    okay(device.destroy_pipeline(pipeline));
    auto missing_color=desc;missing_color.color_format=gpu::Format::unknown;
    check(!device.create_pipeline(missing_color),
          "stage-aware graphics validation still rejects an unknown color attachment");
    auto invalid_visibility=desc;invalid_visibility.descriptor_bindings[0].stages=gpu::ShaderStage::compute;
    check(!device.create_pipeline(invalid_visibility),
          "resolved graphics shader handles cannot acquire compute binding visibility");
    desc.push_constant_bytes = 220;
    const auto overflow = device.create_pipeline(desc);
    check(!overflow && overflow.error().message.find("64 DWORDs") != std::string::npos,
          "nine-table graphics pipelines still reject root cost above64 DWORDs");
    desc.push_constant_bytes = 0;
    desc.sampled_texture_count = 10;
    desc.descriptor_bindings.push_back({0, 9, gpu::ShaderStage::fragment});
    const auto too_many = device.create_pipeline(desc);
    check(!too_many && too_many.error().message.find("at most nine") != std::string::npos,
          "graphics table extension remains bounded to nine textures");
    for (auto shader : shaders)
        okay(device.destroy_shader(shader));
}
std::vector<std::uint8_t> resident_pixels(d3d12::D3D12Device &device,
                                          const d3d12::ResidentViewportImage &image,
                                          std::uint32_t width, std::uint32_t height) {
    const auto pitch = (width * 4U + 255U) & ~255U;
    const auto readback =
        take(device.create_buffer({std::uint64_t(pitch) * height, gpu::MemoryClass::readback,
                                   gpu::BufferUsage::transfer_destination}));
    device_ir::DeviceCommandStream commands;
    commands.append(device_ir::CmdTransitionResource{{readback},
                                                     device_ir::ResourceState::undefined,
                                                     device_ir::ResourceState::copy_destination});
    commands.append(device_ir::CmdTransitionResource{{image.texture},
                                                     device_ir::ResourceState::shader_read,
                                                     device_ir::ResourceState::copy_source});
    commands.append(device_ir::CmdCopyTextureToBuffer{image.texture, readback, 0});
    commands.append(device_ir::CmdTransitionResource{{image.texture},
                                                     device_ir::ResourceState::copy_source,
                                                     device_ir::ResourceState::shader_read});
    okay(device.wait(take(device.submit(commands))));
    const auto packed = take(device.read_buffer(readback, 0, std::uint64_t(pitch) * height));
    std::vector<std::uint8_t> pixels(std::size_t(width) * height * 4U);
    for (std::uint32_t row = 0; row < height; ++row)
        std::copy_n(packed.data() + std::size_t(row) * pitch, width * 4U,
                    pixels.data() + std::size_t(row) * width * 4U);
    okay(device.destroy_buffer(readback));
    return pixels;
}
void compare(const production::AuthoringViewportImage &cpu,
             const production::AuthoringViewportImage &gpu) {
    check(cpu.width == gpu.width && cpu.height == gpu.height, "GPU extent matches request");
    std::size_t compared = 0;
    for (std::size_t p = 0; p < cpu.objects.size(); ++p) {
        // Raster edge rules differ; compare shared covered samples, and check
        // global coverage separately so an empty/native-clear pass cannot pass.
        if (!cpu.objects[p] || cpu.objects[p] != gpu.objects[p])
            continue;
        check(cpu.faces[p] == gpu.faces[p], "GPU identity resolves authored faces");
        const auto a = cpu.linear_hdr[p], b = gpu.linear_hdr[p];
        for (auto [expected, actual] :
             {std::pair{a.x, b.x}, std::pair{a.y, b.y}, std::pair{a.z, b.z}})
            check(std::isfinite(actual) &&
                      std::abs(expected - actual) < 2e-4 + std::abs(expected) * 1e-3,
                  "GPU/CPU HDR BRDF parity");
        check(std::abs(cpu.depth[p] - gpu.depth[p]) < 0.003,
              "GPU depth reconstructs camera distance");
        for (std::size_t c = 0; c < 3; ++c)
            check(std::abs(int(cpu.display_rgba8[p * 4U + c]) -
                           int(gpu.display_rgba8[p * 4U + c])) <= 1,
                  "GPU display has one exposure and selected tone map");
        ++compared;
    }
    check(compared > 50U, "GPU must shade a substantial image region");
    const auto cpu_count =
        std::count_if(cpu.objects.begin(), cpu.objects.end(), [](auto id) { return bool(id); });
    const auto gpu_count =
        std::count_if(gpu.objects.begin(), gpu.objects.end(), [](auto id) { return bool(id); });
    check(std::abs(cpu_count - gpu_count) < long(cpu.width + cpu.height),
          "GPU coverage agrees beyond a center pixel");
}
void native_environment_cases(d3d12::AuthoringViewport &renderer,
                              const std::filesystem::path &output) {
    const auto e = asymmetric_environment();
    auto f = environment_fixture(e);
    auto r = request(f);
    r.environment_lighting = take(production::compile_environment_lighting(e));
    auto source = std::make_shared<const project::ProjectDocument>(f.document);
    const auto cpu = take(production::render_authoring_viewport(f.document, r));
    const auto cold = take(renderer.render(source, r));
    compare(cpu, cold.image);
    check(cold.environment_upload_bytes == 128 * 207 * 16 &&
              cold.environment_resident_bytes == 128 * 207 * 16,
          "native environment uploads one bounded, unexposed linear atlas");
    const auto warm = take(renderer.render(source, r));
    check(warm.environment_upload_bytes == 0 && warm.environment_compile_milliseconds == 0 &&
              warm.geometry_upload_bytes == 0 &&
              warm.image.display_rgba8 == cold.image.display_rgba8,
          "warm environment reuses immutable source, convolution and GPU atlas");
    auto polar = r;
    polar.camera.reset();
    polar.view = cpu.view;
    polar.view.position = {0, 3, 0};
    polar.view.rotation = {-std::sqrt(.5), 0, 0, std::sqrt(.5)};
    polar.transform_overrides[f.front] = {{}, polar.view.rotation};
    compare(take(production::render_authoring_viewport(f.document, polar)),
            take(renderer.render(source, polar)).image);
    auto rotated = f.document;
    auto catalog = rotated.presentation_catalog();
    auto turned = e;
    turned.rotation_radians = std::numbers::pi / 2;
    turned.revision = turned.revision.next();
    okay(catalog.set_environment(turned));
    mutate(rotated, [&](auto &tx) { okay(tx.set_presentation_catalog(catalog)); });
    const auto changed =
        take(renderer.render(std::make_shared<const project::ProjectDocument>(rotated), r));
    compare(take(production::render_authoring_viewport(rotated, r)), changed.image);
    check(changed.environment_upload_bytes == 0 && changed.environment_compile_milliseconds == 0 &&
              changed.image.display_rgba8 != cold.image.display_rgba8,
          "environment rotation changes native reflections without rebuilding atlas");
    auto stale = f.document;
    catalog = stale.presentation_catalog();
    auto changed_pixels = e;
    changed_pixels.linear_radiance[0].x += 1;
    okay(catalog.set_environment(changed_pixels));
    mutate(stale, [&](auto &tx) { okay(tx.set_presentation_catalog(catalog)); });
    check(!renderer.render(std::make_shared<const project::ProjectDocument>(stale), r),
          "stale derived environment cannot replace newer source pixels");
    check(take(renderer.render(source, r)).image.display_rgba8 == cold.image.display_rgba8,
          "failed environment replacement preserves prior good appearance");
    for (double metallic : {0., 1.})
        for (double roughness : {0., .3, 1.}) {
            auto edited = f.document;
            auto materials = edited.material_catalog();
            auto m = materials.materials().at(1);
            m.revision = m.revision.next();
            m.definition.metallic = metallic;
            m.definition.roughness = roughness;
            m.definition.clearcoat = .4;
            m.definition.clearcoat_roughness = 1 - roughness;
            okay(materials.replace_material(m));
            set_materials(edited, std::move(materials));
            const auto gpu =
                take(renderer.render(std::make_shared<const project::ProjectDocument>(edited), r));
            compare(take(production::render_authoring_viewport(edited, r)), gpu.image);
            check(gpu.environment_upload_bytes == 0, "material sweeps do not reupload environment");
        }
    auto normal_scene = f.document;
    mutate(normal_scene, [&](auto &tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle())); });
    const auto normal = pixels(82, 1, 1, {{180, 170, 230, 255}});
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    okay(library->admit(normal.record, normal.encoded));
    auto materials = normal_scene.material_catalog();
    okay(materials.insert_texture(normal.record));
    auto m = materials.materials().at(1);
    m.revision = m.revision.next();
    m.definition.normal = {82, 9};
    m.definition.occlusion = {82, 9};
    m.definition.metallic = 0;
    okay(materials.replace_material(m));
    set_materials(normal_scene, std::move(materials));
    auto mapped = r;
    mapped.textures = library;
    mapped.transform_overrides[f.front] = {{}, {}, {-1.2, .7, 1}};
    compare(take(production::render_authoring_viewport(normal_scene, mapped)),
            take(renderer.render(std::make_shared<const project::ProjectDocument>(normal_scene),
                                 mapped))
                .image);
    auto reopened = std::make_shared<const project::ProjectDocument>(
        take(project::ProjectDocument::deserialize(f.document.serialize())));
    check(take(renderer.render(reopened, r)).image.display_rgba8 == cold.image.display_rgba8,
          "native environment appearance survives authored source reopen");
    const auto resident = take(renderer.render_resident(source, r, false));
    check(resident.resident_image && resident.image_transfer_bytes == 0 &&
              resident.image.linear_hdr.empty() && resident.image.depth.empty(),
          "environment shading uses the resident display route without frame transfer");
    check(resident_pixels(*resident.resident_image->device, *resident.resident_image, r.width,
                          r.height) == cold.image.display_rgba8,
          "resident environment pixels match numeric reference under on-demand readback");
    check(take(resident.deferred_timing->resolve()).envelope_ms >= 0,
          "environment deferred GPU timing resolves after consumer fence");
    if (!output.empty()) {
        std::filesystem::create_directories(output);
        static_cast<void>(take(f.document.save_atomic(output / "environment-gpu.carto")));
        std::ofstream image(output / "environment-gpu.ppm", std::ios::binary);
        image << "P6\n" << r.width << ' ' << r.height << "\n255\n";
        for (std::size_t p = 0; p < cold.image.objects.size(); ++p)
            image.write(reinterpret_cast<const char *>(&cold.image.display_rgba8[p * 4]), 3);
    }
    std::cout << "native environment parity, rotation, roughness/coat, mapped normals/AO, warm "
                 "residency, failure/reopen and resident display passed\n";
}
void native_shadow_cases(d3d12::AuthoringViewport &renderer, const std::filesystem::path &output) {
    auto f = shadow_fixture();
    auto r = shadow_request(f);
    r.width=r.height=192; // enough receiver interior beyond both raster/PCF edge bands
    auto source = std::make_shared<const project::ProjectDocument>(f.document);
    auto baseline = r;
    baseline.shadows.enabled = false;
    const auto direct = take(production::render_authoring_viewport(f.document, baseline));
    const auto cpu = take(production::render_authoring_viewport(f.document, r));
    const auto gpu = take(renderer.render(source, r));
    check(gpu.shadow_draw_calls == 2 &&
              gpu.shadow_resident_bytes == std::uint64_t(128) * 128 * 8 + 128,
          "first shadow frame rasterizes authored casters within its resource budget");
    unsigned dark = 0, lit = 0;
    unsigned raw_lit=0,raw_dark=0,receivers=0;
    for(std::size_t p=0;p<cpu.objects.size();++p) if(cpu.objects[p]==f.back && direct.objects[p]==f.back) {
        ++receivers;raw_lit+=std::abs(cpu.linear_hdr[p].x-direct.linear_hdr[p].x)<1e-9;
        raw_dark+=cpu.linear_hdr[p].x<direct.linear_hdr[p].x*.01;
        if(receivers==1) std::cout<<"shadow first receiver direct="<<direct.linear_hdr[p].x<<" cpu="<<cpu.linear_hdr[p].x<<" gpu="<<gpu.image.linear_hdr[p].x<<'\n';
    }
    std::cout<<"shadow receiver counts: "<<receivers<<" raw_lit="<<raw_lit<<" raw_dark="<<raw_dark<<'\n';
    const auto stable = [&](unsigned x, unsigned y, bool shadow) {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const auto p = std::size_t(int(y) + dy) * r.width + unsigned(int(x) + dx);
                if (cpu.objects[p] != f.back || direct.objects[p] != f.back)
                    return false;
                if (shadow ? cpu.linear_hdr[p].x > direct.linear_hdr[p].x * .01
                           : std::abs(cpu.linear_hdr[p].x - direct.linear_hdr[p].x) > 1e-9)
                    return false;
            }
        return true;
    };
    for (unsigned y = 1; y + 1 < r.height; ++y)
        for (unsigned x = 1; x + 1 < r.width; ++x) {
            const auto p = std::size_t(y) * r.width + x;
            if (stable(x, y, true)) {
                check(gpu.image.objects[p] == f.back && gpu.image.linear_hdr[p].x < .002,
                      "native spatial shadow darkens the receiver interior");
                ++dark;
            } else if (stable(x, y, false)) {
                check(gpu.image.objects[p] == f.back &&
                          std::abs(gpu.image.linear_hdr[p].x - direct.linear_hdr[p].x) < .002,
                      "native unoccluded receiver remains lit");
                ++lit;
            }
        }
    std::cout<<"shadow stable receiver samples: dark="<<dark<<" lit="<<lit<<'\n';
    check(dark > 5 && lit > 300, "shadow test covers spatial dark and lit receiver regions");
    const auto warm = take(renderer.render(source, r));
    check(warm.shadow_draw_calls == 0 && warm.lighting_upload_bytes == 0 &&
              warm.geometry_upload_bytes == 0 && warm.material_upload_bytes == 0 &&
              warm.image.display_rgba8 == gpu.image.display_rgba8,
          "unchanged lighting reuses map, packets and geometry with identical appearance");
    auto orbit = r;
    orbit.camera.reset();
    orbit.view = cpu.view;
    orbit.view.position.x += .1;
    check(take(renderer.render(source, orbit)).shadow_draw_calls == 0,
          "camera-only movement reuses the fitted shadow map");
    auto resize=r;resize.width=80;resize.height=64;
    const auto resized=take(renderer.render(source,resize));
    check(resized.shadow_draw_calls==0 && resized.image.width==80 && resized.image.height==64,
          "viewport resize reuses camera-independent shadow resources");
    auto moving = r;
    moving.transform_overrides[f.front] = {{2, 0, 0}, {}, {.35, .35, 1}};
    const auto moved = take(renderer.render(source, moving));
    check(moved.shadow_draw_calls == 2 && moved.image.display_rgba8 != gpu.image.display_rgba8,
          "moving occluder invalidates native shadow lighting");
    auto light_preview=r;
    light_preview.transform_overrides[f.light]={{},{0,-std::sin(.3),0,std::cos(.3)}};
    const auto rotated=take(renderer.render(source,light_preview));
    check(rotated.shadow_draw_calls==2 && rotated.image.display_rgba8!=gpu.image.display_rgba8,
          "transient light rotation invalidates native shadow lighting");
    auto masked = f;
    auto mask_request = shadow_request(masked);
    mask_request.width=r.width;mask_request.height=r.height;
    mask_request.textures = mask_shadow_caster(masked);
    auto mask_source = std::make_shared<const project::ProjectDocument>(masked.document);
    const auto cutout_cpu =
        take(production::render_authoring_viewport(masked.document, mask_request));
    const auto cutout_gpu = take(renderer.render(mask_source, mask_request));
    unsigned holes = 0;
    for (std::size_t p = 0; p < cpu.objects.size(); ++p)
        if (cpu.objects[p] == f.back && cutout_cpu.objects[p] == f.back &&
            cutout_cpu.linear_hdr[p].x > cpu.linear_hdr[p].x + .15 &&
            cutout_gpu.image.objects[p] == f.back &&
            cutout_gpu.image.linear_hdr[p].x > gpu.image.linear_hdr[p].x + .1)
            ++holes;
    check(holes > 3, "native decoded alpha mask transmits spatial light through caster holes");
    auto reopened = std::make_shared<const project::ProjectDocument>(
        take(project::ProjectDocument::deserialize(masked.document.serialize())));
    check(take(renderer.render(reopened, mask_request)).image.display_rgba8 ==
              cutout_gpu.image.display_rgba8,
          "native authored shadow appearance survives reopen");
    if (!output.empty()) {
        std::filesystem::create_directories(output);
        static_cast<void>(take(f.document.save_atomic(output / "native-shadow.carto")));
        std::ofstream file(output / "native-shadow.ppm", std::ios::binary);
        file << "P6\n" << r.width << ' ' << r.height << "\n255\n";
        for (std::size_t p = 0; p < gpu.image.objects.size(); ++p)
            file.write(reinterpret_cast<const char *>(&gpu.image.display_rgba8[p * 4]), 3);
        std::ofstream timing(output/"shadow-frame-timing.csv");
        timing<<"enabled,sample,width,height,caller_wall_ms,prepare_ms,geometry_ms,material_ms,lighting_ms,metadata_wait_ms,metadata_gpu_ms,shadow_gpu_ms,raster_gpu_ms,display_gpu_ms,readback_gpu_ms,gpu_envelope_ms,geometry_upload_bytes,material_upload_bytes,metadata_upload_bytes,shadow_draws,resident_bytes,readback_bytes,prepared,resources_ready,geometry_ready,materials_ready,commands_ready,submitted,fence_complete,timestamps_read,readback_ready,outputs_allocated,resolved,returning\n";
        timing<<std::setprecision(10);
        // Same scene/camera/extent; this is feature cost, not an optimization
        // speedup control. Numeric readback is explicit; no UI time is claimed.
        for(bool enabled:{false,true}) {
            auto benchmark=r;benchmark.shadows.enabled=enabled;
            std::vector<std::uint8_t> expected;
            for(unsigned sample=0;sample<35;++sample) {
                const auto began=std::chrono::steady_clock::now();
                const auto frame=take(renderer.render(source,benchmark));
                const auto wall=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
                if(sample==0) expected=frame.image.display_rgba8;
                check(expected==frame.image.display_rgba8,"fixed shadow performance workload remains numerically stable");
                if(sample>=5) check(frame.geometry_upload_bytes==0 && frame.material_upload_bytes==0 &&
                    frame.lighting_upload_bytes==0 && frame.shadow_draw_calls==0,
                    "warm shadow measurements reuse all source uploads and cached map");
                const auto& h=frame.host_timeline;
                timing<<enabled<<','<<sample<<','<<r.width<<','<<r.height<<','<<wall<<','<<frame.cpu_prepare_milliseconds
                    <<','<<frame.geometry_update_milliseconds<<','<<frame.material_update_milliseconds
                    <<','<<frame.lighting_update_milliseconds<<','<<frame.lighting_upload_wait_milliseconds
                    <<','<<frame.lighting_gpu_upload_milliseconds<<','<<frame.gpu_shadow_milliseconds
                    <<','<<frame.gpu_milliseconds<<','<<frame.gpu_display_milliseconds
                    <<','<<frame.gpu_transfer_milliseconds<<','<<frame.gpu_all_stages_milliseconds
                    <<','<<frame.geometry_upload_bytes<<','<<frame.material_upload_bytes<<','<<frame.lighting_upload_bytes
                    <<','<<frame.shadow_draw_calls<<','<<frame.logical_resident_bytes<<','<<frame.readback_bytes
                    <<','<<h.prepared<<','<<h.resources_ready<<','<<h.geometry_ready<<','<<h.materials_ready
                    <<','<<h.commands_ready<<','<<h.submitted<<','<<h.fence_complete<<','<<h.timestamps_read
                    <<','<<h.readback_ready<<','<<h.outputs_allocated<<','<<h.resolved<<','<<h.returning<<'\n';
            }
        }
    }
    std::cout << "native directional shadow spatial coverage, masked transmission, warm reuse, "
                 "camera/motion and reopen passed\n";
}
void native_surface_cases(d3d12::AuthoringViewport &renderer, const std::filesystem::path &output) {
    auto f = fixture();
    mutate(f.document, [&](auto &tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle())); });
    auto color = pixels(
        71, 2, 2, {{128, 40, 80, 0}, {240, 128, 32, 255}, {30, 80, 200, 0}, {200, 180, 40, 255}});
    auto normal = pixels(72, 1, 1, {{170, 190, 230, 255}});
    auto data = pixels(73, 1, 1, {{160, 210, 80, 255}});
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    for (const auto *source : {&color, &normal, &data})
        okay(library->admit(source->record, source->encoded));
    auto c = f.document.material_catalog();
    for (const auto *source : {&color, &normal, &data})
        okay(c.insert_texture(source->record));
    project::SamplerIntent nearest;
    nearest.id = 44;
    nearest.name = "Independent nearest clamp";
    nearest.minification = nearest.magnification = nearest.mip = project::SamplerFilter::nearest;
    nearest.address_u = nearest.address_v = project::SamplerAddressMode::clamp_to_edge;
    okay(c.insert_sampler(nearest));
    auto material = c.materials().at(1);
    material.revision = material.revision.next();
    auto &m = material.definition;
    m.base_color = {71, 9};
    m.normal = {72, 9, 44};
    m.normal.normal_convention = render::NormalConvention::opengl_y_plus;
    m.metallic_roughness = {73, 9, 44};
    m.occlusion = {73, 9};
    m.alpha_mode = render::AlphaMode::masked;
    okay(c.replace_material(material));
    auto graph = authoring_graph_fixture();
    graph.identity = "materials/native/textured";
    project::MaterialGraphNode u;
    u.id = 15;
    u.kind = project::MaterialGraphNodeKind::parameter_scalar;
    u.parameter_name = "uv.u";
    auto v = u;
    v.id = 16;
    v.parameter_name = "uv.v";
    project::MaterialGraphNode t;
    t.id = 17;
    t.kind = project::MaterialGraphNodeKind::texture_sample;
    t.value_type = project::MaterialGraphValueType::color;
    t.texture_asset = 71;
    t.uv_set = 9;
    t.inputs = {15, 16};
    graph.nodes.push_back(u);
    graph.nodes.push_back(v);
    graph.nodes.push_back(t);
    graph.nodes[3].inputs = {17, 2, 3};
    graph.outputs.roughness = 15;
    okay(c.insert_material_graph(graph));
    okay(c.bind_material_graph(1, graph.identity));
    set_materials(f.document, c);
    auto presentation = f.document.presentation_catalog();
    auto camera = presentation.cameras().at(f.camera);
    camera.revision = camera.revision.next();
    camera.exposure_ev = -3;
    okay(presentation.replace_camera(camera));
    mutate(f.document, [&](auto &tx) { okay(tx.set_presentation_catalog(presentation)); });
    auto r = request(f);
    r.textures = library;
    r.camera_display = true;
    const auto cpu = take(production::render_authoring_viewport(f.document, r));
    auto source = std::make_shared<const project::ProjectDocument>(f.document);
    const auto cold = take(renderer.render(source, r));
    compare(cpu, cold.image);
    check(cold.material_upload_bytes > 0 && cold.material_resident_bytes > 0,
          "native first surface frame uploads admitted mips and program");
    const auto warm = take(renderer.render(source, r));
    compare(cpu, warm.image);
    check(warm.material_upload_bytes == 0 && warm.geometry_upload_bytes == 0,
          "unchanged surface frame uploads no geometry, pixels or program");
    check(warm.image.objects[40 * 64 + 26] == f.back && warm.image.objects[40 * 64 + 38] == f.front,
          "native masked texture preserves rear depth and picking");
    if (!output.empty()) {
        std::filesystem::create_directories(output);
        static_cast<void>(take(source->save_atomic(output / "native-surface.carto")));
        assets::BlobStore store(output / ".carto-assets" / "source-blobs");
        for (const auto *pixels : {&color, &normal, &data})
            static_cast<void>(take(store.put(pixels->encoded, "image/x-tga")));
        std::ofstream capture(output / "native-surface.ppm", std::ios::binary);
        capture << "P6\n64 64\n255\n";
        for (std::size_t pixel = 0; pixel < warm.image.objects.size(); ++pixel)
            capture.write(reinterpret_cast<const char *>(&warm.image.display_rgba8[pixel * 4]), 3);
    }
    auto bad = r;
    bad.textures = std::make_shared<production::AuthoringTextureLibrary>();
    check(!renderer.render(source, bad),
          "missing replacement pixels fail before native publication");
    check(take(renderer.render(source, r)).image.display_rgba8 == warm.image.display_rgba8,
          "failed source replacement preserves prior good native resources");
    for (double mirror : {1., -1.}) {
        r.transform_overrides[f.front] = {
            {}, {0, std::sin(.15), 0, std::cos(.15)}, {mirror * 1.5, .7, 1}};
        compare(take(production::render_authoring_viewport(f.document, r)),
                take(renderer.render(source, r)).image);
    }
    r.transform_overrides.clear();
    graph.revision = graph.revision.next();
    graph.nodes[1].constants[0] = .8;
    okay(c.replace_material_graph(graph));
    set_materials(f.document, c);
    check(!renderer.render(f.document, r),
          "native texture graph edit leaves stale binding visibly rejected");
    okay(c.bind_material_graph(1, graph.identity));
    set_materials(f.document, c);
    const auto edited = take(renderer.render(f.document, r));
    compare(take(production::render_authoring_viewport(f.document, r)), edited.image);
    check(edited.material_upload_bytes > 0 && edited.material_upload_bytes <= 6656,
          "graph edit replaces bounded program without reuploading unchanged image mips");
    check(edited.image.display_rgba8 != warm.image.display_rgba8,
          "native per-fragment graph edit changes appearance");
    auto reopened = take(project::ProjectDocument::deserialize(f.document.serialize()));
    check(take(renderer.render(reopened, r)).image.display_rgba8 == edited.image.display_rgba8,
          "native surface appearance survives source reopen");
    material = c.materials().at(1);
    material.revision = material.revision.next();
    material.definition.base_color.transform.scale = {64, 64};
    material.definition.normal.normal_convention = render::NormalConvention::directx_y_minus;
    okay(c.replace_material(material));
    set_materials(f.document, c);
    compare(take(production::render_authoring_viewport(f.document, r)),
            take(renderer.render(f.document, r)).image);
    r.width = 96;
    r.height = 48;
    compare(take(production::render_authoring_viewport(f.document, r)),
            take(renderer.render(f.document, r)).image);
    graph.revision = graph.revision.next();
    graph.nodes[2].constants[0] = 2;
    graph.outputs.roughness = 3;
    okay(c.replace_material_graph(graph));
    okay(c.bind_material_graph(1, graph.identity));
    set_materials(f.document, c);
    check(!production::render_authoring_viewport(f.document, r),
          "CPU reference rejects invalid dynamic graph outputs");
    const auto invalid = renderer.render(f.document, r);
    check(!invalid && invalid.error().message.find("invalid numeric values") != std::string::npos,
          "GPU numeric graph fault fails readback instead of silently clamping");
    r.width = 0;
    check(take(renderer.render(f.document, r)).image.objects.empty(),
          "native surface zero extent suspends");
    std::cout << "native surface cases: HDR/color/data/mips, independent samplers, normal "
                 "conventions/transforms, masked IDs, warm residency, failed replacement, graph "
                 "edit/rebind/reopen and resize passed\n";
}
} // namespace
int capture_authored_project(const std::filesystem::path &project_path,
                             const std::filesystem::path &output) {
    const auto source = std::make_shared<const project::ProjectDocument>(
        take(project::ProjectDocument::load(project_path)));
    auto library = std::make_shared<production::AuthoringTextureLibrary>();
    check(source->material_catalog().textures().size() <= 32,
          "authored capture source texture bound");
    assets::BlobStore store(project_path.parent_path() / ".carto-assets" / "source-blobs");
    for (const auto &[id, record] : source->material_catalog().textures()) {
        static_cast<void>(id);
        okay(library->admit(record, take(store.read(record.content_digest))));
    }
    production::AuthoringViewportRequest request;
    request.width = 640;
    request.height = 480;
    request.camera_display = true;
    request.textures = library;
    for (const auto &[id, camera] : source->presentation_catalog().cameras())
        if (camera.active_for_review)
            request.camera = id;
    check(request.camera.has_value(), "authored capture requires a saved active review camera");
    auto device =
        std::shared_ptr<d3d12::D3D12Device>(take(d3d12::D3D12Device::create({{}, false, true})));
    auto renderer = take(d3d12::AuthoringViewport::create_on_device(device));
    std::filesystem::create_directories(output);
    const auto save = [&](const char *name, const production::AuthoringViewportImage &image) {
        std::ofstream file(output / name, std::ios::binary);
        file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (std::size_t p = 0; p < image.objects.size(); ++p)
            file.write(reinterpret_cast<const char *>(&image.display_rgba8[p * 4]), 3);
    };
    const auto first = take(renderer->render(source, request));
    save("authored-project-gpu.ppm", first.image);
    std::set<scene::ObjectId> covered;
    double maximum = 0;
    for (std::size_t p = 0; p < first.image.objects.size(); ++p)
        if (first.image.objects[p]) {
            covered.insert(first.image.objects[p]);
            const auto c = first.image.linear_hdr[p];
            check(c.finite(), "authored GPU capture has finite linear HDR");
            maximum = std::max({maximum, c.x, c.y, c.z});
        }
    check(covered.size() >= 7 && maximum > 1,
          "authored GPU capture must rasterize substantial multi-object HDR scene");
    std::ofstream timing(output / "authored-project-timing.csv");
    timing << "sample,caller_ms,raster_gpu_ms,shadow_gpu_ms,display_gpu_ms,readback_gpu_ms,gpu_"
              "envelope_ms,prepare_ms,geometry_ms,material_ms,environment_ms,lighting_ms,geometry_"
              "upload_bytes,material_upload_bytes,environment_upload_bytes,metadata_upload_bytes,"
              "readback_bytes,shadow_draws\n"
           << std::setprecision(10);
    for (unsigned sample = 0; sample < 35; ++sample) {
        const auto began = std::chrono::steady_clock::now();
        const auto frame = take(renderer->render(source, request));
        const auto wall =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began)
                .count();
        check(frame.image.display_rgba8 == first.image.display_rgba8,
              "authored GPU workload remains stable");
        check(frame.geometry_upload_bytes == 0 && frame.material_upload_bytes == 0 &&
                  frame.environment_upload_bytes == 0 && frame.lighting_upload_bytes == 0 &&
                  frame.shadow_draw_calls == 0,
              "authored warm capture remains resident");
        timing << sample << ',' << wall << ',' << frame.gpu_milliseconds << ','
               << frame.gpu_shadow_milliseconds << ',' << frame.gpu_display_milliseconds << ','
               << frame.gpu_transfer_milliseconds << ',' << frame.gpu_all_stages_milliseconds << ','
               << frame.cpu_prepare_milliseconds << ',' << frame.geometry_update_milliseconds << ','
               << frame.material_update_milliseconds << ','
               << frame.environment_prepare_milliseconds << ','
               << frame.lighting_update_milliseconds << ',' << frame.geometry_upload_bytes << ','
               << frame.material_upload_bytes << ',' << frame.environment_upload_bytes << ','
               << frame.lighting_upload_bytes << ',' << frame.readback_bytes << ','
               << frame.shadow_draw_calls << '\n';
    }
    const auto resident = take(renderer->render_resident(source, request, false));
    check(resident.resident_image && resident.image_transfer_bytes == 0 &&
              resident.image.linear_hdr.empty(),
          "authored environment scene uses actual GPU-resident presentation output");
    check(resident_pixels(*device, *resident.resident_image, request.width, request.height) ==
              first.image.display_rgba8,
          "on-demand full resident capture matches numeric output");
    check(take(resident.deferred_timing->resolve()).envelope_ms >= 0,
          "authored resident timing resolves after consumer fence");
    for (const auto &object : source->scene().objects_sorted())
        if (object.name == "Beveled box") {
            auto moved = take(source->scene().world_transform(object.id));
            moved.translation.x += .8;
            auto motion = request;
            motion.transform_overrides[object.id] = moved;
            const auto frame = take(renderer->render(source, motion));
            check(frame.shadow_draw_calls > 0 && frame.environment_upload_bytes == 0 &&
                      frame.image.display_rgba8 != first.image.display_rgba8,
                  "authored moving occluder updates spatial shadows without rebaking environment");
            save("authored-project-gpu-motion.ppm", frame.image);
        }
    request.width = 800;
    request.height = 600;
    const auto resized = take(renderer->render(source, request));
    check(resized.target_generation > first.target_generation &&
              resized.environment_upload_bytes == 0,
          "authored scene resize safely replaces targets and reuses environment");
    save("authored-project-gpu-resized.ppm", resized.image);
    for (const auto &message : device->debug_receipts())
        std::cerr << message << '\n';
    check(device->debug_receipts().empty(), "authored project debug/lifetime validation is clean");
    std::cout << "Authored GPU capture passed: " << covered.size() << " visible objects, max HDR "
              << maximum
              << "; numeric readback timing excludes native UI; native visual acceptance remains "
                 "separate\n";
    return 0;
}
int main(int argc, char **argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--project") {
            check(argc == 4, "--project requires project path and output directory");
            return capture_authored_project(argv[2], argv[3]);
        }
        {
            const auto with_normal = [](core::Vec3d n) {
                auto mesh = triangle();
                auto payload = mesh.attributes();
                okay(payload.set_domain_count(attributes::AttributeDomain::corner, 3U));
                okay(
                    payload.add_layer({{"cartographer.normal0", attributes::AttributeDomain::corner,
                                        attributes::AttributeType::vec3},
                                       {n, n, n}}));
                okay(mesh.set_attribute_payload(std::move(payload), {}));
                return mesh;
            };
            const auto a = with_normal({.6, 0, .8}), b = with_normal({0, 0, 1});
            check(a.revision() == b.revision() && project::canonical_mesh_source_digest(a) !=
                                                      project::canonical_mesh_source_digest(b),
                  "mesh cache fingerprint binds authored corner data even when revisions collide");
        }
        // Enable the process-global debug layer before creating any device;
        // enabling it after a live device exists can remove that device.
        auto test_device = std::shared_ptr<d3d12::D3D12Device>(
            take(d3d12::D3D12Device::create({{}, false, true})));
        graphics_table_limits(*test_device);
        auto renderer = take(d3d12::AuthoringViewport::create_on_device(test_device));
        {
            auto lighting = fixture();
            auto light = lighting.document.presentation_catalog().lights().at(lighting.light);
            const auto baseline = take(renderer->render(lighting.document, request(lighting))).image;
            check(baseline.linear_hdr[center(baseline)].x > 0.0,
                  "steady GPU source illuminates the test fixture");
            scene::ObjectId dark_object;
            mutate(lighting.document, [&](auto &transaction) {
                dark_object = take(transaction.create_object("Dark source"));
            });
            auto dark = light;
            dark.object = dark_object; dark.identity = "viewport.dark";
            dark.failure_state = project::LightFailureState::dark;
            dark.color_temperature_kelvin = 2700.0;
            set_fixture_lights(lighting, {light, dark});
            auto with_override = request(lighting);
            core::Transform changed; changed.rotation = {0, 1, 0, 0};
            with_override.transform_overrides[dark_object] = changed;
            const auto unchanged = take(renderer->render(lighting.document, with_override)).image;
            check(std::abs(unchanged.linear_hdr[center(unchanged)].x -
                           baseline.linear_hdr[center(baseline)].x) < 1e-4,
                  "dark light and its transient transform cannot replace the active GPU light");
            set_fixture_lights(lighting, {dark});
            const auto only_dark = take(renderer->render(lighting.document, request(lighting))).image;
            check(only_dark.objects[center(only_dark)] &&
                  std::abs(only_dark.linear_hdr[center(only_dark)].x) < 1e-6,
                  "known-dark GPU source emits no light");
            light.normal_state = project::LightNormalState::switchable;
            set_fixture_lights(lighting, {light});
            check(!renderer->render(lighting.document, request(lighting)),
                  "GPU viewport rejects an ambiguous light state");
            light.normal_state = project::LightNormalState::steady;
            light.color_temperature_kelvin = 2700.0;
            set_fixture_lights(lighting, {light});
            check(!renderer->render(lighting.document, request(lighting)),
                  "GPU viewport rejects active color-temperature intent");
        }
        native_surface_cases(*renderer,
                             argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        native_shadow_cases(*renderer,
                            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        native_environment_cases(*renderer, argc > 1 ? std::filesystem::path(argv[1])
                                                     : std::filesystem::path{});
        {
            const auto empty = take(project::ProjectDocument::create("Empty GPU viewport"));
            production::AuthoringViewportRequest er;
            er.width = 64;
            er.height = 32;
            const auto frame = take(renderer->render(empty, er));
            check(std::all_of(frame.image.objects.begin(), frame.image.objects.end(),
                              [](auto id) { return !id; }) &&
                      std::all_of(frame.image.display_rgba8.begin(),
                                  frame.image.display_rgba8.end(),
                                  [](auto byte) { return byte == 0; }),
                  "empty scene uses a vertex-ID fullscreen display pass and keeps "
                  "transparent coverage");
        }
        auto f = fixture();
        {
            auto legacy = f.document;
            auto mesh = legacy.meshes().at(f.front_mesh);
            auto attributes = mesh.attributes();
            okay(attributes.remove_layer("cartographer.material_slot"));
            okay(attributes.add_layer({{"condition.material_region", attributes::AttributeDomain::face,
                                         attributes::AttributeType::uint32}, {std::uint32_t{1U}}}));
            okay(mesh.set_attribute_payload(std::move(attributes), mesh.uv_sets()));
            mutate(legacy, [&](auto& transaction) { okay(transaction.replace_mesh(f.front_mesh, std::move(mesh))); });
            const auto legacy_request = request(f);
            compare(take(production::render_authoring_viewport(legacy, legacy_request)),
                    take(renderer->render(legacy, legacy_request)).image);
            check(take(renderer->render(legacy, legacy_request)).image.display_rgba8 ==
                      take(renderer->render(f.document, legacy_request)).image.display_rgba8,
                  "legacy region binding reaches GPU shading without a default-material substitution");
        }
        auto r = request(f);
        // GPU execution of these new cases remains a separate coordinated gate;
        // the CPU authoring_material suite does not instantiate this
        // executable.
        {
            auto graphed = fixture();
            auto graph = authoring_graph_fixture();
            auto catalog = graphed.document.material_catalog();
            okay(catalog.insert_material_graph(graph));
            okay(catalog.bind_material_graph(1, graph.identity));
            set_materials(graphed.document, catalog);
            auto gr = request(graphed);
            gr.exposure_ev = -4;
            const auto rendered = take(renderer->render(graphed.document, gr));
            compare(take(production::render_authoring_viewport(graphed.document, gr)),
                    rendered.image);
            graph.revision = graph.revision.next();
            graph.nodes[1].constants = {.2, .7, .9, 1};
            okay(catalog.replace_material_graph(graph));
            set_materials(graphed.document, catalog);
            check(!renderer->render(graphed.document, gr),
                  "GPU preparation rejects a stale graph binding");
            okay(catalog.bind_material_graph(1, graph.identity));
            set_materials(graphed.document, catalog);
            auto changed = take(renderer->render(graphed.document, gr));
            compare(take(production::render_authoring_viewport(graphed.document, gr)),
                    changed.image);
            check(changed.image.display_rgba8 != rendered.image.display_rgba8,
                  "rebound constant graph reaches GPU material shading");
            const auto reopened =
                take(project::ProjectDocument::deserialize(graphed.document.serialize()));
            check(take(renderer->render(reopened, gr)).image.display_rgba8 ==
                      changed.image.display_rgba8,
                  "constant graph GPU appearance survives source reopen");
        }
        {
            auto async_renderer = take(d3d12::AuthoringViewport::create_on_device(test_device));
            const auto source = std::make_shared<const project::ProjectDocument>(f.document);
            std::vector<d3d12::AuthoringGpuFrame> pending;
            pending.push_back(take(async_renderer->render_resident(source, r, false)));
            check(pending.front().deferred_timing && pending.front().fence_wait_milliseconds == 0 &&
                      pending.front().host_timeline.fence_complete == -1 &&
                      pending.front().image_transfer_bytes == 0,
                  "asynchronous resident producer does not wait or claim completed GPU timing");
            const auto pixels =
                resident_pixels(*test_device, *pending.front().resident_image, r.width, r.height);
            const auto receipt = take(pending.front().deferred_timing->resolve());
            check(receipt.envelope_ms >= 0 && receipt.end_host_ms >= receipt.begin_host_ms,
                  "consumer completion permits matched nonblocking GPU receipt resolution");
            check(pixels == take(async_renderer->render(source, r)).image.display_rgba8,
                  "deferred producer matches synchronous numeric reference");
            for (unsigned i = 1; i < 4; ++i)
                pending.push_back(take(async_renderer->render_resident(source, r, false)));
            check(!async_renderer->render_resident(source, r, false),
                  "bounded receipt pool rejects overwriting four retained measurements");
            pending.front().deferred_timing.reset();
            auto reused_slot = take(async_renderer->render_resident(source, r, false));
            static_cast<void>(
                resident_pixels(*test_device, *reused_slot.resident_image, r.width, r.height));
            check(take(reused_slot.deferred_timing->resolve()).envelope_ms >= 0 &&
                      take(pending.back().deferred_timing->resolve()).envelope_ms >= 0,
                  "released timing storage is reused without corrupting other receipts");
            for(const auto& message:test_device->debug_receipts()) std::cerr<<message<<'\n';
            check(test_device->debug_receipts().empty(),
                  "deferred queue/lifetime validation is clean");
        }
        {
            auto shared_device = test_device;
            auto resident_renderer =
                take(d3d12::AuthoringViewport::create_on_device(shared_device));
            const auto source = std::make_shared<const project::ProjectDocument>(f.document);
            auto displayed = take(resident_renderer->render_resident(source, r));
            check(displayed.resident_image && displayed.resident_image->device == shared_device &&
                      displayed.image_transfer_bytes == 0 && displayed.readback_bytes == 0 &&
                      displayed.image.display_rgba8.empty() && displayed.image.objects.empty() &&
                      displayed.image.linear_hdr.empty(),
                  "resident presentation shares the actual device and performs no frame readback");
            const auto gpu_pixels =
                resident_pixels(*shared_device, *displayed.resident_image, r.width, r.height);
            const auto numeric = take(resident_renderer->render(source, r));
            compare(take(production::render_authoring_viewport(f.document, r)), numeric.image);
            check(gpu_pixels == numeric.image.display_rgba8,
                  "resident GPU image exactly matches explicit reference readback");
            const auto old_texture = displayed.resident_image->texture;
            r.width = 128;
            r.height = 96;
            auto resized = take(resident_renderer->render_resident(source, r));
            check(shared_device->native_texture_handle(old_texture).has_value(),
                  "presentation lease retains the old target generation through resize");
            displayed.resident_image.reset();
            check(!shared_device->native_texture_handle(old_texture),
                  "retired target handles reject access after the last lease is released");
            resident_renderer.reset();
            check(
                shared_device->native_texture_handle(resized.resident_image->texture).has_value() &&
                    !resident_pixels(*shared_device, *resized.resident_image, r.width, r.height)
                         .empty(),
                "leased presentation survives renderer teardown on the same device");
            check(shared_device->debug_receipts().empty(),
                  "resident lifetime/barrier validation is clean");
            r = request(f);
        }
        auto first = take(renderer->render(f.document, r));
        auto reference = take(d3d12::AuthoringViewport::create(false));
        auto reference_frame = take(reference->render(f.document, r));
        compare(reference_frame.image, first.image);
        check(reference_frame.image.objects == first.image.objects,
              "fused GPU identities match the two-pass reference");
        const auto immutable = std::make_shared<const project::ProjectDocument>(f.document);
        auto immutable_first = take(renderer->render(immutable, r));
        {
            auto material_edit = f.document.material_catalog();
            auto material = material_edit.materials().at(1U);
            material.definition.base_color_factor = {.1, .6, .2, 1};
            material.revision = material.revision.next();
            okay(material_edit.replace_material(material));
            auto edited = f.document;
            set_materials(edited, std::move(material_edit));
            const auto changed =
                take(renderer->render(std::make_shared<const project::ProjectDocument>(edited), r));
            check(changed.mesh_compile_count == 0 && changed.mesh_cache_hits == 2 &&
                      changed.image.display_rgba8 != immutable_first.image.display_rgba8,
                  "material edit reuses content-verified meshes and updates visible shading");
            compare(take(production::render_authoring_viewport(edited, r)), changed.image);
            auto external = f.document.serialize();
            const auto vertex = external.find("VERTEX 1 -1 -1 0");
            check(vertex != std::string::npos,
                  "external mesh edit fixture locates a source vertex");
            external.replace(vertex, std::string("VERTEX 1 -1 -1 0").size(), "VERTEX 1 -0.5 -1 0");
            auto reopened = take(project::ProjectDocument::deserialize(external));
            check(reopened.revision() == f.document.revision() &&
                      reopened.meshes().at(f.front_mesh).revision() ==
                          f.document.meshes().at(f.front_mesh).revision(),
                  "external reopen retains colliding project and mesh revisions");
            const auto modified = take(
                renderer->render(std::make_shared<const project::ProjectDocument>(reopened), r));
            check(modified.mesh_compile_count == 1 && modified.geometry_upload_bytes > 0 &&
                      modified.image.objects != immutable_first.image.objects,
                  "content digest rejects stale geometry despite matching identities/revisions");
            compare(take(production::render_authoring_viewport(reopened, r)), modified.image);
        }
        auto reusable = take(renderer->render(immutable, r));
        const auto *hdr_storage = reusable.image.linear_hdr.data();
        const auto *id_storage = reusable.image.objects.data();
        auto reused = take(renderer->render(immutable, r, std::move(reusable.image)));
        check(reused.image.linear_hdr.data() == hdr_storage &&
                  reused.image.objects.data() == id_storage,
              "explicit ownership transfer reuses host attachments without "
              "aliasing retained frames");
        auto cleared_request = r;
        cleared_request.transform_overrides[f.front] = {{40, 0, 0}};
        cleared_request.transform_overrides[f.back] = {{40, 0, 0}};
        auto cleared = take(renderer->render(immutable, cleared_request, std::move(reused.image)));
        check(std::all_of(cleared.image.objects.begin(), cleared.image.objects.end(),
                          [](auto id) { return !id; }) &&
                  std::all_of(cleared.image.faces.begin(), cleared.image.faces.end(),
                              [](auto id) { return !id; }) &&
                  std::all_of(cleared.image.linear_hdr.begin(), cleared.image.linear_hdr.end(),
                              [](auto v) { return v.length_squared() == 0; }) &&
                  std::all_of(cleared.image.depth.begin(), cleared.image.depth.end(),
                              [](double v) { return std::isinf(v); }) &&
                  std::all_of(cleared.image.display_rgba8.begin(),
                              cleared.image.display_rgba8.end(), [](auto v) { return v == 0; }),
              "recycled background clears every prior numeric and picking "
              "attachment");
        auto restored_reuse = take(renderer->render(immutable, r, std::move(cleared.image)));
        compare(immutable_first.image, restored_reuse.image);
        auto suspended = r;
        suspended.width = 0;
        auto suspended_reuse =
            take(renderer->render(immutable, suspended, std::move(restored_reuse.image)));
        check(suspended_reuse.image.objects.empty() && suspended_reuse.image.linear_hdr.empty() &&
                  suspended_reuse.image.display_rgba8.empty(),
              "suspended recycled frame exposes no stale pixels");
        auto moved_request = r;
        moved_request.transform_overrides[f.front] = {{4, 0, 0}};
        auto moved_snapshot = take(renderer->render(immutable, moved_request));
        check(moved_snapshot.image.objects[center(moved_snapshot.image)] == f.back,
              "immutable preparation cache observes object motion");
        check(take(renderer->render(immutable, r)).image.objects == immutable_first.image.objects,
              "removing transient overrides restores the source pose");
        compare(take(production::render_authoring_viewport(f.document, r)), first.image);
        check(first.image.objects[center(first.image)] == f.front,
              "GPU depth rejects the back object drawn last");
        auto cached = take(renderer->render(f.document, r));
        check(cached.geometry_upload_bytes == 0 &&
                  cached.target_generation == first.target_generation,
              "unchanged GPU resources persist");
        for (double roughness : {0.05, 0.3, 0.8, 1.0})
            for (double metal : {0.0, 0.5, 1.0})
                for (double angle : {0.0, 0.5, 1.2}) {
                    auto catalog = f.document.material_catalog();
                    auto material = catalog.materials().at(1U);
                    material.revision = material.revision.next();
                    material.definition.roughness = roughness;
                    material.definition.metallic = metal;
                    material.definition.clearcoat = 0.4;
                    material.definition.clearcoat_roughness = 0.2;
                    okay(catalog.replace_material(material));
                    set_materials(f.document, catalog);
                    r.camera.reset();
                    r.view.position = {3 * std::sin(angle), 0, 3 * std::cos(angle)};
                    r.view.rotation = {0, std::sin(angle * .5), 0, std::cos(angle * .5)};
                    r.view.orthographic_height = 4;
                    r.view.near_plane = .1;
                    r.view.far_plane = 30;
                    r.view.projection = project::CameraProjection::perspective;
                    auto frame = take(renderer->render(f.document, r));
                    compare(take(production::render_authoring_viewport(f.document, r)),
                            frame.image);
                    check(frame.geometry_upload_bytes == 0,
                          "material/camera edits do not re-upload geometry");
                }
        r = request(f);
        for (auto color : {core::Vec3d{.18, .18, .18}, core::Vec3d{.5, .5, .5},
                           core::Vec3d{1, 0, 0}, core::Vec3d{0, 1, 0}, core::Vec3d{0, 0, 1}}) {
            auto catalog = f.document.material_catalog();
            auto m = catalog.materials().at(1U);
            m.revision = m.revision.next();
            m.definition.base_color_factor = {color.x, color.y, color.z, 1};
            m.definition.emissive_factor = {4, 2, 1};
            m.definition.emissive_strength = 1;
            okay(catalog.replace_material(m));
            set_materials(f.document, catalog);
            for (auto tone : {render::ToneMapMode::aces_fitted, render::ToneMapMode::neutral,
                              render::ToneMapMode::linear_diagnostic})
                for (double ev : {-2.0, 0.0, 2.0}) {
                    r.tone_mode = tone;
                    r.exposure_ev = ev;
                    auto frame = take(renderer->render(f.document, r));
                    compare(take(production::render_authoring_viewport(f.document, r)),
                            frame.image);
                }
        }
        for (auto scale : {core::Vec3d{2, .5, 1}, core::Vec3d{-2, .5, 1}}) {
            core::Transform t;
            t.scale = scale;
            t.rotation = {0, std::sin(.2), 0, std::cos(.2)};
            r.transform_overrides[f.front] = t;
            compare(take(production::render_authoring_viewport(f.document, r)),
                    take(renderer->render(f.document, r)).image);
        }
        r = request(f);
        {
            auto masked = fixture();
            auto catalog = masked.document.material_catalog();
            auto material = catalog.materials().at(1U);
            material.revision = material.revision.next();
            material.definition.alpha_mode = render::AlphaMode::masked;
            material.definition.base_color_factor.w = .2;
            material.definition.alpha_cutoff = .5;
            okay(catalog.replace_material(material));
            set_materials(masked.document, catalog);
            const auto mr = request(masked);
            const auto frame = take(renderer->render(masked.document, mr));
            compare(take(production::render_authoring_viewport(masked.document, mr)), frame.image);
            compare(take(reference->render(masked.document, mr)).image, frame.image);
            check(frame.image.objects[center(frame.image)] == masked.back,
                  "masked front fragments preserve background depth and authored "
                  "identity");
        }
        {
            auto bright = fixture();
            auto catalog = bright.document.material_catalog();
            auto material = catalog.materials().at(1U);
            material.revision = material.revision.next();
            material.definition.emissive_factor = {1e20, 1e10, 4};
            material.definition.emissive_strength = 1;
            okay(catalog.replace_material(material));
            set_materials(bright.document, catalog);
            auto br = request(bright);
            br.exposure_ev = 32;
            for (auto mode : {render::ToneMapMode::aces_fitted, render::ToneMapMode::neutral,
                              render::ToneMapMode::linear_diagnostic}) {
                br.tone_mode = mode;
                const auto frame = take(renderer->render(bright.document, br));
                compare(take(production::render_authoring_viewport(bright.document, br)),
                        frame.image);
                check(frame.image.linear_hdr[center(frame.image)].x > 1e19,
                      "GPU display transform preserves unclamped and unexposed HDR");
            }
        }
        r.width = 96;
        r.height = 48;
        auto resized = take(renderer->render(f.document, r));
        check(resized.target_generation > first.target_generation,
              "resize replaces GPU attachments");
        r.width = 1048577;
        check(!renderer->render(f.document, r), "oversized GPU target fails before allocation");
        r.width = 96;
        check(take(renderer->render(f.document, r)).target_generation == resized.target_generation,
              "failed resize preserves good target generation");
        r.width = 0;
        check(take(renderer->render(f.document, r)).image.objects.empty(),
              "zero extent suspends GPU rendering");
        r = request(f);
        auto frame = take(renderer->render(f.document, r));
        auto reopened = take(project::ProjectDocument::deserialize(f.document.serialize()));
        check(take(renderer->render(reopened, r)).image.display_rgba8 == frame.image.display_rgba8,
              "GPU save/reopen appearance stable");
        {
            auto normals = fixture();
            auto mesh = triangle();
            attributes::AttributeSet payload({{attributes::AttributeDomain::corner, 3U},
                                              {attributes::AttributeDomain::face, 1U}});
            okay(
                payload.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                                    attributes::AttributeType::uint32},
                                   {std::uint32_t{1}}}));
            const core::Vec3d normal{.6, 0, .8};
            okay(payload.add_layer({{"cartographer.normal0", attributes::AttributeDomain::corner,
                                     attributes::AttributeType::vec3},
                                    {normal, normal, normal}}));
            okay(mesh.set_attribute_payload(std::move(payload), {}));
            mutate(normals.document, [&](auto &transaction) {
                okay(transaction.replace_mesh(normals.front_mesh, std::move(mesh)));
            });
            auto nrequest = request(normals);
            for (double mirror : {1.0, -1.0}) {
                nrequest.transform_overrides[normals.front] = {
                    {}, {0, std::sin(.2), 0, std::cos(.2)}, {mirror * 2, .5, 1}};
                compare(take(production::render_authoring_viewport(normals.document, nrequest)),
                        take(renderer->render(normals.document, nrequest)).image);
            }
            auto catalog = normals.document.presentation_catalog();
            auto camera = catalog.cameras().at(normals.camera);
            camera.revision = camera.revision.next();
            camera.exposure_ev = 1.5;
            camera.tone_mode = render::ToneMapMode::neutral;
            okay(catalog.replace_camera(camera));
            mutate(normals.document,
                   [&](auto &transaction) { okay(transaction.set_presentation_catalog(catalog)); });
            nrequest.camera_display = true;
            const auto authored = take(renderer->render(normals.document, nrequest));
            compare(take(production::render_authoring_viewport(normals.document, nrequest)),
                    authored.image);
            const auto restored =
                take(project::ProjectDocument::deserialize(normals.document.serialize()));
            check(take(renderer->render(restored, nrequest)).image.display_rgba8 ==
                      authored.image.display_rgba8,
                  "GPU camera-owned exposure/tone survive reopen");
        }
        std::cout << "GPU authored viewport passed: " << frame.device_name << "; GPU HDR+IDs "
                  << frame.gpu_milliseconds << " ms; submit/wait " << frame.submit_wait_milliseconds
                  << " ms; HDR/depth/ID readback " << frame.readback_milliseconds
                  << " ms; CPU display " << frame.display_milliseconds
                  << " ms; logical resident payload " << frame.logical_resident_bytes << " bytes\n";
        if (argc > 1) {
            const std::filesystem::path output(argv[1]);
            std::filesystem::create_directories(output);
            static_cast<void>(take(f.document.save_atomic(output / "gpu-viewport.carto")));
            std::ofstream ppm(output / "gpu-viewport.ppm", std::ios::binary);
            ppm << "P6\n64 64\n255\n";
            for (std::size_t p = 0; p < frame.image.objects.size(); ++p)
                ppm.write(reinterpret_cast<const char *>(&frame.image.display_rgba8[p * 4U]), 3);
        }
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
