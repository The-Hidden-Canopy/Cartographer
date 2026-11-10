// CPU-only acceptance-scene authoring/capture tool. No GPU or native window.
#include "authoring_environment_fixture.hpp"
#include "authoring_surface_fixture.hpp"
#include <carto/geometry/primitives.hpp>
#include <fstream>
#include <iostream>

namespace {
geometry::EditableMesh normal_mesh(geometry::EditableMesh mesh, bool radial = false,
                                   bool uv = false) {
    const auto faces = mesh.faces_sorted();
    std::vector<attributes::AttributeValue> normals, coordinates;
    std::map<geometry::FaceId, core::Vec3d> face_normals;
    std::map<std::pair<geometry::FaceId, geometry::VertexId>, core::Vec2d> face_uvs;
    for (const auto &face : faces) {
        core::Vec3d plane{};
        for (std::size_t i = 0; i < face.vertices.size(); ++i)
            plane = plane +
                    core::cross(
                        mesh.find_vertex(face.vertices[i])->position,
                        mesh.find_vertex(face.vertices[(i + 1) % face.vertices.size()])->position);
        plane = plane.normalized();
        face_normals.emplace(face.id, plane);
        for (std::size_t i = 0; i < face.vertices.size(); ++i) {
            if (uv) {
                const std::array<core::Vec2d, 4> corners{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
                check(face.vertices.size() == 4, "normal tile/card fixture is one quad");
                face_uvs.emplace(std::pair{face.id, face.vertices[i]}, corners[i]);
            }
        }
    }
    // Payload domains follow stable corner IDs, including corners preserved
    // and created by the existing bevel operator.
    for (const auto &corner : mesh.corners_sorted(std::numeric_limits<std::size_t>::max())) {
        normals.push_back(radial ? mesh.find_vertex(corner.vertex)->position.normalized()
                                 : face_normals.at(corner.face));
        if (uv)
            coordinates.push_back(face_uvs.at({corner.face, corner.vertex}));
    }
    attributes::AttributeSet attrs({{attributes::AttributeDomain::face, faces.size()},
                                    {attributes::AttributeDomain::corner, normals.size()}});
    okay(attrs.add_layer({{"cartographer.normal0", attributes::AttributeDomain::corner,
                           attributes::AttributeType::vec3},
                          std::move(normals)}));
    okay(
        attrs.add_layer({{"cartographer.material_slot", attributes::AttributeDomain::face,
                          attributes::AttributeType::uint32},
                         std::vector<attributes::AttributeValue>(faces.size(), std::uint32_t{1})}));
    attributes::UvSetTable sets;
    if (uv) {
        okay(attrs.add_layer(
            {{"uv.scene", attributes::AttributeDomain::corner, attributes::AttributeType::vec2},
             std::move(coordinates)}));
        okay(sets.add({9, "Fixture UV", "uv.scene", true, true, false, 1}));
    }
    okay(mesh.set_attribute_payload(std::move(attrs), std::move(sets)));
    return mesh;
}
geometry::EditableMesh sphere() {
    constexpr unsigned slices = 32, bands = 24;
    std::vector<geometry::Vertex> vertices;
    std::vector<geometry::Face> faces;
    vertices.push_back({geometry::VertexId{1}, {0, .8, 0}});
    for (unsigned y = 1; y < bands; ++y)
        for (unsigned x = 0; x < slices; ++x) {
            const double t = std::numbers::pi * y / bands, p = 2 * std::numbers::pi * x / slices;
            vertices.push_back({geometry::VertexId{vertices.size() + 1},
                                {.8 * std::sin(t) * std::cos(p), .8 * std::cos(t),
                                 .8 * std::sin(t) * std::sin(p)}});
        }
    const auto bottom = geometry::VertexId{vertices.size() + 1};
    vertices.push_back({bottom, {0, -.8, 0}});
    const auto ring = [](unsigned y, unsigned x) {
        return geometry::VertexId{2 + (y - 1) * slices + x % slices};
    };
    const auto face = [&](std::vector<geometry::VertexId> ids) {
        faces.push_back({geometry::FaceId{faces.size() + 1}, std::move(ids)});
    };
    for (unsigned x = 0; x < slices; ++x) {
        face({geometry::VertexId{1}, ring(1, x + 1), ring(1, x)});
        for (unsigned y = 1; y + 1 < bands; ++y)
            face({ring(y, x), ring(y, x + 1), ring(y + 1, x + 1), ring(y + 1, x)});
        face({bottom, ring(bands - 1, x), ring(bands - 1, x + 1)});
    }
    geometry::EditableMesh mesh;
    okay(mesh.insert_bulk(std::move(vertices), std::move(faces)));
    return normal_mesh(std::move(mesh), true);
}
core::Quaternion look_at(core::Vec3d eye, core::Vec3d target) {
    const auto back = (eye - target).normalized();
    const double yaw = std::atan2(back.x, back.z), pitch = -std::asin(back.y);
    return core::Quaternion{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)} *
           core::Quaternion{std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
}
} // namespace
project::AuthoredEnvironment studio_environment() {
    project::AuthoredEnvironment e;
    e.identity = "environment.acceptance.studio";
    e.width = 64;
    e.height = 32;
    const auto key = core::Vec3d{-.5, .7, .5}.normalized(),
               fill = core::Vec3d{.8, .3, -.4}.normalized();
    for (unsigned y = 0; y < e.height; ++y)
        for (unsigned x = 0; x < e.width; ++x) {
            const double t = std::numbers::pi * (y + .5) / e.height,
                         p = 2 * std::numbers::pi * ((x + .5) / e.width - .5);
            const core::Vec3d d{std::sin(t) * std::cos(p), std::cos(t), std::sin(t) * std::sin(p)};
            const auto ambient =
                core::Vec3d{.06, .07, .085} + core::Vec3d{.25, .25, .25} * std::max(0., d.y);
            e.linear_radiance.push_back(
                ambient +
                core::Vec3d{15, 14.7, 14.1} * std::pow(std::max(0., core::dot(d, key)), 32) +
                core::Vec3d{2.8, 3.3, 4} * std::pow(std::max(0., core::dot(d, fill)), 24));
        }
    return e;
}
int main(int argc, char **argv) {
    try {
        check(argc == 2, "pass an output directory for this CPU-only authored scene");
        const auto folder = std::filesystem::absolute(argv[1]).lexically_normal();
        std::filesystem::create_directories(folder);
        assets::BlobStore blobs(folder / ".carto-assets" / "source-blobs");
        auto tile = pixels(
            91, 2, 2,
            {{180, 128, 235, 255}, {128, 180, 235, 255}, {80, 128, 235, 255}, {128, 80, 235, 255}});
        auto cutout = pixels(
            92, 2, 2,
            {{220, 180, 90, 255}, {180, 220, 90, 0}, {90, 180, 220, 0}, {220, 90, 180, 255}});
        auto library = std::make_shared<production::AuthoringTextureLibrary>();
        for (auto *p : {&tile, &cutout}) {
            static_cast<void>(take(blobs.put(p->encoded, "image/x-tga")));
            p->record.relative_locator = blobs.path_for(p->record.content_digest)
                                             .lexically_relative(folder)
                                             .generic_string();
            okay(library->admit(p->record, p->encoded));
        }
        auto document = take(project::ProjectDocument::create("Bounded realism acceptance scene"));
        project::MaterialCatalog materials;
        for (unsigned id = 1; id <= 8; ++id) {
            render::MaterialAsset m;
            m.id = id;
            m.name = "Acceptance material " + std::to_string(id);
            m.definition.base_color_factor = {.55, .55, .55, 1};
            m.definition.roughness = .75;
            if (id == 2) {
                m.definition.base_color_factor = {.45, .08, .03, 1};
                m.definition.roughness = .9;
            }
            if (id == 3) {
                m.definition.base_color_factor = {.8, .65, .35, 1};
                m.definition.metallic = 1;
                m.definition.roughness = .45;
            }
            if (id == 4) {
                m.definition.base_color_factor = {.95, .95, .95, 1};
                m.definition.metallic = 1;
                m.definition.roughness = 0;
            }
            if (id == 5) {
                m.definition.base_color_factor = {.08, .25, .45, 1};
                m.definition.metallic = .4;
                m.definition.roughness = .25;
            }
            if (id == 6) {
                m.definition.normal = {91, 9};
                m.definition.roughness = .3;
            }
            if (id == 7) {
                m.definition.base_color = {92, 9};
                m.definition.alpha_mode = render::AlphaMode::masked;
            }
            if (id == 8) {
                m.definition.base_color_factor = {.01, .01, .01, 1};
                m.definition.emissive_factor = {1, .2, .04};
                m.definition.emissive_strength = 4;
            }
            okay(materials.insert_material(m));
        }
        okay(materials.insert_texture(tile.record));
        okay(materials.insert_texture(cutout.record));
        auto globe = sphere();
        auto box = take(geometry::make_box({1.3, 1.3, 1.3}));
        const auto edges = box.edges_sorted(64);
        const auto score = [&](const auto &edge) {
            const auto a = box.find_vertex(edge.first)->position;
            const auto b = box.find_vertex(edge.second)->position;
            return a.y + b.y + a.z + b.z;
        };
        const auto front_edge =
            std::max_element(edges.begin(), edges.end(),
                             [&](const auto &a, const auto &b) { return score(a) < score(b); });
        static_cast<void>(take(box.bevel_edge(front_edge->id, .08, 3)));
        box = normal_mesh(std::move(box));
        scene::ObjectId camera_id, box_id;
        std::map<std::string, scene::ObjectId> objects;
        auto environment = studio_environment();
        mutate(document, [&](auto &tx) {
            const auto add = [&](const char *name, geometry::EditableMesh mesh,
                                 core::Transform pose, unsigned material) {
                const auto asset = take(tx.add_mesh(std::move(mesh)));
                const auto object = take(tx.create_object(name, pose));
                okay(tx.attach_mesh(object, asset));
                okay(materials.insert_assignment({{asset, 1}, material, {}}));
                objects.emplace(name, object);
                return object;
            };
            add("Neutral floor", normal_mesh(take(geometry::make_plane(24, 20))), {{0, -.85, 0}},
                1);
            const core::Quaternion upright{std::sin(std::numbers::pi / 4), 0, 0,
                                           std::cos(std::numbers::pi / 4)};
            add("Neutral backdrop", normal_mesh(take(geometry::make_plane(30, 18))),
                {{0, 5.5, -4}, upright}, 1);
            add("Matte sphere", globe, {{-2, 0, -.6}}, 2);
            add("Rough metal sphere", globe, {{0, 0, -.6}}, 3);
            add("Mirror sphere", globe, {{2, 0, -.6}}, 4);
            box_id = add("Beveled box", box, {{-2, -.2, 1.7}}, 5);
            add("Normal tile", normal_mesh(take(geometry::make_plane(1.5, 1.5)), false, true),
                {{.6, -.84, 2.3}}, 6);
            add("Masked card", normal_mesh(take(geometry::make_plane(1.4, 1.8)), false, true),
                {{2.7, .1, 1.7}, upright}, 7);
            add("Emissive patch", normal_mesh(take(geometry::make_plane(.6, .6))),
                {{-.6, -.84, 2.4}}, 8);
            const core::Vec3d eye{6, 4, 8};
            camera_id =
                take(tx.create_object("Acceptance camera", {eye, look_at(eye, {0, 0, .2})}));
            const auto d = core::Vec3d{.6, .8, .4}.normalized();
            const core::Quaternion sun{-d.y, d.x, 0, 1 + d.z};
            const auto light_id = take(tx.create_object("Directional key", {{}, sun}));
            project::PresentationCatalog presentation;
            project::AuthoredCamera camera;
            camera.object = camera_id;
            camera.identity = "acceptance.camera";
            camera.bookmark_identity = "acceptance.fixed";
            camera.vertical_fov_radians = .6;
            camera.near_plane = .1;
            camera.far_plane = 50;
            camera.exposure_ev = -1;
            camera.active_for_review = true;
            okay(presentation.insert_camera(camera));
            project::AuthoredLight light;
            light.object = light_id;
            light.identity = "acceptance.key";
            light.kind = project::AuthoredLightKind::directional;
            light.linear_color = {1, .95, .85};
            light.intensity = 4;
            light.range = 0;
            okay(presentation.insert_light(light));
            okay(presentation.set_environment(environment));
            okay(tx.set_material_catalog(materials));
            okay(tx.set_presentation_catalog(presentation));
        });
        production::AuthoringViewportRequest request;
        request.width = 320;
        request.height = 240;
        request.camera = camera_id;
        request.camera_display = true;
        request.shadows.resolution = 512;
        request.textures = library;
        request.environment_lighting = take(production::compile_environment_lighting(environment));
        const auto image = take(production::render_authoring_viewport(document, request));
        std::map<scene::ObjectId, unsigned> coverage;
        double maximum = 0;
        for (std::size_t p = 0; p < image.objects.size(); ++p)
            if (image.objects[p]) {
                ++coverage[image.objects[p]];
                maximum = std::max(
                    {maximum, image.linear_hdr[p].x, image.linear_hdr[p].y, image.linear_hdr[p].z});
            }
        check(coverage.size() >= 7 && maximum > 1,
              "acceptance fixture must render multiple actual objects and HDR values");
        static_cast<void>(take(document.save_atomic(folder / "acceptance-scene.carto")));
        std::ofstream ppm(folder / "acceptance-scene-cpu.ppm", std::ios::binary);
        ppm << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (std::size_t p = 0; p < image.objects.size(); ++p)
            ppm.write(reinterpret_cast<const char *>(&image.display_rgba8[p * 4]), 3);
        std::ofstream receipt(folder / "acceptance-scene-cpu.txt");
        receipt << "CPU reference only; no GPU/native visual acceptance\n"
                << "resolution " << image.width << 'x' << image.height
                << " shadow512\nmaximum HDR channel " << maximum << "\nhost ms "
                << image.host_milliseconds << '\n';
        for (const auto &[name, id] : objects)
            receipt << name << " #" << id.value << " visible pixels " << coverage[id] << '\n';
        receipt << "moving occluder source object #" << box_id.value << "; camera #"
                << camera_id.value << '\n';
        std::cout << "CPU acceptance fixture saved; " << coverage.size()
                  << " visible objects, max HDR " << maximum
                  << "; GPU/native checks remain pending\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
