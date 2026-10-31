#include <carto/d3d12/scene_renderer.hpp>
#include <carto/project/project.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
template <typename T> T take(carto::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
template <typename T> T number(std::string_view input) {
    T value{}; const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
        throw std::runtime_error("invalid numeric argument");
    return value;
}
std::string json(std::string_view input) {
    std::string output = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (char character : input) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') { output += '\\'; output += static_cast<char>(byte); }
        else if (byte < 0x20U) { output += "\\u00"; output += hex[byte >> 4U]; output += hex[byte & 15U]; }
        else output += static_cast<char>(byte);
    }
    return output + '"';
}
void u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) output.push_back(static_cast<std::uint8_t>(value >> shift));
}
std::vector<std::uint8_t> bmp(const carto::d3d12::SceneRenderImage& image) {
    std::vector<std::uint8_t> output{'B', 'M'};
    u32(output, 54U + static_cast<std::uint32_t>(image.srgb_rgba.size())); u32(output, 0U); u32(output, 54U);
    u32(output, 40U); u32(output, image.width); u32(output, image.height);
    u32(output, 0x00200001U); u32(output, 0U); u32(output, static_cast<std::uint32_t>(image.srgb_rgba.size()));
    for (unsigned i = 0U; i < 4U; ++i) u32(output, 0U);
    for (std::uint32_t y = image.height; y > 0U; --y)
        for (std::uint32_t x = 0U; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y - 1U) * image.width + x) * 4U;
            output.insert(output.end(), {image.srgb_rgba[offset + 2U], image.srgb_rgba[offset + 1U], image.srgb_rgba[offset], 255U});
        }
    return output;
}
void write(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    output.close(); if (!output) throw std::runtime_error("could not write render artifact");
}
bool path_entry_exists(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) throw std::runtime_error("could not inspect render output path: " + error.message());
    return status.type() != std::filesystem::file_type::not_found;
}

std::filesystem::path create_staging_directory(const std::filesystem::path& output_path) {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto parent = output_path.parent_path().empty()
        ? std::filesystem::path(".") : output_path.parent_path();
    for (unsigned attempt = 0U; attempt < 32U; ++attempt) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto suffix = std::string(".pending-") + std::to_string(tick) + "-" +
            std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
        const auto staging = parent / (output_path.filename().string() + suffix);
        std::error_code error;
        if (std::filesystem::create_directory(staging, error)) return staging;
        if (error && error != std::errc::file_exists)
            throw std::runtime_error("could not create render staging directory: " + error.message());
    }
    throw std::runtime_error("could not reserve a unique render staging directory");
}

struct StagingDirectoryCleanup {
    std::filesystem::path path;
    std::filesystem::path image_path;
    std::filesystem::path receipt_path;
    StagingDirectoryCleanup(std::filesystem::path directory,
        std::filesystem::path image, std::filesystem::path receipt)
        : path(std::move(directory)), image_path(std::move(image)),
          receipt_path(std::move(receipt)) {}
    StagingDirectoryCleanup(const StagingDirectoryCleanup&) = delete;
    StagingDirectoryCleanup& operator=(const StagingDirectoryCleanup&) = delete;
    ~StagingDirectoryCleanup() {
        std::error_code ignored;
        std::filesystem::remove(image_path, ignored);
        ignored.clear();
        std::filesystem::remove(receipt_path, ignored);
        ignored.clear();
        std::filesystem::remove(path, ignored);
    }
};

void publish_capture(const std::filesystem::path& output_path,
    const std::filesystem::path& receipt_path, std::span<const std::uint8_t> image,
    std::span<const std::uint8_t> receipt) {
    const auto staging_path = create_staging_directory(output_path);
    StagingDirectoryCleanup cleanup{staging_path,
        staging_path / output_path.filename(), staging_path / receipt_path.filename()};
    write(cleanup.image_path, image);
    write(cleanup.receipt_path, receipt);
    if (path_entry_exists(output_path) || path_entry_exists(receipt_path))
        throw std::runtime_error("render output or receipt appeared before publication");

    std::filesystem::rename(cleanup.receipt_path, receipt_path);
    try {
        // Publish the BMP last so its presence means the complete receipt was written.
        std::filesystem::rename(cleanup.image_path, output_path);
    } catch (const std::exception& publish_error) {
        std::error_code rollback_error;
        static_cast<void>(std::filesystem::remove(receipt_path, rollback_error));
        if (rollback_error)
            throw std::runtime_error("BMP publication failed and the receipt could not be rolled back: " +
                std::string(publish_error.what()));
        throw;
    }
}

constexpr std::uint64_t kMaxSuppliedMipArtifactBytes = 512ULL * 1024ULL * 1024ULL;

std::optional<carto::project::MaterialSurfaceInput> graph_surface_input(
    std::string_view value) {
    using carto::project::MaterialSurfaceInput;
    if (value == "curvature") return MaterialSurfaceInput::curvature;
    if (value == "thickness") return MaterialSurfaceInput::thickness;
    if (value == "contact") return MaterialSurfaceInput::contact;
    if (value == "weathering") return MaterialSurfaceInput::weathering;
    if (value == "edge_process") return MaterialSurfaceInput::edge_process;
    if (value == "surface_domain") return MaterialSurfaceInput::surface_domain;
    if (value == "material_stratigraphy") return MaterialSurfaceInput::material_stratigraphy;
    if (value == "authored_mask") return MaterialSurfaceInput::authored_mask;
    return std::nullopt;
}
}

int main(int argc, char** argv) {
    using namespace carto;
    try {
        if (argc < 3) throw std::runtime_error(
            "usage: cartographer_scene_render input.carto output.bmp --identity stable-id --shaders directory "
            "(--camera px py pz tx ty tz [--projection perspective|orthographic] "
            "[--orthographic-height world-units] | --authored-camera object-id) "
            "--size width height "
            "[--exposure EV] [--light dx dy dz r g b] "
            "[--camera-up ux uy uz] [--vertical-fov radians] [--clip-range near far] "
            "[--gpu-timing] "
            "[--antialias fxaa|none] "
            "[--graph-parameter graph-id name scalar|color|normal values...] "
            "[--graph-surface-input graph-id input-name scalar|color|normal values...] "
            "[--texture id exact-current-source-mip-artifact] [--diffuse-irradiance-sh file] "
            "[--environment-equirectangular png|jpg|tga|Radiance-HDR --environment-color-space linear|srgb (HDR requires linear)]");
        const std::filesystem::path input_path = argv[1], output_path = argv[2];
        const std::filesystem::path receipt_path = output_path.string() + ".receipt.json";
        if (output_path.extension() != ".bmp" || path_entry_exists(output_path) ||
            path_entry_exists(receipt_path))
            throw std::runtime_error("output must be a new .bmp path with no existing receipt");
        std::filesystem::path shader_directory;
        std::optional<std::filesystem::path> environment_sh_path;
        std::optional<std::filesystem::path> environment_image_path;
        std::optional<assets::ColorSpace> environment_image_color_space;
        std::string identity;
        d3d12::SceneRenderCamera camera;
        std::optional<scene::ObjectId> authored_camera_object;
        d3d12::SceneRenderAntialiasing antialiasing =
            d3d12::SceneRenderAntialiasing::fxaa;
        render::PbrLight light{{0.3, 0.7, 1.0}, {3.0, 3.0, 3.0}};
        std::uint32_t width = 0U, height = 0U; bool has_camera = false;
        bool has_camera_up = false;
        bool has_vertical_fov = false;
        bool has_clip_range = false;
        bool has_projection_option = false;
        bool has_orthographic_height = false;
        bool capture_gpu_timing = false;
        bool has_gpu_timing_option = false;
        struct SuppliedMipArtifact {
            assets::TextureAssetId asset = 0U;
            std::filesystem::path path;
        };
        std::vector<SuppliedMipArtifact> supplied_mip_artifacts;
        std::map<std::string, project::MaterialGraphEvaluationInputs> pending_graph_inputs;
        const auto read_graph_value = [&](std::string_view type,
                                          const auto& next) {
            project::MaterialGraphValue value;
            std::size_t component_count = 0U;
            if (type == "scalar") {
                value.type = project::MaterialGraphValueType::scalar;
                component_count = 1U;
            } else if (type == "color") {
                value.type = project::MaterialGraphValueType::color;
                component_count = 4U;
            } else if (type == "normal") {
                value.type = project::MaterialGraphValueType::normal;
                component_count = 3U;
            } else {
                throw std::runtime_error(
                    "graph input type must be scalar, color, or normal");
            }
            for (std::size_t component = 0U; component < component_count; ++component)
                value.components[component] = number<double>(next());
            if (auto valid = value.validate(); !valid)
                throw std::runtime_error(valid.error().message);
            return value;
        };
        for (int i = 3; i < argc; ++i) {
            const std::string_view option = argv[i];
            const auto next = [&]() -> std::string_view {
                if (++i >= argc) throw std::runtime_error("option is missing a value"); return argv[i];
            };
            const auto vector = [&]() { const auto x = number<double>(next()); const auto y = number<double>(next());
                const auto z = number<double>(next()); return core::Vec3d{x, y, z}; };
            if (option == "--identity") identity = next();
            else if (option == "--shaders") shader_directory = next();
            else if (option == "--diffuse-irradiance-sh") {
                if (environment_sh_path.has_value())
                    throw std::runtime_error(
                        "--diffuse-irradiance-sh may be supplied only once");
                environment_sh_path = std::filesystem::path(next());
            }
            else if (option == "--environment-equirectangular") {
                if (environment_image_path.has_value())
                    throw std::runtime_error(
                        "--environment-equirectangular may be supplied only once");
                environment_image_path = std::filesystem::path(next());
            }
            else if (option == "--environment-color-space") {
                if (environment_image_color_space.has_value())
                    throw std::runtime_error(
                        "--environment-color-space may be supplied only once");
                const auto value = next();
                if (value == "linear") environment_image_color_space = assets::ColorSpace::linear;
                else if (value == "srgb") environment_image_color_space = assets::ColorSpace::srgb;
                else throw std::runtime_error(
                    "--environment-color-space must be linear or srgb");
            }
            else if (option == "--camera") {
                if (has_camera)
                    throw std::runtime_error("--camera may be supplied only once");
                camera.position = vector();
                camera.target = vector();
                has_camera = true;
            }
            else if (option == "--authored-camera") {
                if (authored_camera_object.has_value())
                    throw std::runtime_error("--authored-camera may be supplied only once");
                const auto object_id = number<std::uint64_t>(next());
                if (object_id == 0U)
                    throw std::runtime_error("--authored-camera requires a nonzero scene object ID");
                authored_camera_object = scene::ObjectId{object_id};
            }
            else if (option == "--camera-up") {
                if (has_camera_up)
                    throw std::runtime_error("--camera-up may be supplied only once");
                camera.up = vector();
                has_camera_up = true;
            }
            else if (option == "--projection") {
                if (has_projection_option)
                    throw std::runtime_error("--projection may be supplied only once");
                const auto value = next();
                if (value == "perspective")
                    camera.projection = d3d12::SceneRenderProjection::perspective;
                else if (value == "orthographic")
                    camera.projection = d3d12::SceneRenderProjection::orthographic;
                else
                    throw std::runtime_error(
                        "--projection must be perspective or orthographic");
                has_projection_option = true;
            }
            else if (option == "--orthographic-height") {
                if (has_orthographic_height)
                    throw std::runtime_error(
                        "--orthographic-height may be supplied only once");
                camera.orthographic_height = number<double>(next());
                has_orthographic_height = true;
            }
            else if (option == "--vertical-fov") {
                if (has_vertical_fov)
                    throw std::runtime_error("--vertical-fov may be supplied only once");
                camera.vertical_fov = number<double>(next());
                has_vertical_fov = true;
            }
            else if (option == "--clip-range") {
                if (has_clip_range)
                    throw std::runtime_error("--clip-range may be supplied only once");
                camera.near_plane = number<double>(next());
                camera.far_plane = number<double>(next());
                has_clip_range = true;
            }
            else if (option == "--gpu-timing") {
                if (has_gpu_timing_option)
                    throw std::runtime_error("--gpu-timing may be supplied only once");
                capture_gpu_timing = true;
                has_gpu_timing_option = true;
            }
            else if (option == "--size") { width = number<std::uint32_t>(next()); height = number<std::uint32_t>(next()); }
            else if (option == "--exposure") camera.exposure_ev = number<double>(next());
            else if (option == "--antialias") {
                const auto value = next();
                if (value == "fxaa") antialiasing = d3d12::SceneRenderAntialiasing::fxaa;
                else if (value == "none") antialiasing = d3d12::SceneRenderAntialiasing::none;
                else throw std::runtime_error("--antialias must be fxaa or none");
            }
            else if (option == "--light") { light.direction_to_light = vector(); light.radiance = vector(); }
            else if (option == "--texture") {
                const auto id = number<std::uint64_t>(next()); const std::filesystem::path path = next();
                if (supplied_mip_artifacts.size() >= 1024U)
                    throw std::runtime_error("more than 1024 mip artifacts were supplied");
                supplied_mip_artifacts.push_back({id, path});
            } else if (option == "--graph-parameter" ||
                       option == "--graph-surface-input") {
                const std::string graph_identity(next());
                if (graph_identity.empty() ||
                    graph_identity.size() > project::MaterialGraph::kMaxIdentityBytes) {
                    throw std::runtime_error("graph input identity is empty or exceeds its bound");
                }
                const std::string key(next());
                const auto type = next();
                const auto value = read_graph_value(type, next);
                if (!pending_graph_inputs.contains(graph_identity) &&
                    pending_graph_inputs.size() >= 1024U) {
                    throw std::runtime_error("more than 1024 material graphs supplied inputs");
                }
                auto& inputs = pending_graph_inputs[graph_identity];
                if (option == "--graph-parameter") {
                    if (!inputs.parameters.emplace(key, value).second)
                        throw std::runtime_error("duplicate graph parameter input");
                } else {
                    const auto surface = graph_surface_input(key);
                    if (!surface.has_value())
                        throw std::runtime_error("unknown graph surface input name");
                    if (!inputs.surface_inputs.emplace(*surface, value).second)
                        throw std::runtime_error("duplicate graph surface input");
                }
            } else throw std::runtime_error("unknown render option");
        }
        if (has_camera == authored_camera_object.has_value())
            throw std::runtime_error(
                "supply exactly one of --camera or --authored-camera");
        if (authored_camera_object.has_value() &&
            (has_camera_up || has_vertical_fov || has_clip_range ||
             has_projection_option || has_orthographic_height)) {
            throw std::runtime_error(
                "--authored-camera uses its saved projection and does not accept camera-up, projection, field-of-view, orthographic-height, or clip-range overrides");
        }
        if ((has_projection_option || has_orthographic_height) && !has_camera)
            throw std::runtime_error(
                "--projection and --orthographic-height require an explicit --camera");
        if (has_orthographic_height && camera.projection !=
                d3d12::SceneRenderProjection::orthographic)
            throw std::runtime_error(
                "--orthographic-height requires --projection orthographic");
        if (identity.empty() || shader_directory.empty() || width == 0U || height == 0U)
            throw std::runtime_error("camera, source identity, shader directory and extent are required");
        if (environment_image_path.has_value() != environment_image_color_space.has_value())
            throw std::runtime_error(
                "--environment-equirectangular requires --environment-color-space");
        if (environment_sh_path.has_value() && environment_image_path.has_value())
            throw std::runtime_error(
                "supply either a diffuse irradiance SH file or an environment image");
        const auto document = take(project::ProjectDocument::load(input_path));
        if (authored_camera_object.has_value()) {
            camera = take(d3d12::resolve_project_scene_render_camera(
                document, *authored_camera_object, camera.exposure_ev));
        }
        d3d12::SceneRenderMaterialGraphInputs graph_inputs;
        struct GraphInputReceipt {
            std::string identity;
            core::Revision revision;
            assets::Sha256Digest graph_digest;
            assets::Sha256Digest inputs_digest;
        };
        std::vector<GraphInputReceipt> graph_input_receipts;
        for (auto& [graph_identity, inputs] : pending_graph_inputs) {
            const auto graph = document.material_catalog().material_graphs().find(
                graph_identity);
            if (graph == document.material_catalog().material_graphs().end())
                throw std::runtime_error("graph input source does not resolve in the project");
            const auto graph_digest = take(graph->second.canonical_digest());
            const auto inputs_digest = take(inputs.canonical_digest());
            graph_input_receipts.push_back({graph_identity, graph->second.revision,
                graph_digest, inputs_digest});
            graph_inputs.emplace(graph_identity,
                d3d12::SceneRenderMaterialGraphInputBinding{
                    graph->second.revision, graph_digest, std::move(inputs)});
        }
        auto textures = take(d3d12::load_project_scene_textures(input_path, document));
        std::set<assets::TextureAssetId> verified_overrides;
        std::uint64_t supplied_mip_artifact_bytes = 0U;
        for (const auto& supplied : supplied_mip_artifacts) {
            const auto current = std::find_if(textures.begin(), textures.end(),
                [&supplied](const auto& texture) { return texture.asset == supplied.asset; });
            if (current == textures.end() ||
                !verified_overrides.insert(supplied.asset).second ||
                supplied.asset == 0U) {
                throw std::runtime_error(
                    "supplied mip artifact does not name one current project texture");
            }
            std::ifstream input(supplied.path, std::ios::binary | std::ios::ate);
            const auto artifact_size = input.tellg();
            if (!input || artifact_size <= 0 ||
                artifact_size > static_cast<std::streamoff>(kMaxSuppliedMipArtifactBytes)) {
                throw std::runtime_error("mip artifact is missing or exceeds capture bounds");
            }
            const auto artifact_bytes = static_cast<std::uint64_t>(artifact_size);
            if (artifact_bytes > kMaxSuppliedMipArtifactBytes - supplied_mip_artifact_bytes) {
                throw std::runtime_error("aggregate mip artifacts exceed capture bounds");
            }
            supplied_mip_artifact_bytes += artifact_bytes;
            std::vector<std::uint8_t> data(static_cast<std::size_t>(artifact_bytes));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!input) throw std::runtime_error("mip artifact read failed");
            auto supplied_mips = take(assets::deserialize_rgba8_mip_chain(data));
            if (supplied_mips.source.digest != current->current_source_digest ||
                supplied_mips.artifact_digest != current->mips.artifact_digest) {
                throw std::runtime_error(
                    "supplied mip artifact does not exactly match the current project texture source");
            }
        }
        auto request = take(d3d12::prepare_project_scene_render(
            identity, document, camera, width, height, std::move(textures), light,
            std::move(graph_inputs)));
        request.antialiasing = antialiasing;
        if (environment_sh_path.has_value())
            request.environment = take(d3d12::load_scene_render_environment_sh(*environment_sh_path));
        else if (environment_image_path.has_value())
            request.environment = take(d3d12::load_scene_render_environment_equirectangular(
                *environment_image_path, *environment_image_color_space));
        const auto device = take(d3d12::D3D12Device::create({{}, false, true}));
        const auto render_started = std::chrono::steady_clock::now();
        const auto image = take(d3d12::render_scene(*device, request, shader_directory,
            capture_gpu_timing));
        const auto render_wall_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - render_started).count();
        if (!device->debug_receipts().empty()) throw std::runtime_error("native device reported debug errors; capture refused");
        const auto encoded = bmp(image);
        const std::size_t host_output_payload_bytes =
            image.linear_hdr_rgba.size() * sizeof(float) +
            image.depth.size() * sizeof(float) + image.srgb_rgba.size() + encoded.size();
        std::ostringstream receipt;
        receipt << std::setprecision(17) << "{\n  \"algorithm\": " << json(d3d12::kSceneRenderAlgorithm) << ",\n"
            << "  \"source_revision\": " << image.source_revision.value() << ",\n  \"source_digest\": " << json(image.source_digest.hex())
            << ",\n  \"request_digest\": " << json(image.request_digest.hex()) << ",\n  \"pixel_digest\": " << json(image.pixel_digest.hex())
            << ",\n  \"environment_digest\": " << json(image.environment_digest.hex())
            << ",\n  \"environment_color_space\": ";
        if (image.environment_color_space == assets::ColorSpace::linear)
            receipt << json("linear");
        else if (image.environment_color_space == assets::ColorSpace::srgb)
            receipt << json("srgb");
        else receipt << "null";
        receipt << ",\n  \"bmp_digest\": " << json(assets::sha256(encoded).hex())
            << ",\n  \"extent\": [" << width << ", " << height
            << "],\n  \"antialiasing\": " << json(image.antialiasing == d3d12::SceneRenderAntialiasing::fxaa ? "fxaa" : "none")
            << ",\n  \"draw_count\": " << image.draw_count
            << ",\n  \"render_wall_time_ns\": " << render_wall_time_ns
            << ",\n  \"host_output_payload_bytes\": " << host_output_payload_bytes
            << ",\n  \"gpu_timing_requested\": " << (capture_gpu_timing ? "true" : "false")
            << ",\n  \"gpu_timing\": ";
        if (image.gpu_timing.has_value()) {
            receipt << "{\"scope\": \"scene_render_graph_passes\", \"elapsed_time_ns\": "
                << image.gpu_timing->elapsed_nanoseconds
                << ", \"timestamp_frequency_hz\": "
                << image.gpu_timing->timestamp_frequency_hz << "}";
        } else {
            receipt << "null";
        }
        receipt << ",\n  \"adapter\": " << json(image.device.adapter_name)
            << ",\n  \"adapter_id\": " << json(image.device.stable_adapter_id) << ",\n  \"device_reported_driver\": " << json(image.device.driver)
            << ",\n  \"camera_source\": ";
        if (authored_camera_object.has_value()) {
            const auto& authored_camera =
                document.presentation_catalog().cameras().at(*authored_camera_object);
            receipt << "{\"kind\": \"authored_project\", \"object_id\": "
                << authored_camera_object->value << ", \"identity\": "
                << json(authored_camera.identity) << ", \"revision\": "
                << authored_camera.revision.value() << "}";
        } else {
            receipt << "{\"kind\": \"explicit\"}";
        }
        receipt << ",\n  \"camera_position\": [" << camera.position.x << ',' << camera.position.y << ',' << camera.position.z
            << "],\n  \"camera_target\": [" << camera.target.x << ',' << camera.target.y << ',' << camera.target.z
            << "],\n  \"camera_up\": [" << camera.up.x << ',' << camera.up.y << ',' << camera.up.z
            << "],\n  \"camera_projection\": "
            << json(camera.projection == d3d12::SceneRenderProjection::orthographic
                ? "orthographic" : "perspective")
            << ",\n  \"vertical_fov\": " << camera.vertical_fov
            << ",\n  \"orthographic_height\": " << camera.orthographic_height
            << ",\n  \"clip_range\": [" << camera.near_plane << ',' << camera.far_plane << "],\n  \"exposure_ev\": " << camera.exposure_ev
            << ",\n  \"light_direction\": [" << request.light.direction_to_light.x << ',' << request.light.direction_to_light.y << ',' << request.light.direction_to_light.z
            << "],\n  \"light_radiance\": [" << request.light.radiance.x << ',' << request.light.radiance.y << ',' << request.light.radiance.z
            << "],\n  \"local_lighting_model\": ";
        if (request.local_lights.empty()) {
            receipt << "null";
        } else {
            receipt << "{\"radiance\": "
                << json(d3d12::kSceneRenderLocalLightRadianceUnits)
                << ", \"distance_attenuation\": "
                << json(d3d12::kSceneRenderLocalLightDistanceAttenuation)
                << ", \"near_field_floor_meters\": "
                << d3d12::kSceneRenderLocalLightDistanceFloorMeters
                << ", \"range_fade\": "
                << json(d3d12::kSceneRenderLocalLightRangeFade)
                << ", \"shadowing\": "
                << json(d3d12::kSceneRenderLocalLightShadowing) << "}";
        }
        receipt << ",\n  \"local_lights\": [";
        for (std::size_t i = 0U; i < request.local_lights.size(); ++i) {
            const auto& local = request.local_lights[i];
            const std::string_view kind =
                local.kind == d3d12::SceneRenderLocalLightKind::point
                ? "point" : "spot";
            receipt << (i == 0U ? "\n    {" : ",\n    {")
                << "\"source_object_id\": " << local.object.value
                << ", \"kind\": " << json(kind)
                << ", \"position_world\": [" << local.position.x << ','
                << local.position.y << ',' << local.position.z << ']'
                << ", \"radiance_linear\": [" << local.radiance.x << ','
                << local.radiance.y << ',' << local.radiance.z << ']'
                << ", \"range_world\": " << local.range
                << ", \"shadowed\": "
                << (local.casts_shadows ? "true" : "false");
            if (local.kind == d3d12::SceneRenderLocalLightKind::spot) {
                receipt << ", \"emission_direction_world\": ["
                    << local.emission_direction.x << ','
                    << local.emission_direction.y << ','
                    << local.emission_direction.z << ']'
                    << ", \"cone_radians\": ["
                    << local.inner_cone_radians << ','
                    << local.outer_cone_radians << ']';
            }
            receipt << '}';
        }
        receipt << "\n  ],\n  \"shader_digests\": [";
        for (std::size_t i = 0U; i < image.shader_digests.size(); ++i) receipt << (i ? "," : "") << json(image.shader_digests[i].hex());
        receipt << "],\n  \"temporal_output_state\": ";
        if (!image.temporal_output_state.has_value()) {
            receipt << "null";
        } else if (*image.temporal_output_state ==
                device_ir::ResourceState::shader_read) {
            receipt << json("shader_read");
        } else if (*image.temporal_output_state ==
                device_ir::ResourceState::copy_source) {
            receipt << json("copy_source");
        } else {
            receipt << json("unknown");
        }
        receipt << ",\n  \"material_graph_inputs\": [";
        for (std::size_t i = 0U; i < graph_input_receipts.size(); ++i) {
            const auto& graph_input = graph_input_receipts[i];
            receipt << (i ? "," : "") << "\n    {\"identity\": "
                << json(graph_input.identity) << ", \"revision\": "
                << graph_input.revision.value() << ", \"graph_digest\": "
                << json(graph_input.graph_digest.hex()) << ", \"inputs_digest\": "
                << json(graph_input.inputs_digest.hex()) << "}";
        }
        receipt << "\n  ],\n  \"render_graph_passes\": [";
        for (std::size_t i = 0U; i < image.render_graph_passes.size(); ++i) receipt << (i ? "," : "") << json(image.render_graph_passes[i]);
        receipt << "],\n  \"limitations\": [";
        for (std::size_t i = 0U; i < image.limitations.size(); ++i) receipt << (i ? "," : "") << json(image.limitations[i]);
        receipt << "]\n}\n";
        const auto receipt_text = receipt.str();
        publish_capture(output_path, receipt_path, encoded,
            {reinterpret_cast<const std::uint8_t*>(receipt_text.data()), receipt_text.size()});
        std::cout << "Rendered " << image.draw_count << " authored draws on " << image.device.adapter_name
            << "; pixel digest=" << image.pixel_digest.hex() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "Scene render refused: " << error.what() << '\n'; return 1; }
}
