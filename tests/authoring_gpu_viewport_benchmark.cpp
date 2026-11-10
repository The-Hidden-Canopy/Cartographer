#include "authoring_viewport_fixture.hpp"
#include <algorithm>
#include <carto/d3d12/authoring_viewport.hpp>
#include <carto/geometry/primitives.hpp>
#include <fstream>
#include <iostream>
#include <numbers>

namespace {
geometry::EditableMesh sphere() {
    constexpr std::uint32_t lat = 16, lon = 32;
    std::vector<geometry::Vertex> vertices{{geometry::VertexId{1}, {0, .8, 0}}};
    for (std::uint32_t r = 1; r < lat; ++r)
        for (std::uint32_t c = 0; c < lon; ++c) {
            const double a = std::numbers::pi * r / lat, b = 2 * std::numbers::pi * c / lon;
            vertices.push_back({geometry::VertexId{vertices.size() + 1},
                                {.8 * std::sin(a) * std::cos(b), .8 * std::cos(a),
                                 .8 * std::sin(a) * std::sin(b)}});
        }
    const geometry::VertexId bottom{vertices.size() + 1};
    vertices.push_back({bottom, {0, -.8, 0}});
    std::vector<geometry::Face> faces;
    auto ring = [](std::uint32_t r, std::uint32_t c) {
        return geometry::VertexId{2U + (r - 1U) * lon + c % lon};
    };
    auto face = [&](geometry::VertexId a, geometry::VertexId b, geometry::VertexId c) {
        const auto p = vertices[a.value - 1].position, q = vertices[b.value - 1].position,
                   t = vertices[c.value - 1].position;
        if (core::dot(core::cross(q - p, t - p), p + q + t) < 0)
            std::swap(b, c);
        faces.push_back({geometry::FaceId{faces.size() + 1}, {a, b, c}});
    };
    for (std::uint32_t c = 0; c < lon; ++c) {
        face(geometry::VertexId{1}, ring(1, c), ring(1, c + 1));
        for (std::uint32_t r = 1; r + 1 < lat; ++r) {
            face(ring(r, c), ring(r + 1, c), ring(r + 1, c + 1));
            face(ring(r, c), ring(r + 1, c + 1), ring(r, c + 1));
        }
        face(bottom, ring(lat - 1, c + 1), ring(lat - 1, c));
    }
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    return mesh;
}
void slot(geometry::EditableMesh &mesh) {
    attributes::AttributeSet attributes({{attributes::AttributeDomain::face, mesh.face_count()}});
    attributes::AttributeLayer layer{{"cartographer.material_slot",
                                      attributes::AttributeDomain::face,
                                      attributes::AttributeType::uint32},
                                     {}};
    layer.values.assign(mesh.face_count(), std::uint32_t{1});
    okay(attributes.add_layer(std::move(layer)));
    okay(mesh.set_attribute_payload(std::move(attributes), {}));
}
Fixture workload(unsigned spheres = 6U) {
    auto f = fixture();
    auto catalog = f.document.material_catalog();
    auto presentation = f.document.presentation_catalog();
    mutate(f.document, [&](auto &transaction) {
        for (unsigned i = 0; i < spheres + 2U; ++i) {
            auto mesh = i < spheres ? sphere()
                                    : (i == spheres ? take(geometry::make_plane(12, 10))
                                                    : take(geometry::make_box({1.4, 1.4, 1.4})));
            if (i == spheres + 1U)
                static_cast<void>(take(mesh.bevel_edge(mesh.edges_sorted(1U).front().id, .12, 3)));
            slot(mesh);
            const auto asset = take(transaction.add_mesh(std::move(mesh)));
            const auto object = take(transaction.create_object(
                i < spheres ? "BRDF sphere" : (i == spheres ? "Neutral floor" : "Chamfered box")));
            okay(transaction.attach_mesh(object, asset));
            core::Transform t;
            if (i < spheres) {
                const unsigned columns =
                    spheres == 6 ? 3U : static_cast<unsigned>(std::ceil(std::sqrt(spheres * 1.5)));
                const unsigned rows = (spheres + columns - 1) / columns;
                const double scale = 3.0 / columns;
                t.translation = {(double(i % columns) * 2.2 - double(columns - 1) * 1.1) * scale, 0,
                                 (double(i / columns) * 2.3 - double(rows - 1) * 1.1) * scale};
                if (spheres == 6)
                    t.translation = {double(i % 3) * 2.2 - 2.2, 0, double(i / 3) * 2.3 - 1.1};
                t.scale = {scale, scale, scale};
            } else if (i == spheres)
                t.translation = {0, -.8, 0};
            else
                t.translation = {3.8, -.1, -1};
            okay(transaction.set_object_transform(object, t));
            render::MaterialAsset m;
            m.id = 10 + i;
            m.name = "Public diagnostic material";
            m.definition.base_color_factor =
                i == spheres ? core::Vec4d{.18, .18, .18, 1} : core::Vec4d{.5, .3, .1, 1};
            m.definition.roughness = i < spheres ? std::array<double, 3>{.05, .45, .95}[i % 3] : .5;
            m.definition.metallic = i < spheres && (i % 6) < 3 ? 0 : 1;
            if (i == spheres)
                m.definition.metallic = 0;
            okay(catalog.insert_material(m));
            okay(catalog.insert_assignment({{asset, 1}, m.id, {}}));
        }
        okay(transaction.set_material_catalog(catalog));
        okay(transaction.set_object_transform(
            f.camera, {{0, 3, 8}, {std::sin(-.175), 0, 0, std::cos(-.175)}}));
        okay(transaction.set_object_transform(f.light,
                                              {{0, 0, 0}, {std::sin(-.35), 0, 0, std::cos(-.35)}}));
        project::PresentationCatalog next;
        for (auto [id, c] : presentation.cameras()) {
            static_cast<void>(id);
            c.projection = project::CameraProjection::perspective;
            okay(next.insert_camera(c));
        }
        for (auto [id, l] : presentation.lights()) {
            static_cast<void>(id);
            okay(next.insert_light(l));
        }
        okay(transaction.set_presentation_catalog(next));
    });
    return f;
}
void summary(const char *name, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    if (values.back() < 0) {
        std::cout << name << " unavailable (not separately timestamped)\n";
        return;
    }
    double sum = 0;
    for (double x : values)
        sum += x;
    std::cout << name << " ms min=" << values.front() << " p50=" << values[values.size() / 2]
              << " p95=" << values[std::size_t(std::ceil(.95 * double(values.size()))) - 1]
              << " max=" << values.back() << " mean=" << sum / double(values.size()) << '\n';
}
} // namespace
int main(int argc, char **argv) {
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : ".";
        std::filesystem::create_directories(output);
        const auto scene_path = output / "gpu-profile-scene.carto";
        const unsigned sphere_count = argc > 3 ? static_cast<unsigned>(std::stoul(argv[3])) : 6U;
        const unsigned resolution = argc > 4 ? static_cast<unsigned>(std::stoul(argv[4])) : 512U;
        check(sphere_count >= 1 && sphere_count <= 60 && resolution >= 256 && resolution <= 1024,
              "bounded benchmark accepts 1..60 spheres and 256..1024 square pixels");
        const bool saved = std::filesystem::exists(scene_path);
        const auto f = saved ? Fixture{take(project::ProjectDocument::load(scene_path)),
                                       {1},
                                       {2},
                                       {3},
                                       {4},
                                       1,
                                       2}
                             : workload(sphere_count);
        auto r = request(f);
        r.width = resolution;
        r.height = resolution;
        const auto init = std::chrono::steady_clock::now();
        const std::string mode = argc > 2 ? argv[2] : "reference";
        const bool optimized = mode != "reference";
        const bool refresh = mode.find("refresh") != std::string::npos;
        const bool cache_meshes = mode.find("uncached") == std::string::npos;
        check(mode == "reference" || mode == "optimized" || mode == "optimized-refresh" ||
              mode == "optimized-refresh-uncached", "unknown benchmark mode");
        const auto frozen = std::make_shared<const project::ProjectDocument>(f.document);
        auto renderer = take(d3d12::AuthoringViewport::create(optimized, cache_meshes));
        std::cout << "renderer initialize ms="
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               init)
                         .count()
                  << '\n';
        const auto cold_start = std::chrono::steady_clock::now();
        auto cold = take(renderer->render(frozen, r));
        const auto cold_external_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cold_start)
                .count();
        {
            auto oracle = take(d3d12::AuthoringViewport::create(!optimized));
            const auto other = take(oracle->render(frozen, r));
            check(cold.image.objects == other.image.objects &&
                      cold.image.faces == other.image.faces,
                  "optimized/reference full-frame authored identity mismatch");
            double max_hdr_error = 0, max_depth_error = 0;
            int max_byte_error = 0;
            std::size_t covered = 0;
            for (std::size_t p = 0; p < cold.image.objects.size(); ++p) {
                if (!cold.image.objects[p])
                    continue;
                ++covered;
                const auto a = cold.image.linear_hdr[p], b = other.image.linear_hdr[p];
                for (const auto [x, y] :
                     {std::pair{a.x, b.x}, std::pair{a.y, b.y}, std::pair{a.z, b.z}}) {
                    check(std::abs(x - y) <= 2e-4 + 1e-3 * std::abs(y),
                          "optimized/reference full-frame HDR mismatch");
                    max_hdr_error = std::max(max_hdr_error, std::abs(x - y));
                }
                max_depth_error =
                    std::max(max_depth_error, std::abs(cold.image.depth[p] - other.image.depth[p]));
                for (std::size_t c = 0; c < 4; ++c)
                    max_byte_error = std::max(max_byte_error,
                                              std::abs(int(cold.image.display_rgba8[p * 4 + c]) -
                                                       int(other.image.display_rgba8[p * 4 + c])));
            }
            check(covered > 10000 && max_depth_error <= 1e-5 && max_byte_error <= 1,
                  "optimized/reference coverage/depth/display mismatch");
            std::cout << "full-frame reference parity covered=" << covered
                      << " max HDR abs=" << max_hdr_error << " max depth abs=" << max_depth_error
                      << " max display byte=" << max_byte_error << '\n';
        }
        std::cout << "device=" << cold.device_name
                  << " startup device ms=" << cold.startup_device_milliseconds
                  << " startup DXC/pipeline ms=" << cold.startup_shader_pipeline_milliseconds
                  << " cold allocation ms=" << cold.allocation_milliseconds
                  << " cold upload bytes=" << cold.geometry_upload_bytes
                  << " logical resident payload bytes=" << cold.logical_resident_bytes
                  << " draws=" << cold.draw_calls << " triangles=" << cold.triangles
                  << " readback bytes=" << cold.readback_bytes << "\n";
        std::cout << "cold render external ms=" << cold_external_ms
                  << " source prepare ms=" << cold.cpu_prepare_milliseconds
                  << " geometry pack/upload ms=" << cold.geometry_update_milliseconds
                  << " internal return ms=" << cold.host_timeline.returning << '\n';
        std::cout << "prepare nested: validation=" << cold.prepare_validate_ms
                  << " identity serialization/hash=" << cold.prepare_identity_ms
                  << " mesh compilation=" << cold.prepare_meshes_ms
                  << " material resolution=" << cold.prepare_materials_ms << " ms\n";
        std::cout << "source refresh=" << refresh << " mesh cache=" << cache_meshes
                  << " cold mesh compiles=" << cold.mesh_compile_count
                  << " content cache hits=" << cold.mesh_cache_hits
                  << "; refresh snapshot copy is excluded from render-call timing\n";
        constexpr unsigned warmup = 5, samples = 30;
        production::AuthoringViewportImage recycled;
        for (unsigned i = 0; i < warmup; ++i) {
            auto source = refresh ? std::make_shared<const project::ProjectDocument>(f.document) : frozen;
            auto warm = take(renderer->render(source, r, std::move(recycled)));
            recycled = std::move(warm.image);
        }
        std::ofstream csv(output / ("gpu-profile-" + mode + ".csv"));
        csv << "sample,gpu_hdr_ids_ms,cpu_submit_ms,fence_wait_ms,readback_api_ms,"
               "cpu_resolve_display_ms,cpu_prepare_ms,geometry_update_ms,"
               "allocation_ms,"
               "total_ms,gpu_transfer_ms,gpu_display_ms,upload_bytes\n";
        std::array<std::vector<double>, 11> metrics;
        constexpr std::array host_names{"prepare_setup",
                                        "resize",
                                        "geometry",
                                        "record",
                                        "submit",
                                        "wait",
                                        "query_read",
                                        "readback_unpack",
                                        "output_allocate",
                                        "resolve",
                                        "finalize",
                                        "external_call",
                                        "map_nested",
                                        "staging_allocate_nested",
                                        "copy_nested",
                                        "unmap_nested",
                                        "gpu_all_stages",
                                        "gpu_begin_host",
                                        "gpu_end_host",
                                        "clock_uncertainty",
                                        "gpu_query_resolve",
                                        "submit_validate_nested",
                                        "submit_context_nested",
                                        "submit_encode_nested",
                                        "submit_execute_signal_nested",
                                        "submit_context_wait_nested"};
        std::array<std::vector<double>, host_names.size()> host_metrics;
        std::ofstream timeline(output / ("host-timeline-" + mode + ".csv"));
        timeline.precision(10);
        timeline << "sample";
        for (const auto *name : host_names)
            timeline << ',' << name << "_ms";
        timeline << '\n';
        for (unsigned i = 0; i < samples; ++i) {
            auto source = refresh ? std::make_shared<const project::ProjectDocument>(f.document) : frozen;
            const auto call_start = std::chrono::steady_clock::now();
            auto frame = take(renderer->render(source, r, std::move(recycled)));
            if (refresh && cache_meshes) check(frame.mesh_compile_count == 0,
                "unchanged source content must reuse meshes across snapshot replacement");
            const auto external_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - call_start)
                                         .count();
            const auto &t = frame.host_timeline;
            const std::array boundaries{t.prepared,        t.resources_ready, t.geometry_ready,
                                        t.commands_ready,  t.submitted,       t.fence_complete,
                                        t.timestamps_read, t.readback_ready,  t.outputs_allocated,
                                        t.resolved,        t.returning};
            double previous = 0;
            std::array<double, host_names.size()> host{};
            for (std::size_t k = 0; k < boundaries.size(); ++k) {
                check(boundaries[k] >= previous, "host timeline must be monotonic");
                host[k] = boundaries[k] - previous;
                previous = boundaries[k];
            }
            check(external_ms >= t.returning, "external call contains every host phase");
            host[11] = external_ms;
            host[12] = frame.readback_map_ms;
            host[13] = frame.readback_allocation_ms;
            host[14] = frame.readback_copy_ms;
            host[15] = frame.readback_unmap_ms;
            host[16] = frame.gpu_all_stages_milliseconds;
            host[17] = frame.gpu_begin_host_ms;
            host[18] = frame.gpu_end_host_ms;
            host[19] = frame.clock_uncertainty_ms;
            host[20] = frame.gpu_query_resolve_ms;
            host[21] = frame.submit_validate_ms;
            host[22] = frame.submit_context_ms;
            host[23] = frame.submit_encode_ms;
            host[24] = frame.submit_execute_signal_ms;
            host[25] = frame.submit_context_wait_ms;
            timeline << i;
            for (std::size_t k = 0; k < host.size(); ++k) {
                host_metrics[k].push_back(host[k]);
                timeline << ',' << host[k];
            }
            timeline << '\n';
            check(frame.geometry_upload_bytes == 0 && frame.allocation_milliseconds == 0,
                  "warm workload must reuse resources");
            const std::array<double, 11> v{frame.gpu_milliseconds,
                                           frame.cpu_submission_milliseconds,
                                           frame.fence_wait_milliseconds,
                                           frame.readback_milliseconds,
                                           frame.display_milliseconds,
                                           frame.cpu_prepare_milliseconds,
                                           frame.geometry_update_milliseconds,
                                           frame.allocation_milliseconds,
                                           frame.image.host_milliseconds,
                                           frame.gpu_transfer_milliseconds,
                                           frame.gpu_display_milliseconds};
            csv << i;
            for (std::size_t j = 0; j < v.size(); ++j) {
                metrics[j].push_back(v[j]);
                csv << ',' << v[j];
            }
            csv << ",0\n";
            check(frame.image.display_rgba8 == cold.image.display_rgba8 &&
                      frame.image.objects == cold.image.objects &&
                      frame.image.faces == cold.image.faces &&
                      frame.image.depth == cold.image.depth &&
                      std::equal(frame.image.linear_hdr.begin(), frame.image.linear_hdr.end(),
                                 cold.image.linear_hdr.begin(),
                                 [](auto a, auto b) { return (a - b).length_squared() == 0; }),
                  "fixed workload must remain identical while profiled");
            recycled = std::move(frame.image);
        }
        std::cout << "fixed authored scene: " << sphere_count
                  << " spheres, floor, chamfered box, two "
                     "occlusion "
                     "triangles; "
                  << resolution << 'x' << resolution << "; directional light; warmup=" << warmup
                  << " samples=" << samples << "\n";
        constexpr std::array names{"gpu_hdr_ids",     "cpu_submit",          "fence_wait",
                                   "readback_api",    "cpu_resolve_display", "cpu_prepare",
                                   "geometry_update", "allocation",          "total",
                                   "gpu_transfer",    "gpu_display"};
        for (std::size_t i = 0; i < metrics.size(); ++i)
            summary(names[i], metrics[i]);
        std::cout << "Host phases through finalize are consecutive and sum per sample; "
                     "map/allocation/copy/unmap are nested inside readback, GPU stages "
                     "overlap "
                     "host submit/wait. External call includes return/destruction "
                     "overhead. "
                     "Returned host attachment ownership is recycled after each "
                     "comparison.\n";
        for (std::size_t i = 0; i < host_metrics.size(); ++i)
            summary(host_names[i], host_metrics[i]);
        if (!saved)
            static_cast<void>(take(f.document.save_atomic(scene_path)));
        const auto source = f.document.serialize();
        std::cout << "scene sha256="
                  << assets::sha256(
                         std::span<const std::uint8_t>{
                             reinterpret_cast<const std::uint8_t *>(source.data()), source.size()})
                         .hex()
                  << '\n';
        std::ofstream ppm(output / ("gpu-profile-" + mode + ".ppm"), std::ios::binary);
        ppm << "P6\n" << resolution << ' ' << resolution << "\n255\n";
        for (std::size_t p = 0; p < cold.image.objects.size(); ++p)
            ppm.write(reinterpret_cast<const char *>(&cold.image.display_rgba8[p * 4]), 3);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
