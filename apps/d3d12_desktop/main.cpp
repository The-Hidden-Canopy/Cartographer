#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <carto/d3d12/device.hpp>
#include <carto/d3d12/detail/render_graph_dispatch.hpp>
#include <carto/d3d12/frame_scheduler.hpp>
#include <carto/d3d12/scene_camera_fit.hpp>
#include <carto/d3d12/scene_camera_navigation.hpp>
#include <carto/d3d12/scene_renderer.hpp>
#include <carto/d3d12/temporal_sampling.hpp>
#include <carto/d3d12/swapchain.hpp>
#include <carto/d3d12/viewport_input.hpp>
#include <carto/device_ir/command.hpp>
#include <carto/project/project.hpp>
#include <carto/render_graph/graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <locale>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"CartographerD3D12Window";
constexpr std::uint32_t kTemporalPreviewSampleLimit =
    carto::d3d12::temporal_sample_limit;

struct DesktopArguments {
    std::optional<std::filesystem::path> project_path;
    std::optional<std::filesystem::path> shader_directory;
    std::optional<std::filesystem::path> environment_sh_path;
    std::optional<std::filesystem::path> environment_image_path;
    std::optional<carto::assets::ColorSpace> environment_image_color_space;
    bool enable_debug_layer = false;
};

struct PreparedDesktopScene {
    carto::d3d12::SceneRenderRequest request;
    double camera_distance = 5.0;
    double frame_radius = 0.05;
};

struct WindowState {
    std::unique_ptr<carto::d3d12::D3D12Device> device;
    std::unique_ptr<carto::d3d12::D3D12Swapchain> swapchain;
    carto::d3d12::SceneRenderCache scene_render_cache;
    std::optional<carto::d3d12::SceneRenderRequest> scene_request;
    carto::d3d12::SceneRenderCamera scene_camera_base;
    std::optional<carto::gpu::TextureHandle> scene_texture;
    std::uint32_t scene_texture_width = 0U;
    std::uint32_t scene_texture_height = 0U;
    bool scene_texture_initialized = false;
    std::array<carto::gpu::TextureHandle, 2U> temporal_textures{};
    std::array<carto::device_ir::ResourceState, 2U> temporal_texture_states{
        carto::device_ir::ResourceState::undefined,
        carto::device_ir::ResourceState::undefined};
    std::uint32_t temporal_texture_width = 0U;
    std::uint32_t temporal_texture_height = 0U;
    std::uint32_t temporal_sample_count = 0U;
    std::uint32_t temporal_history_slot = 0U;
    bool temporal_history_valid = false;
    bool temporal_preview_enabled = false;
    carto::assets::Sha256Digest temporal_scene_identity;
    carto::assets::Sha256Digest temporal_history_content_digest;
    carto::assets::Sha256Digest temporal_history_request_digest;
    carto::core::Revision temporal_history_source_revision;
    carto::assets::Sha256Digest temporal_history_source_digest;
    carto::core::Vec2d temporal_history_jitter{};
    std::filesystem::path shader_directory;
    double camera_distance = 5.0;
    double scene_frame_radius = 0.05;
    bool scene_dirty = false;
    carto::d3d12::ViewportInputState input;
    std::optional<std::string> window_error;
};

bool parse_desktop_arguments(DesktopArguments& arguments, std::string& error) {
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr) {
        error = "Unable to read the D3D12 desktop command line.";
        return false;
    }
    bool valid = true;
    for (int index = 1; index < count && valid; ++index) {
        const std::wstring_view option(values[index]);
        if (option == L"--debug-layer") {
            arguments.enable_debug_layer = true;
            continue;
        }
        if (option != L"--project" && option != L"--shaders" &&
            option != L"--diffuse-irradiance-sh" &&
            option != L"--environment-equirectangular" &&
            option != L"--environment-color-space") {
            error = "Usage: cartographer_d3d12_desktop [--debug-layer] "
                    "[--project file.carto --shaders directory "
                    "[--diffuse-irradiance-sh file | "
                    "--environment-equirectangular image-or-radiance-hdr "
                    "--environment-color-space linear|srgb]]";
            valid = false;
            break;
        }
        if (++index >= count) {
            error = "A D3D12 desktop option is missing its value.";
            valid = false;
            break;
        }
        if (option == L"--project") {
            if (arguments.project_path.has_value()) {
                error = "The --project option may be supplied only once.";
                valid = false;
            } else {
                arguments.project_path = std::filesystem::path(values[index]);
            }
        } else if (option == L"--shaders" && arguments.shader_directory.has_value()) {
            error = "The --shaders option may be supplied only once.";
            valid = false;
        } else if (option == L"--diffuse-irradiance-sh" &&
                   arguments.environment_sh_path.has_value()) {
            error = "The --diffuse-irradiance-sh option may be supplied only once.";
            valid = false;
        } else if (option == L"--diffuse-irradiance-sh") {
            arguments.environment_sh_path = std::filesystem::path(values[index]);
        } else if (option == L"--environment-equirectangular" &&
                   arguments.environment_image_path.has_value()) {
            error = "The --environment-equirectangular option may be supplied only once.";
            valid = false;
        } else if (option == L"--environment-equirectangular") {
            arguments.environment_image_path = std::filesystem::path(values[index]);
        } else if (option == L"--environment-color-space" &&
                   arguments.environment_image_color_space.has_value()) {
            error = "The --environment-color-space option may be supplied only once.";
            valid = false;
        } else if (option == L"--environment-color-space") {
            const std::wstring_view value(values[index]);
            if (value == L"linear") {
                arguments.environment_image_color_space = carto::assets::ColorSpace::linear;
            } else if (value == L"srgb") {
                arguments.environment_image_color_space = carto::assets::ColorSpace::srgb;
            } else {
                error = "The environment color space must be linear or srgb.";
                valid = false;
            }
        } else {
            arguments.shader_directory = std::filesystem::path(values[index]);
        }
    }
    static_cast<void>(LocalFree(values));
    if (!valid) return false;
    if (arguments.project_path.has_value() != arguments.shader_directory.has_value()) {
        error = "Project viewing requires both --project and --shaders.";
        return false;
    }
    if (arguments.environment_image_path.has_value() !=
        arguments.environment_image_color_space.has_value()) {
        error = "Environment image viewing requires an explicit linear or srgb color space.";
        return false;
    }
    if (arguments.environment_sh_path.has_value() &&
        arguments.environment_image_path.has_value()) {
        error = "Supply either a diffuse irradiance SH file or an environment image.";
        return false;
    }
    if ((arguments.environment_sh_path.has_value() ||
         arguments.environment_image_path.has_value()) &&
        !arguments.project_path.has_value()) {
        error = "Environment lighting requires source-backed project viewing.";
        return false;
    }
    return true;
}

carto::core::Result<PreparedDesktopScene> prepare_desktop_scene(
    const std::filesystem::path& project_path,
    std::uint32_t width,
    std::uint32_t height,
    const std::optional<std::filesystem::path>& environment_sh_path,
    const std::optional<std::filesystem::path>& environment_image_path,
    const std::optional<carto::assets::ColorSpace>& environment_image_color_space) {
    using Output = carto::core::Result<PreparedDesktopScene>;
    const auto document = carto::project::ProjectDocument::load(project_path);
    if (!document) return Output::failure(document.error());
    const auto& source_namespace = document.value().source_namespace();
    const auto namespace_digest = carto::assets::sha256({
        reinterpret_cast<const std::uint8_t*>(source_namespace.data()), source_namespace.size()});
    // Keep derived artifact identities bounded; the scene request separately
    // carries the full project revision and content digest.
    const std::string identity = "desktop-" + namespace_digest.hex().substr(0U, 48U);
    auto textures = carto::d3d12::load_project_scene_textures(project_path, document.value());
    if (!textures) return Output::failure(textures.error());
    auto request = carto::d3d12::prepare_project_scene_render(
        identity, document.value(), {}, width, height, std::move(textures.value()));
    if (!request) return Output::failure(request.error());
    if (environment_sh_path.has_value()) {
        auto environment = carto::d3d12::load_scene_render_environment_sh(*environment_sh_path);
        if (!environment) return Output::failure(environment.error());
        request.value().environment = std::move(environment.value());
    } else if (environment_image_path.has_value() &&
               environment_image_color_space.has_value()) {
        auto environment = carto::d3d12::load_scene_render_environment_equirectangular(
            *environment_image_path, *environment_image_color_space);
        if (!environment) return Output::failure(environment.error());
        request.value().environment = std::move(environment.value());
    }

    carto::core::Vec3d minimum{
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    carto::core::Vec3d maximum{
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (const auto& object : request.value().objects) {
        for (const auto& vertex : object.mesh.vertices) {
            const auto world = object.world_transform.translation +
                object.world_transform.rotation.rotate(carto::core::componentwise_multiply(
                    object.world_transform.scale, vertex.position));
            if (!world.finite()) {
                return Output::failure({carto::core::ErrorCode::validation_failed,
                    "D3D12 scene bounds contain a non-finite world vertex."});
            }
            minimum.x = std::min(minimum.x, world.x);
            minimum.y = std::min(minimum.y, world.y);
            minimum.z = std::min(minimum.z, world.z);
            maximum.x = std::max(maximum.x, world.x);
            maximum.y = std::max(maximum.y, world.y);
            maximum.z = std::max(maximum.z, world.z);
        }
    }
    if (request.value().objects.empty() || !minimum.finite() || !maximum.finite()) {
        return Output::failure({carto::core::ErrorCode::not_found,
            "The saved project contains no visible mesh scene to display."});
    }
    const auto center = (minimum + maximum) * 0.5;
    const auto half_extent = (maximum - minimum) * 0.5;
    const double radius = std::max(half_extent.length(), 0.05);
    auto& camera = request.value().camera;
    camera.target = center;
    camera.vertical_fov = 0.7853981633974483;
    const auto fitted_distance = carto::d3d12::fit_camera_distance_to_sphere(
        radius, camera.vertical_fov, width, height);
    if (!fitted_distance) {
        return Output::failure({carto::core::ErrorCode::validation_failed,
            "The saved scene cannot be fitted to the requested viewport extent."});
    }
    const double camera_distance = *fitted_distance;
    camera.position = center + carto::core::Vec3d{0.0, 0.0, camera_distance};
    camera.near_plane = std::max(0.0001, radius / 16000.0);
    camera.far_plane = camera_distance * 8.0 + radius * 2.0 + 1.0;
    if (auto valid = request.value().validate(); !valid) {
        return Output::failure(valid.error());
    }
    return Output::success(PreparedDesktopScene{
        std::move(request.value()), camera_distance, radius});
}

carto::gpu::TextureDesc scene_target_descriptor(
    std::uint32_t width,
    std::uint32_t height) {
    carto::gpu::TextureDesc texture_desc;
    texture_desc.width = width;
    texture_desc.height = height;
    texture_desc.format = carto::gpu::Format::bgra8_unorm;
    texture_desc.usage = carto::gpu::TextureUsage::color_attachment |
        carto::gpu::TextureUsage::transfer_source;
    return texture_desc;
}

carto::core::Result<carto::gpu::TextureHandle> create_scene_target(
    carto::d3d12::D3D12Device& device,
    const carto::gpu::TextureDesc& descriptor) {
    return device.create_texture(descriptor);
}

carto::gpu::TextureDesc temporal_target_descriptor(
    std::uint32_t width,
    std::uint32_t height) {
    carto::gpu::TextureDesc descriptor;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = carto::gpu::Format::rgba32_float;
    descriptor.usage = carto::gpu::TextureUsage::storage |
        carto::gpu::TextureUsage::sampled |
        carto::gpu::TextureUsage::transfer_source;
    return descriptor;
}

bool temporal_preview_fits_memory_budget(
    std::uint32_t width,
    std::uint32_t height) noexcept {
    return carto::d3d12::temporal_history_textures_fit_budget(width, height);
}

carto::core::Vec2d temporal_sample_jitter(std::uint32_t completed_samples) noexcept {
    return carto::d3d12::temporal_sample_jitter(completed_samples);
}

carto::core::Result<carto::assets::Sha256Digest> temporal_texture_digest(
    carto::d3d12::D3D12Device& device,
    carto::gpu::TextureHandle texture,
    std::uint32_t width,
    std::uint32_t height) {
    using Output = carto::core::Result<carto::assets::Sha256Digest>;
    auto pixels = device.read_texture_rgba32f(texture);
    if (!pixels) return Output::failure(pixels.error());
    const std::uint64_t expected_values =
        static_cast<std::uint64_t>(width) * height * 4U;
    if (expected_values != pixels.value().size() ||
        pixels.value().size() > std::numeric_limits<std::size_t>::max() /
            sizeof(float) ||
        !std::all_of(pixels.value().begin(), pixels.value().end(),
            [](float value) { return std::isfinite(value); })) {
        return Output::failure({carto::core::ErrorCode::validation_failed,
            "Temporal history readback has an invalid extent or non-finite pixels."});
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(pixels.value().data());
    const std::span<const std::uint8_t> content{
        bytes, pixels.value().size() * sizeof(float)};
    return Output::success(carto::assets::sha256(content));
}

void reset_temporal_history(WindowState& window_state) noexcept {
    window_state.temporal_sample_count = 0U;
    window_state.temporal_history_slot = 0U;
    window_state.temporal_history_valid = false;
    window_state.temporal_history_content_digest = {};
    window_state.temporal_history_request_digest = {};
    window_state.temporal_history_source_revision = {};
    window_state.temporal_history_source_digest = {};
    window_state.temporal_history_jitter = {};
}

carto::core::Result<void> release_temporal_textures(
    WindowState& window_state,
    carto::d3d12::D3D12Device& device) {
    for (auto& texture : window_state.temporal_textures) {
        if (!texture) continue;
        auto released = device.destroy_texture(texture);
        if (!released) return released;
        texture = {};
    }
    window_state.temporal_texture_states.fill(
        carto::device_ir::ResourceState::undefined);
    window_state.temporal_texture_width = 0U;
    window_state.temporal_texture_height = 0U;
    window_state.temporal_scene_identity = {};
    reset_temporal_history(window_state);
    return carto::core::Result<void>::success();
}

carto::core::Result<void> record_frame_graph(
    carto::device_ir::DeviceCommandStream& stream,
    carto::gpu::TextureHandle back_buffer,
    carto::gpu::Format back_buffer_format,
    std::uint32_t width,
    std::uint32_t height,
    std::optional<carto::gpu::TextureHandle> scene_texture) {
    using namespace carto;
    using Output = core::Result<void>;
    using render_graph::Access;
    using render_graph::PassDesc;
    using render_graph::PipelineStage;
    using render_graph::ResourceDesc;
    using render_graph::ResourceId;
    using render_graph::ResourceUsage;

    render_graph::GraphBuilder builder;
    const auto add_texture = [&](std::string name, gpu::Format format) {
        ResourceDesc descriptor;
        descriptor.name = std::move(name);
        descriptor.kind = render_graph::ResourceKind::texture;
        descriptor.imported = true;
        descriptor.width = width;
        descriptor.height = height;
        descriptor.depth = 1U;
        descriptor.layers = 1U;
        descriptor.mip_levels = 1U;
        descriptor.format = format;
        return builder.create_resource(std::move(descriptor));
    };

    const auto graph_back_buffer = add_texture("desktop.back-buffer", back_buffer_format);
    if (!graph_back_buffer) return Output::failure(graph_back_buffer.error());
    std::optional<ResourceId> graph_scene;
    if (scene_texture.has_value()) {
        const auto added = add_texture("desktop.authored-scene", gpu::Format::bgra8_unorm);
        if (!added) return Output::failure(added.error());
        graph_scene = added.value();
    }

    std::optional<render_graph::PassId> copy_scene_pass;
    std::optional<render_graph::PassId> clear_background_pass;
    if (scene_texture.has_value()) {
        const auto pass = builder.add_pass({"desktop.copy-authored-scene", {
            ResourceUsage{*graph_scene, Access::read, PipelineStage::copy},
            ResourceUsage{graph_back_buffer.value(), Access::write, PipelineStage::copy}}});
        if (!pass) return Output::failure(pass.error());
        copy_scene_pass = pass.value();
    } else {
        const auto pass = builder.add_pass({"desktop.clear-background", {
            ResourceUsage{graph_back_buffer.value(), Access::write, PipelineStage::color_output}}});
        if (!pass) return Output::failure(pass.error());
        clear_background_pass = pass.value();
    }
    const auto present_pass = builder.add_pass({"desktop.present", {
        ResourceUsage{graph_back_buffer.value(), Access::write, PipelineStage::present}}});
    if (!present_pass) return Output::failure(present_pass.error());

    auto compiled = builder.compile();
    if (!compiled) return Output::failure(compiled.error());
    std::vector<render_graph::PassId> expected_order;
    if (copy_scene_pass) expected_order.push_back(*copy_scene_pass);
    if (clear_background_pass) expected_order.push_back(*clear_background_pass);
    expected_order.push_back(present_pass.value());
    if (compiled.value().pass_order != expected_order) {
        return Output::failure({core::ErrorCode::invalid_state,
            "D3D12 desktop graph compiled to an unexpected frame pass order"});
    }

    std::map<ResourceId, gpu::TextureHandle> textures{
        {graph_back_buffer.value(), back_buffer}};
    std::map<ResourceId, device_ir::ResourceState> states{
        {graph_back_buffer.value(), device_ir::ResourceState::present}};
    if (scene_texture.has_value()) {
        textures.emplace(*graph_scene, *scene_texture);
        states.emplace(*graph_scene, device_ir::ResourceState::copy_source);
    }
    std::map<render_graph::PassId, PassDesc> passes;
    if (copy_scene_pass) passes.emplace(*copy_scene_pass, PassDesc{
        "desktop.copy-authored-scene", {
            ResourceUsage{*graph_scene, Access::read, PipelineStage::copy},
            ResourceUsage{graph_back_buffer.value(), Access::write, PipelineStage::copy}}});
    if (clear_background_pass) passes.emplace(*clear_background_pass, PassDesc{
        "desktop.clear-background", {
            ResourceUsage{graph_back_buffer.value(), Access::write, PipelineStage::color_output}}});
    passes.emplace(present_pass.value(), PassDesc{"desktop.present", {
        ResourceUsage{graph_back_buffer.value(), Access::write, PipelineStage::present}}});

    d3d12::detail::RenderGraphDispatch::ResourceBindings graph_bindings;
    for (const auto& [resource, handle] : textures) {
        graph_bindings.emplace(resource, device_ir::ResourceHandle{handle});
    }
    auto graph_dispatch = d3d12::detail::RenderGraphDispatch::create(
        std::move(compiled).value(), std::move(passes), std::move(graph_bindings),
        std::move(states));
    if (!graph_dispatch) return Output::failure(graph_dispatch.error());

    for (const auto pass_id : expected_order) {
        const auto dispatched = graph_dispatch.value().record_pass(pass_id, stream);
        if (!dispatched) return Output::failure(dispatched.error());

        if (copy_scene_pass && pass_id == *copy_scene_pass) {
            stream.append(device_ir::CmdCopyTexture{*scene_texture, back_buffer});
        } else if (clear_background_pass && pass_id == *clear_background_pass) {
            stream.append(device_ir::CmdBeginRendering{back_buffer, std::nullopt});
            stream.append(device_ir::CmdClearColor{
                back_buffer, {0.035F, 0.045F, 0.065F, 1.0F}});
            stream.append(device_ir::CmdEndRendering{});
        } else if (pass_id != present_pass.value()) {
            return Output::failure({core::ErrorCode::invalid_state,
                "D3D12 desktop graph pass has no frame command implementation"});
        }
    }
    if (const auto finished = graph_dispatch.value().finish(); !finished)
        return Output::failure(finished.error());
    return stream.validate();
}

WindowState* state(HWND window) noexcept {
    return reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void update_navigation_title(HWND window) {
    const auto* window_state = state(window);
    if (window_state == nullptr) return;
    const auto& input = window_state->input;
    std::wostringstream title;
    title.imbue(std::locale::classic());
    title << L"Cartographer D3D12 | Left drag: orbit, middle drag: pan, arrows: nudge, wheel: zoom, Home: frame, F2: AA | "
          << (window_state->scene_request.has_value() &&
              window_state->scene_request->antialiasing ==
                  carto::d3d12::SceneRenderAntialiasing::fxaa
              ? L"FXAA on | yaw " : L"FXAA off | yaw ")
          << std::fixed << std::setprecision(1) << input.yaw_degrees()
          << L"° pitch " << input.pitch_degrees()
          << L"° zoom " << input.zoom()
          << L"x | DPI " << input.dpi()
          << L" (" << std::setprecision(2) << input.dpi_scale() << L"x)";
    if (window_state->scene_request.has_value()) {
        if (window_state->temporal_preview_enabled) {
            title << L" | temporal " << window_state->temporal_sample_count
                  << L"/" << kTemporalPreviewSampleLimit;
        } else {
            title << L" | temporal off (memory bound)";
        }
    }
    const std::wstring text = title.str();
    static_cast<void>(SetWindowTextW(window, text.c_str()));
}

std::optional<carto::d3d12::ViewportNavigationKey> navigation_key(WPARAM key) noexcept {
    using carto::d3d12::ViewportNavigationKey;
    switch (key) {
    case VK_LEFT: return ViewportNavigationKey::yaw_left;
    case VK_RIGHT: return ViewportNavigationKey::yaw_right;
    case VK_UP: return ViewportNavigationKey::pitch_up;
    case VK_DOWN: return ViewportNavigationKey::pitch_down;
    case VK_ADD:
    case VK_OEM_PLUS: return ViewportNavigationKey::zoom_in;
    case VK_SUBTRACT:
    case VK_OEM_MINUS: return ViewportNavigationKey::zoom_out;
    case VK_HOME: return ViewportNavigationKey::frame_all;
    default: return std::nullopt;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* window_state = state(window);
    switch (message) {
    case WM_SIZE:
        if (window_state != nullptr && window_state->swapchain != nullptr &&
            w_param != SIZE_MINIMIZED) {
            RECT client{};
            if (GetClientRect(window, &client) == 0) {
                window_state->window_error = "GetClientRect failed while resizing the D3D12 swapchain";
                return 0;
            }
            const auto width = static_cast<std::uint32_t>(client.right - client.left);
            const auto height = static_cast<std::uint32_t>(client.bottom - client.top);
            if (width != 0U && height != 0U) {
                const auto resized = window_state->swapchain->resize(width, height);
                if (!resized) {
                    window_state->window_error = resized.error().message;
                } else if (window_state->scene_request.has_value()) {
                    auto& request = *window_state->scene_request;
                    const auto fitted_distance =
                        carto::d3d12::fit_camera_distance_to_sphere(
                            window_state->scene_frame_radius,
                            request.camera.vertical_fov, width, height);
                    if (!fitted_distance) {
                        window_state->window_error =
                            "The saved scene camera cannot be fitted after resize.";
                    } else {
                        request.width = width;
                        request.height = height;
                        window_state->temporal_preview_enabled =
                            temporal_preview_fits_memory_budget(width, height);
                        window_state->camera_distance = *fitted_distance;
                        request.camera.far_plane = *fitted_distance * 8.0 +
                            window_state->scene_frame_radius * 2.0 + 1.0;
                        window_state->scene_camera_base.far_plane =
                            request.camera.far_plane;
                        window_state->scene_dirty = true;
                    }
                }
            }
            update_navigation_title(window);
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(l_param);
        if (suggested == nullptr) return DefWindowProcW(window, message, w_param, l_param);
        if (window_state != nullptr &&
            !window_state->input.set_dpi(static_cast<std::uint32_t>(LOWORD(w_param)))) {
            window_state->window_error = "WM_DPICHANGED supplied an unsupported DPI value";
            return 0;
        }
        if (SetWindowPos(window, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE) == 0 && window_state != nullptr) {
            window_state->window_error = "SetWindowPos failed while applying WM_DPICHANGED bounds";
        }
        if (window_state != nullptr && window_state->input.end_drag() &&
            GetCapture() == window) {
            // Client pointer coordinates change scale while moving between
            // monitors; cancel the drag so the next delta cannot jump.
            ReleaseCapture();
        }
        update_navigation_title(window);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (window_state != nullptr) {
            SetCapture(window);
            if (GetCapture() == window) {
                static_cast<void>(window_state->input.begin_orbit(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)));
                update_navigation_title(window);
            }
            return 0;
        }
        break;
    case WM_MBUTTONDOWN:
        if (window_state != nullptr) {
            SetCapture(window);
            if (GetCapture() == window) {
                static_cast<void>(window_state->input.begin_pan(
                    GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)));
                update_navigation_title(window);
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (window_state != nullptr && window_state->input.dragging() &&
            window_state->input.move_pointer(
                GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param))) {
            window_state->scene_dirty = window_state->scene_request.has_value();
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (window_state != nullptr && window_state->input.end_orbit()) {
            if (GetCapture() == window) ReleaseCapture();
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_MBUTTONUP:
        if (window_state != nullptr && window_state->input.end_pan()) {
            if (GetCapture() == window) ReleaseCapture();
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (window_state != nullptr && window_state->input.end_drag()) {
            update_navigation_title(window);
        }
        break;
    case WM_MOUSEWHEEL:
        if (window_state != nullptr && window_state->input.zoom_wheel(
                GET_WHEEL_DELTA_WPARAM(w_param))) {
            window_state->scene_dirty = window_state->scene_request.has_value();
            update_navigation_title(window);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (window_state != nullptr) {
            if (w_param == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            if (w_param == VK_F2 && window_state->scene_request.has_value()) {
                const bool is_repeat = (static_cast<std::uint64_t>(l_param) &
                    (std::uint64_t{1U} << 30U)) != 0U;
                if (!is_repeat) {
                    auto& antialiasing = window_state->scene_request->antialiasing;
                    antialiasing = antialiasing ==
                            carto::d3d12::SceneRenderAntialiasing::fxaa
                        ? carto::d3d12::SceneRenderAntialiasing::none
                        : carto::d3d12::SceneRenderAntialiasing::fxaa;
                    window_state->scene_dirty = true;
                    update_navigation_title(window);
                }
                return 0;
            }
            const auto action = navigation_key(w_param);
            if (action.has_value()) {
                const bool is_repeat = (static_cast<std::uint64_t>(l_param) &
                    (std::uint64_t{1U} << 30U)) != 0U;
                if (!is_repeat && window_state->input.adjust(*action)) {
                    window_state->scene_dirty = window_state->scene_request.has_value();
                    update_navigation_title(window);
                }
                return 0;
            }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        const DWORD error = GetLastError();
        const bool already_per_monitor_v2 = error == ERROR_ACCESS_DENIED &&
            AreDpiAwarenessContextsEqual(
                GetThreadDpiAwarenessContext(),
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0;
        if (!already_per_monitor_v2) {
            MessageBoxW(nullptr,
                L"Cartographer D3D12 requires per-monitor DPI awareness v2.",
                L"Cartographer D3D12", MB_ICONERROR | MB_OK);
            return 1;
        }
    }

    DesktopArguments arguments;
    std::string argument_error;
    if (!parse_desktop_arguments(arguments, argument_error)) {
        MessageBoxA(nullptr, argument_error.c_str(),
                    "Cartographer D3D12 arguments", MB_ICONERROR | MB_OK);
        return 1;
    }

    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    if (RegisterClassW(&window_class) == 0U) return 1;

    HWND window = CreateWindowExW(
        0U,
        kWindowClass,
        L"Cartographer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1280,
        800,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) return 1;

    RECT initial_client{};
    if (GetClientRect(window, &initial_client) == 0 ||
        initial_client.right <= initial_client.left ||
        initial_client.bottom <= initial_client.top) {
        MessageBoxW(window, L"The initial D3D12 client area is invalid.",
                    L"Cartographer D3D12", MB_ICONERROR | MB_OK);
        DestroyWindow(window);
        return 1;
    }
    const auto initial_width = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(initial_client.right) - initial_client.left);
    const auto initial_height = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(initial_client.bottom) - initial_client.top);

    carto::d3d12::D3D12DeviceOptions device_options;
    device_options.enable_debug_layer = arguments.enable_debug_layer;
    auto device = carto::d3d12::D3D12Device::create(device_options);
    if (!device) {
        MessageBoxA(window, device.error().message.c_str(), "Cartographer D3D12", MB_ICONERROR);
        return 1;
    }
    WindowState window_state;
    window_state.device.reset(device.value().release());
    auto swapchain = carto::d3d12::D3D12Swapchain::create(
        *window_state.device,
        carto::d3d12::D3D12SwapchainOptions{
            reinterpret_cast<std::uintptr_t>(window), initial_width, initial_height, 2U,
            carto::gpu::Format::bgra8_unorm, false});
    if (!swapchain) {
        MessageBoxA(window, swapchain.error().message.c_str(), "Cartographer D3D12", MB_ICONERROR);
        return 1;
    }
    window_state.swapchain.reset(swapchain.value().release());
    if (arguments.project_path.has_value() && arguments.shader_directory.has_value()) {
        auto prepared = prepare_desktop_scene(
            *arguments.project_path, initial_width, initial_height,
            arguments.environment_sh_path, arguments.environment_image_path,
            arguments.environment_image_color_space);
        if (!prepared) {
            MessageBoxA(window, prepared.error().message.c_str(),
                        "Cartographer D3D12 scene", MB_ICONERROR | MB_OK);
            return 1;
        }
        window_state.scene_request.emplace(
            std::move(prepared.value().request));
        window_state.scene_camera_base = window_state.scene_request->camera;
        window_state.temporal_preview_enabled =
            temporal_preview_fits_memory_budget(initial_width, initial_height);
        window_state.camera_distance = prepared.value().camera_distance;
        window_state.scene_frame_radius = prepared.value().frame_radius;
        window_state.shader_directory = *arguments.shader_directory;
        window_state.scene_dirty = true;
    }
    static_cast<void>(window_state.input.set_dpi(GetDpiForWindow(window)));
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window_state));
    update_navigation_title(window);
    ShowWindow(window, show_command);
    UpdateWindow(window);

    carto::d3d12::D3D12FrameScheduler scheduler(
        *window_state.device, window_state.swapchain->buffer_count());

    MSG message{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE) != 0) {
            if (message.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        if (window_state.window_error.has_value()) {
            MessageBoxA(window, window_state.window_error->c_str(),
                        "Cartographer D3D12 window", MB_ICONERROR);
            break;
        }
        if (window_state.scene_dirty && window_state.scene_request.has_value()) {
            if (auto available = scheduler.begin_submission(); !available) {
                MessageBoxA(window, available.error().message.c_str(),
                            "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
                break;
            }
            if (!window_state.device->debug_receipts().empty()) {
                scheduler.cancel_submission();
                MessageBoxA(window,
                    window_state.device->debug_receipts().front().c_str(),
                    "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
                break;
            }
            const auto camera = carto::d3d12::apply_viewport_camera_input(
                window_state.scene_camera_base, window_state.camera_distance,
                window_state.scene_request->height, window_state.input);
            if (!camera) {
                scheduler.cancel_submission();
                MessageBoxA(window, camera.error().message.c_str(),
                    "Cartographer D3D12 camera", MB_ICONERROR | MB_OK);
                break;
            }
            window_state.scene_request->camera = camera.value();
            const auto width = window_state.scene_request->width;
            const auto height = window_state.scene_request->height;
            if (window_state.scene_texture.has_value() &&
                (window_state.scene_texture_width != width ||
                 window_state.scene_texture_height != height)) {
                static_cast<void>(window_state.device->destroy_texture(
                    *window_state.scene_texture));
                window_state.scene_texture.reset();
                window_state.scene_texture_initialized = false;
            }
            if (!window_state.scene_texture.has_value()) {
                const auto descriptor = scene_target_descriptor(width, height);
                const auto created = create_scene_target(
                    *window_state.device, descriptor);
                if (!created) {
                    scheduler.cancel_submission();
                    MessageBoxA(window, created.error().message.c_str(),
                                "Cartographer D3D12 scene", MB_ICONERROR | MB_OK);
                    break;
                }
                window_state.scene_texture = created.value();
                window_state.scene_texture_width = width;
                window_state.scene_texture_height = height;
            }
            window_state.temporal_preview_enabled =
                temporal_preview_fits_memory_budget(width, height);
            if (!window_state.temporal_preview_enabled) {
                const bool has_temporal_textures = std::any_of(
                    window_state.temporal_textures.begin(),
                    window_state.temporal_textures.end(),
                    [](carto::gpu::TextureHandle texture) {
                        return static_cast<bool>(texture);
                    });
                if (has_temporal_textures) {
                    const auto released = release_temporal_textures(
                        window_state, *window_state.device);
                    if (!released) {
                        scheduler.cancel_submission();
                        MessageBoxA(window, released.error().message.c_str(),
                            "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
                        break;
                    }
                }
                window_state.temporal_scene_identity = {};
                reset_temporal_history(window_state);
                window_state.scene_request->subpixel_jitter_pixels = {};
                window_state.scene_request->temporal.reset();
            } else {
                const bool wrong_temporal_extent =
                    window_state.temporal_texture_width != width ||
                    window_state.temporal_texture_height != height;
                const bool missing_temporal_texture = std::any_of(
                    window_state.temporal_textures.begin(),
                    window_state.temporal_textures.end(),
                    [](carto::gpu::TextureHandle texture) {
                        return !static_cast<bool>(texture);
                    });
                if (wrong_temporal_extent || missing_temporal_texture) {
                    const auto released = release_temporal_textures(
                        window_state, *window_state.device);
                    if (!released) {
                        scheduler.cancel_submission();
                        MessageBoxA(window, released.error().message.c_str(),
                            "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
                        break;
                    }
                    const auto descriptor = temporal_target_descriptor(width, height);
                    for (std::size_t index = 0U;
                         index < window_state.temporal_textures.size(); ++index) {
                        const auto created = window_state.device->create_texture(descriptor);
                        if (!created) {
                            static_cast<void>(release_temporal_textures(
                                window_state, *window_state.device));
                            scheduler.cancel_submission();
                            MessageBoxA(window, created.error().message.c_str(),
                                "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
                            break;
                        }
                        window_state.temporal_textures[index] = created.value();
                    }
                    if (!window_state.temporal_textures[0U] ||
                        !window_state.temporal_textures[1U]) {
                        break;
                    }
                    window_state.temporal_texture_width = width;
                    window_state.temporal_texture_height = height;
                }

                auto stable_request = *window_state.scene_request;
                stable_request.temporal.reset();
                stable_request.subpixel_jitter_pixels = {};
                const auto stable_identity = stable_request.digest();
                if (!stable_identity) {
                    scheduler.cancel_submission();
                    MessageBoxA(window, stable_identity.error().message.c_str(),
                        "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
                    break;
                }
                if (window_state.temporal_scene_identity != stable_identity.value()) {
                    window_state.temporal_scene_identity = stable_identity.value();
                    reset_temporal_history(window_state);
                }

                const auto current_jitter = temporal_sample_jitter(
                    window_state.temporal_sample_count);
                window_state.scene_request->subpixel_jitter_pixels = current_jitter;
                carto::d3d12::SceneRenderTemporalInputs temporal;
                const std::uint32_t output_slot = window_state.temporal_history_valid
                    ? 1U - window_state.temporal_history_slot : 0U;
                temporal.resolved_color = window_state.temporal_textures[output_slot];
                temporal.resolved_initial_state =
                    window_state.temporal_texture_states[output_slot];
                if (window_state.temporal_history_valid) {
                    temporal.history_color = window_state.temporal_textures[
                        window_state.temporal_history_slot];
                    temporal.history_initial_state =
                        window_state.temporal_texture_states[
                            window_state.temporal_history_slot];
                    temporal.history_content_digest =
                        window_state.temporal_history_content_digest;
                    temporal.history_request_digest =
                        window_state.temporal_history_request_digest;
                    temporal.history_source_revision =
                        window_state.temporal_history_source_revision;
                    temporal.history_source_digest =
                        window_state.temporal_history_source_digest;
                    temporal.constant_motion_pixels =
                        carto::d3d12::temporal_history_offset(
                            window_state.temporal_history_jitter, current_jitter);
                    temporal.motion_history_request_digest =
                        window_state.temporal_history_request_digest;
                    temporal.feedback = static_cast<float>(
                        window_state.temporal_sample_count) /
                        static_cast<float>(window_state.temporal_sample_count + 1U);
                } else {
                    temporal.camera_cut = true;
                }
                window_state.scene_request->temporal = std::move(temporal);
            }
            const auto initial_state = window_state.scene_texture_initialized
                ? carto::device_ir::ResourceState::copy_source
                : carto::device_ir::ResourceState::undefined;
            const auto frame_target = window_state.swapchain->acquire_back_buffer();
            if (!frame_target) {
                scheduler.cancel_submission();
                MessageBoxA(window, frame_target.error().message.c_str(),
                            "Cartographer D3D12", MB_ICONERROR);
                break;
            }
            const auto rendered = carto::d3d12::
                render_scene_to_texture_with_frame_handoff_cached_async(
                *window_state.device, window_state.scene_render_cache,
                *window_state.scene_request,
                window_state.shader_directory, *window_state.scene_texture,
                initial_state, frame_target.value());
            if (!rendered) {
                scheduler.cancel_submission();
                static_cast<void>(window_state.device->destroy_texture(frame_target.value()));
                MessageBoxA(window, rendered.error().message.c_str(),
                            "Cartographer D3D12 scene", MB_ICONERROR | MB_OK);
                break;
            }
            if (auto tracked = scheduler.track_submission(
                    rendered.value().submission_serial); !tracked) {
                scheduler.cancel_submission();
                static_cast<void>(window_state.device->wait(
                    rendered.value().submission_serial));
                static_cast<void>(window_state.device->destroy_texture(frame_target.value()));
                MessageBoxA(window, tracked.error().message.c_str(),
                            "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
                break;
            }
            if (!window_state.device->debug_receipts().empty()) {
                static_cast<void>(window_state.device->destroy_texture(frame_target.value()));
                MessageBoxA(window, window_state.device->debug_receipts().front().c_str(),
                    "Cartographer D3D12 scene", MB_ICONERROR | MB_OK);
                break;
            }
            if (window_state.temporal_preview_enabled) {
                const auto& temporal = *window_state.scene_request->temporal;
                const auto output_slot = temporal.resolved_color ==
                    window_state.temporal_textures[0U] ? 0U : 1U;
                const auto output_state = rendered.value().temporal_output_state;
                if (!output_state.has_value()) {
                    static_cast<void>(window_state.device->destroy_texture(frame_target.value()));
                    MessageBoxA(window,
                        "The scene receipt omitted the temporal output state.",
                        "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
                    break;
                }
                const auto history_digest = temporal_texture_digest(
                    *window_state.device, temporal.resolved_color, width, height);
                if (!history_digest) {
                    static_cast<void>(window_state.device->destroy_texture(frame_target.value()));
                    MessageBoxA(window, history_digest.error().message.c_str(),
                        "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
                    break;
                }
                window_state.temporal_texture_states[output_slot] = *output_state;
                window_state.temporal_history_slot = output_slot;
                window_state.temporal_history_valid = true;
                window_state.temporal_history_content_digest = history_digest.value();
                window_state.temporal_history_request_digest = rendered.value().request_digest;
                window_state.temporal_history_source_revision =
                    window_state.scene_request->source_revision;
                window_state.temporal_history_source_digest =
                    window_state.scene_request->source_digest;
                window_state.temporal_history_jitter =
                    window_state.scene_request->subpixel_jitter_pixels;
                window_state.temporal_sample_count = std::min(
                    window_state.temporal_sample_count + 1U,
                    kTemporalPreviewSampleLimit);
            }
            window_state.scene_texture_initialized = true;
            window_state.scene_dirty = window_state.temporal_preview_enabled &&
                window_state.temporal_sample_count < kTemporalPreviewSampleLimit;
            static_cast<void>(window_state.device->destroy_texture(frame_target.value()));
            update_navigation_title(window);
            if (auto presented = window_state.swapchain->present(true); !presented) {
                MessageBoxA(window, presented.error().message.c_str(),
                            "Cartographer D3D12", MB_ICONERROR);
                break;
            }
            continue;
        }
        if (auto available = scheduler.begin_submission(); !available) {
            MessageBoxA(window, available.error().message.c_str(),
                        "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
            break;
        }
        if (!window_state.device->debug_receipts().empty()) {
            scheduler.cancel_submission();
            MessageBoxA(window, window_state.device->debug_receipts().front().c_str(),
                        "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
            break;
        }
        const auto back_buffer = window_state.swapchain->acquire_back_buffer();
        if (!back_buffer) {
            scheduler.cancel_submission();
            MessageBoxA(window, back_buffer.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
        carto::device_ir::DeviceCommandStream frame;
        const auto recorded = record_frame_graph(frame, back_buffer.value(),
            window_state.swapchain->format(), window_state.swapchain->width(),
            window_state.swapchain->height(), window_state.scene_texture);
        if (!recorded) {
            scheduler.cancel_submission();
            static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
            MessageBoxA(window, recorded.error().message.c_str(),
                        "Cartographer D3D12 frame graph", MB_ICONERROR | MB_OK);
            break;
        }
        const auto serial = window_state.device->submit(frame);
        if (!serial) {
            scheduler.cancel_submission();
            static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
            MessageBoxA(window, serial.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
        if (auto tracked = scheduler.track_submission(serial.value()); !tracked) {
            scheduler.cancel_submission();
            static_cast<void>(window_state.device->wait(serial.value()));
            static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
            MessageBoxA(window, tracked.error().message.c_str(),
                        "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
            break;
        }
        static_cast<void>(window_state.device->destroy_texture(back_buffer.value()));
        if (window_state.scene_texture.has_value() &&
            !window_state.device->debug_receipts().empty()) {
            MessageBoxA(window, window_state.device->debug_receipts().front().c_str(),
                        "Cartographer D3D12 presentation", MB_ICONERROR | MB_OK);
            break;
        }
        if (auto presented = window_state.swapchain->present(true); !presented) {
            MessageBoxA(window, presented.error().message.c_str(),
                        "Cartographer D3D12", MB_ICONERROR);
            break;
        }
    }
    if (auto drained = scheduler.wait_idle(); !drained) {
        MessageBoxA(window, drained.error().message.c_str(),
                    "Cartographer D3D12 scheduler", MB_ICONERROR | MB_OK);
    }
    if (!window_state.device->debug_receipts().empty()) {
        MessageBoxA(window, window_state.device->debug_receipts().front().c_str(),
                    "Cartographer D3D12", MB_ICONERROR | MB_OK);
    }
    if (auto released = release_temporal_textures(
            window_state, *window_state.device); !released) {
        MessageBoxA(window, released.error().message.c_str(),
                    "Cartographer D3D12 temporal preview", MB_ICONERROR | MB_OK);
    }
    if (window_state.scene_texture.has_value()) {
        static_cast<void>(window_state.device->destroy_texture(*window_state.scene_texture));
        window_state.scene_texture.reset();
    }
    return 0;
}
