#include "authoring_environment_fixture.hpp"
#include "authoring_surface_fixture.hpp"
#include <carto/d3d12/authoring_viewport.hpp>

#include <algorithm>
#include <iostream>

int main() {
    try {
        auto environment = asymmetric_environment();
        std::fill(environment.linear_radiance.begin(), environment.linear_radiance.end(),
                  core::Vec3d{2.0, 2.0, 2.0});
        auto f = environment_fixture(environment);
        auto front = request(f);
        front.camera.reset();
        front.view.position = {0, 0, 3};
        front.view.rotation = {0, 0, 0, 1};
        front.view.projection = project::CameraProjection::orthographic;
        front.view.orthographic_height = 4;
        front.view.near_plane = .01;
        front.view.far_plane = 10;
        auto back = front;
        back.view.position = {0, 0, -3};
        back.view.rotation = {0, 1, 0, 0};
        auto mirrored_front = front;
        core::Transform mirrored;
        mirrored.scale = {-1, 1, 1};
        mirrored_front.transform_overrides.emplace(f.front, mirrored);
        auto mirrored_back = back;
        mirrored.translation = {0, 0, -1};
        mirrored_back.transform_overrides.emplace(f.back, mirrored);
        auto gpu = take(d3d12::AuthoringViewport::create());
        for (const auto& view : {front, back, mirrored_front, mirrored_back}) {
            const auto cpu = take(production::render_authoring_viewport(f.document, view));
            const auto native = take(gpu->render(f.document, view)).image;
            const auto pixel = center(cpu);
            check(cpu.objects[pixel].value != 0 && native.objects[pixel] == cpu.objects[pixel],
                  "both sides must retain the same visible object identity");
            check(cpu.linear_hdr[pixel].x > .1 && native.linear_hdr[pixel].x > .1,
                  "both sides of a two-sided surface must receive environment light");
            for (std::size_t channel = 0; channel < 3; ++channel)
                check(std::abs(int(cpu.display_rgba8[pixel * 4 + channel]) -
                               int(native.display_rgba8[pixel * 4 + channel])) <= 2,
                      "CPU and GPU disagree about two-sided surface shading");
        }
        // Exercise the tangent frame on the back of a normal-mapped face as
        // well as its front. The untextured fixtures above cannot detect a
        // sign error in a sampled tangent-space normal.
        mutate(f.document, [&](auto& tx) { okay(tx.replace_mesh(f.front_mesh, uv_triangle())); });
        const auto normal = pixels(72, 1, 1, {{170, 190, 230, 255}});
        auto library = std::make_shared<production::AuthoringTextureLibrary>();
        okay(library->admit(normal.record, normal.encoded));
        auto catalog = f.document.material_catalog();
        okay(catalog.insert_texture(normal.record));
        auto material = catalog.materials().at(1);
        material.revision = material.revision.next();
        material.definition.normal = {72, 9};
        material.definition.normal.normal_convention = render::NormalConvention::opengl_y_plus;
        okay(catalog.replace_material(material));
        set_materials(f.document, std::move(catalog));
        front.textures = library;
        back.textures = library;
        back.transform_overrides[f.back] = {{0, 0, 1}};
        for (const auto& view : {front, back}) {
            const auto cpu = take(production::render_authoring_viewport(f.document, view));
            const auto native = take(gpu->render(f.document, view)).image;
            const auto pixel = center(cpu);
            check(cpu.objects[pixel] == f.front && native.objects[pixel] == f.front,
                  "normal-mapped fixture must expose the intended face");
            check(cpu.linear_hdr[pixel].x > .1 && native.linear_hdr[pixel].x > .1,
                  "normal-mapped two-sided surface must receive environment light");
            for (std::size_t channel = 0; channel < 3; ++channel)
                check(std::abs(int(cpu.display_rgba8[pixel * 4 + channel]) -
                               int(native.display_rgba8[pixel * 4 + channel])) <= 2,
                      "CPU and GPU disagree about a normal-mapped face");
        }
        std::cout << "two-sided authoring shading passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
