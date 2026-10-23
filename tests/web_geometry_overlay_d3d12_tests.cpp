#include <carto/attributes/set.hpp>
#include <carto/d3d12/device.hpp>
#include <carto/geometry/primitives.hpp>
#include <carto/web_geometry/compiler.hpp>
#include <carto/web_geometry/overlay.hpp>
#include <carto/web_geometry/package.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

constexpr std::uint32_t kCaptureWidth = 96U;
constexpr std::uint32_t kCaptureHeight = 96U;
constexpr std::uint8_t kMaxChannelDelta = 1U;
constexpr std::size_t kMaxChangedPixels = 24U;

std::string baseline_metadata(const carto::device::DeviceIdentity& identity) {
    REQUIRE(identity.backend == carto::device::BackendKind::d3d12);
    std::ostringstream output;
    output << "format=carto-visual-baseline-v1\n"
           << "fixture=web-geometry-overlay-clusters\n"
           << "backend=d3d12\n"
           << "adapter_name=" << identity.adapter_name << '\n'
           << "vendor_id=" << identity.vendor_id << '\n'
           << "device_id=" << identity.device_id << '\n'
           << "feature_level=" << identity.feature_level << '\n'
           << "driver_label=" << identity.driver << '\n'
           << "dimensions=" << kCaptureWidth << 'x' << kCaptureHeight << '\n'
           << "pixel_encoding=rgb8-channel-round-nearest\n"
           << "max_channel_delta=" << static_cast<unsigned>(kMaxChannelDelta) << '\n'
           << "max_changed_pixels=" << kMaxChangedPixels << '\n';
    return output.str();
}

bool baseline_platform_matches(
    std::string_view metadata,
    const carto::device::DeviceIdentity& identity) {
    return metadata == baseline_metadata(identity);
}

std::size_t changed_pixel_count(
    std::span<const std::uint8_t> actual,
    std::span<const std::uint8_t> expected) {
    REQUIRE(actual.size() == expected.size() && actual.size() % 3U == 0U);
    std::size_t changed_pixels = 0U;
    for (std::size_t pixel = 0U; pixel < actual.size(); pixel += 3U) {
        bool changed = false;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const int delta = std::abs(static_cast<int>(actual[pixel + channel]) -
                                       static_cast<int>(expected[pixel + channel]));
            changed = changed || delta > kMaxChannelDelta;
        }
        if (changed) ++changed_pixels;
    }
    return changed_pixels;
}

void baseline_comparison_rejects_platform_and_pixel_drift() {
    carto::device::DeviceIdentity identity;
    identity.backend = carto::device::BackendKind::d3d12;
    identity.adapter_name = "fixture-adapter";
    identity.vendor_id = 1U;
    identity.device_id = 2U;
    identity.feature_level = "D3D_FEATURE_LEVEL_11_0";
    identity.driver = "fixture-driver";
    const std::string metadata = baseline_metadata(identity);
    REQUIRE(baseline_platform_matches(metadata, identity));
    ++identity.device_id;
    REQUIRE(!baseline_platform_matches(metadata, identity));

    std::vector<std::uint8_t> expected(25U * 3U, 0U);
    auto actual = expected;
    actual[0U] = kMaxChannelDelta;
    REQUIRE(changed_pixel_count(actual, expected) == 0U);
    actual[0U] = static_cast<std::uint8_t>(kMaxChannelDelta + 1U);
    REQUIRE(changed_pixel_count(actual, expected) == 1U);
    for (std::size_t pixel = 1U; pixel < 24U; ++pixel) {
        actual[pixel * 3U] = static_cast<std::uint8_t>(kMaxChannelDelta + 1U);
    }
    REQUIRE(changed_pixel_count(actual, expected) == kMaxChangedPixels);
    actual[24U * 3U] = static_cast<std::uint8_t>(kMaxChannelDelta + 1U);
    REQUIRE(changed_pixel_count(actual, expected) == kMaxChangedPixels + 1U);
}

std::vector<std::uint8_t> quantize_capture(std::span<const float> rgba) {
    REQUIRE(rgba.size() == static_cast<std::size_t>(kCaptureWidth) *
                               kCaptureHeight * 4U);
    std::vector<std::uint8_t> rgb;
    rgb.reserve(static_cast<std::size_t>(kCaptureWidth) * kCaptureHeight * 3U);
    for (std::size_t pixel = 0U; pixel < rgba.size(); pixel += 4U) {
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const float value = rgba[pixel + channel];
            REQUIRE(std::isfinite(value) && value >= 0.0F && value <= 1.0F);
            rgb.push_back(static_cast<std::uint8_t>(value * 255.0F + 0.5F));
        }
    }
    return rgb;
}

std::filesystem::path baseline_path(std::string_view name) {
    return std::filesystem::path{CARTO_VISUAL_BASELINE_DIR} / name;
}

void write_u16(std::ostream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xffU));
    output.put(static_cast<char>((value >> 8U) & 0xffU));
}

void write_u32(std::ostream& output, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.put(static_cast<char>((value >> shift) & 0xffU));
    }
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset) {
    REQUIRE(offset + 4U <= bytes.size());
    std::uint32_t value = 0U;
    for (std::uint32_t byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(bytes[offset + byte])) << (byte * 8U);
    }
    return value;
}

void write_baseline(const std::vector<std::uint8_t>& rgb,
                    const carto::device::DeviceIdentity& identity) {
    const auto image_path = baseline_path("web_geometry_overlay_96x96.bmp");
    const auto metadata_path = baseline_path("web_geometry_overlay_96x96.meta");
    std::filesystem::create_directories(image_path.parent_path());
    std::ofstream image{image_path, std::ios::binary | std::ios::trunc};
    REQUIRE(image.good());
    constexpr std::uint32_t image_bytes = kCaptureWidth * kCaptureHeight * 3U;
    image.put('B');
    image.put('M');
    write_u32(image, 54U + image_bytes);
    write_u16(image, 0U);
    write_u16(image, 0U);
    write_u32(image, 54U);
    write_u32(image, 40U);
    write_u32(image, kCaptureWidth);
    write_u32(image, kCaptureHeight);
    write_u16(image, 1U);
    write_u16(image, 24U);
    write_u32(image, 0U);
    write_u32(image, image_bytes);
    write_u32(image, 0U);
    write_u32(image, 0U);
    write_u32(image, 0U);
    write_u32(image, 0U);
    for (std::size_t row = 0U; row < kCaptureHeight; ++row) {
        const std::size_t source_row = kCaptureHeight - row - 1U;
        for (std::size_t column = 0U; column < kCaptureWidth; ++column) {
            const std::size_t source =
                (source_row * kCaptureWidth + column) * 3U;
            image.put(static_cast<char>(rgb[source + 2U]));
            image.put(static_cast<char>(rgb[source + 1U]));
            image.put(static_cast<char>(rgb[source]));
        }
    }
    REQUIRE(image.good());
    std::ofstream metadata{metadata_path, std::ios::binary | std::ios::trunc};
    REQUIRE(metadata.good());
    metadata << baseline_metadata(identity);
    REQUIRE(metadata.good());
}

std::vector<std::uint8_t> read_baseline_image() {
    std::ifstream image{baseline_path("web_geometry_overlay_96x96.bmp"),
                        std::ios::binary};
    REQUIRE(image.good());
    std::string header(54U, '\0');
    image.read(header.data(), static_cast<std::streamsize>(header.size()));
    REQUIRE(image.good() && header[0] == 'B' && header[1] == 'M');
    REQUIRE(read_u32(header, 2U) == 54U + kCaptureWidth * kCaptureHeight * 3U);
    REQUIRE(read_u32(header, 10U) == 54U && read_u32(header, 14U) == 40U);
    REQUIRE(read_u32(header, 18U) == kCaptureWidth &&
            read_u32(header, 22U) == kCaptureHeight);
    REQUIRE(static_cast<unsigned char>(header[26U]) == 1U &&
            static_cast<unsigned char>(header[27U]) == 0U &&
            static_cast<unsigned char>(header[28U]) == 24U &&
            static_cast<unsigned char>(header[29U]) == 0U);
    REQUIRE(read_u32(header, 30U) == 0U);
    const std::size_t image_size =
        static_cast<std::size_t>(kCaptureWidth) * kCaptureHeight * 3U;
    std::vector<std::uint8_t> bgr(image_size);
    image.read(reinterpret_cast<char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
    REQUIRE(image.good());
    char trailing = '\0';
    REQUIRE(!image.get(trailing));
    std::vector<std::uint8_t> rgb(image_size);
    for (std::size_t row = 0U; row < kCaptureHeight; ++row) {
        const std::size_t source_row = kCaptureHeight - row - 1U;
        for (std::size_t column = 0U; column < kCaptureWidth; ++column) {
            const std::size_t source = (source_row * kCaptureWidth + column) * 3U;
            const std::size_t destination = (row * kCaptureWidth + column) * 3U;
            rgb[destination] = bgr[source + 2U];
            rgb[destination + 1U] = bgr[source + 1U];
            rgb[destination + 2U] = bgr[source];
        }
    }
    return rgb;
}

std::size_t compare_with_baseline(
    const std::vector<std::uint8_t>& actual,
    const carto::device::DeviceIdentity& identity) {
    std::ifstream metadata{baseline_path("web_geometry_overlay_96x96.meta"),
                           std::ios::binary};
    REQUIRE(metadata.good());
    const std::string actual_metadata{
        std::istreambuf_iterator<char>{metadata}, std::istreambuf_iterator<char>{}};
    REQUIRE(baseline_platform_matches(actual_metadata, identity));
    const auto expected = read_baseline_image();
    REQUIRE(expected.size() == actual.size());
    const std::size_t changed_pixels = changed_pixel_count(actual, expected);
    REQUIRE(changed_pixels <= kMaxChangedPixels);
    return changed_pixels;
}

template <typename T>
std::vector<std::uint8_t> bytes_of(const T& value) {
    const auto* first = reinterpret_cast<const std::uint8_t*>(&value);
    return {first, first + sizeof(T)};
}

template <typename T>
std::vector<std::uint8_t> bytes_of(const std::vector<T>& values) {
    std::vector<std::uint8_t> bytes(values.size() * sizeof(T));
    if (!bytes.empty()) std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

carto::web_geometry::WebGeometryPackage make_overlay_package() {
    auto editable = carto::geometry::make_box({2.0, 2.0, 2.0});
    REQUIRE(editable);
    const auto mesh = editable.value().compile();
    REQUIRE(mesh);
    const auto provenance = carto::web_geometry::ProvenanceContext::from_compiled_mesh(
        mesh.value());
    REQUIRE(provenance);
    const auto package = carto::web_geometry::compile_web_geometry(
        mesh.value(), carto::attributes::AttributeSet{},
        carto::web_geometry::WebGeometryCompileOptions{}, provenance.value());
    REQUIRE(package);
    return package.value();
}

carto::gpu::ShaderHandle compile_shader(
    carto::d3d12::D3D12Device& device,
    const std::string& name,
    carto::gpu::ShaderStage stage,
    const std::string& entry_point,
    const std::string& source_text) {
    const auto source = carto::d3d12::compile_hlsl(carto::d3d12::ShaderSource{
        name, stage, entry_point, name, source_text, true, false});
    if (!source) throw TestFailure("D3D12 overlay HLSL compile: " + source.error().message);
    const auto shader = device.create_shader(source.value());
    if (!shader) throw TestFailure("D3D12 overlay shader create: " + shader.error().message);
    return shader.value();
}

void native_overlay_capture_matches_visible_package_clusters() {
    using namespace carto;
    auto created = d3d12::D3D12Device::create(d3d12::D3D12DeviceOptions{
        {}, false, true});
    REQUIRE(created);
    auto device = std::move(created).value();

    const auto package = make_overlay_package();
    const auto serialized_package = web_geometry::serialize_package(package);
    REQUIRE(serialized_package);
    web_geometry::WebGeometryImportBudget import_budget;
    import_budget.expected_source_revision = package.source_revision;
    import_budget.expected_options_digest = package.options.digest();
    web_geometry::WebGeometryRuntimeBudget runtime_budget;
    runtime_budget.device_name = device->identity().adapter_name;
    runtime_budget.driver = device->identity().driver;
    runtime_budget.viewport_width = 96U;
    runtime_budget.viewport_height = 96U;
    runtime_budget.max_clusters = package.hierarchy.clusters.size();
    runtime_budget.max_triangles = package.hierarchy.triangle_indices.size();
    runtime_budget.max_pages = package.pages.size();
    runtime_budget.max_resident_payload_bytes = package.payload.size();
    const auto runtime_admitted = web_geometry::admit_package_for_runtime(
        serialized_package.value(), import_budget, runtime_budget);
    REQUIRE(runtime_admitted);
    REQUIRE(runtime_admitted.value().runtime_receipt.validate());
    REQUIRE(runtime_admitted.value().runtime_receipt.device_name ==
            device->identity().adapter_name);
    REQUIRE(runtime_admitted.value().runtime_receipt.driver == device->identity().driver);
    auto runtime_over_budget = runtime_budget;
    --runtime_over_budget.max_clusters;
    REQUIRE(!web_geometry::admit_package_for_runtime(
        serialized_package.value(), import_budget, runtime_over_budget));
    const auto& admitted_package = runtime_admitted.value().consumer_admission.package;
    web_geometry::OverlayProjection projection;
    projection.values[0U] = 1.0;
    projection.values[5U] = 1.0;
    projection.values[10U] = 1.0;
    projection.values[15U] = 1.0;
    const auto snapshot = web_geometry::project_cluster_overlays(
        admitted_package, projection, {96U, 96U}, "project/object/overlay-fixture");
    REQUIRE(snapshot);
    const auto plan = web_geometry::build_cluster_overlay_draw_plan(snapshot.value());
    REQUIRE(plan && plan.value().validate());

    std::vector<const web_geometry::ProjectedClusterOverlay*> visible;
    for (const auto& cluster : snapshot.value().clusters) {
        if (cluster.visible) visible.push_back(&cluster);
    }
    std::sort(visible.begin(), visible.end(), [](const auto* left, const auto* right) {
        if (left->minimum_depth != right->minimum_depth) {
            return left->minimum_depth > right->minimum_depth;
        }
        return left->cluster < right->cluster;
    });
    REQUIRE(plan.value().draws.size() == visible.size());
    REQUIRE(!plan.value().draws.empty());
    for (std::size_t index = 0U; index < visible.size(); ++index) {
        REQUIRE(plan.value().draws[index].cluster == visible[index]->cluster);
        REQUIRE(plan.value().draws[index].first_vertex == index * 24U);
        REQUIRE(plan.value().draws[index].vertex_count == 24U);
    }

    constexpr char vertex_source[] = R"(
        struct VertexInput { float2 position : POSITION; };
        float4 VSMain(VertexInput input) : SV_POSITION {
            return float4(input.position, 0.0, 1.0);
        }
    )";
    constexpr char fragment_source[] = R"(
        cbuffer OverlayColor : register(b0) { float4 overlay_color; };
        float4 PSMain() : SV_TARGET { return overlay_color; }
    )";
    const auto vertex_shader = compile_shader(
        *device, "web_geometry_overlay_vertex", gpu::ShaderStage::vertex,
        "VSMain", vertex_source);
    const auto fragment_shader = compile_shader(
        *device, "web_geometry_overlay_fragment", gpu::ShaderStage::fragment,
        "PSMain", fragment_source);
    gpu::PipelineDesc pipeline_descriptor;
    pipeline_descriptor.debug_name = "web_geometry_native_overlay";
    pipeline_descriptor.shaders = {vertex_shader, fragment_shader};
    pipeline_descriptor.color_format = gpu::Format::rgba32_float;
    pipeline_descriptor.depth_format = gpu::Format::unknown;
    pipeline_descriptor.depth_test = false;
    pipeline_descriptor.depth_write = false;
    pipeline_descriptor.cull_mode = gpu::CullMode::none;
    pipeline_descriptor.vertex_stride_bytes = sizeof(web_geometry::ClusterOverlayVertex);
    pipeline_descriptor.push_constant_bytes = 16U;
    pipeline_descriptor.vertex_attributes = {
        gpu::VertexAttribute{"POSITION", 0U, gpu::VertexFormat::float2, 0U, 0U},
    };
    const auto pipeline = device->create_pipeline(pipeline_descriptor);
    if (!pipeline) {
        std::string message = "D3D12 overlay pipeline: " + pipeline.error().message;
        for (const auto& receipt : device->debug_receipts()) message += " | " + receipt;
        throw TestFailure(message);
    }

    const auto target = device->create_texture(gpu::TextureDesc{
        96U, 96U, 1U, 1U, 1U, gpu::Format::rgba32_float,
        gpu::TextureDimension::texture_2d,
        gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
        {0.0F, 0.0F, 0.0F, 1.0F},
    });
    const auto vertex_buffer = device->create_buffer(gpu::BufferDesc{
        plan.value().vertices.size() * sizeof(web_geometry::ClusterOverlayVertex),
        gpu::MemoryClass::upload,
        gpu::BufferUsage::vertex | gpu::BufferUsage::transfer_source,
    });
    REQUIRE(target && vertex_buffer);
    const auto vertex_bytes = bytes_of(plan.value().vertices);
    REQUIRE(device->write_buffer(vertex_buffer.value(), 0U, vertex_bytes));

    device_ir::DeviceCommandStream stream;
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{target.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::render_target,
    });
    stream.append(device_ir::CmdTransitionResource{
        device_ir::ResourceHandle{vertex_buffer.value()},
        device_ir::ResourceState::undefined,
        device_ir::ResourceState::vertex_read,
    });
    stream.append(device_ir::CmdBeginRendering{target.value(), std::nullopt});
    stream.append(device_ir::CmdClearColor{target.value(), {0.0F, 0.0F, 0.0F, 1.0F}});
    stream.append(device_ir::CmdSetViewport{0.0F, 0.0F, 96.0F, 96.0F, 0.0F, 1.0F});
    stream.append(device_ir::CmdSetScissor{0, 0, 96U, 96U});
    stream.append(device_ir::CmdBindPipeline{pipeline.value()});
    stream.append(device_ir::CmdBindVertexBuffer{
        vertex_buffer.value(), 0U,
        static_cast<std::uint32_t>(sizeof(web_geometry::ClusterOverlayVertex))});
    for (const auto& draw : plan.value().draws) {
        stream.append(device_ir::CmdPushConstants{
            bytes_of(plan.value().vertices[draw.first_vertex].color)});
        stream.append(device_ir::CmdDraw{
            draw.vertex_count, 1U, draw.first_vertex, 0U});
    }
    stream.append(device_ir::CmdEndRendering{});
    const auto serial = device->submit(stream, gpu::QueueType::graphics);
    if (!serial) throw TestFailure("D3D12 overlay submit: " + serial.error().message);
    REQUIRE(device->wait(serial.value()));

    const auto capture = device->read_texture_rgba32f(target.value());
    REQUIRE(capture && capture.value().size() == 96U * 96U * 4U);
    const auto captured_rgb = quantize_capture(capture.value());
    const char* update_baseline = std::getenv("CARTO_UPDATE_VISUAL_BASELINE");
    std::size_t changed_pixels = 0U;
    if (update_baseline != nullptr && std::string_view{update_baseline} == "1") {
        write_baseline(captured_rgb, device->identity());
    } else {
        changed_pixels = compare_with_baseline(captured_rgb, device->identity());
    }
    std::vector<std::array<float, 4U>> expected_colors;
    for (const auto& draw : plan.value().draws) {
        expected_colors.push_back(plan.value().vertices[draw.first_vertex].color);
    }
    std::size_t rendered_overlay_pixels = 0U;
    for (std::size_t pixel = 0U; pixel < capture.value().size(); pixel += 4U) {
        const std::array<float, 3U> rgb{
            capture.value()[pixel], capture.value()[pixel + 1U], capture.value()[pixel + 2U]};
        if (rgb[0U] + rgb[1U] + rgb[2U] < 0.1F) continue;
        bool belongs_to_draw_plan = false;
        for (const auto& color : expected_colors) {
            if (std::fabs(rgb[0U] - color[0U]) < 0.001F &&
                std::fabs(rgb[1U] - color[1U]) < 0.001F &&
                std::fabs(rgb[2U] - color[2U]) < 0.001F) {
                belongs_to_draw_plan = true;
                break;
            }
        }
        REQUIRE(belongs_to_draw_plan);
        ++rendered_overlay_pixels;
    }
    REQUIRE(rendered_overlay_pixels > 0U);
    REQUIRE(device->debug_receipts().empty());

    std::cout << "D3D12 web-geometry overlay capture: "
              << device->identity().adapter_name << " driver="
              << device->identity().driver << " visible_clusters=" << visible.size()
              << " draw_ranges=" << plan.value().draws.size()
              << " captured_overlay_pixels=" << rendered_overlay_pixels
              << " baseline_changed_pixels=" << changed_pixels
              << " tolerance=max-channel-1/255,max-24-pixels"
              << " runtime_settings=96x96 max_clusters=" << runtime_budget.max_clusters
              << " resident_payload_bytes="
              << runtime_admitted.value().runtime_receipt.resident_payload_bytes << '\n';

    REQUIRE(device->destroy_texture(target.value()));
    REQUIRE(device->destroy_buffer(vertex_buffer.value()));
    REQUIRE(device->destroy_pipeline(pipeline.value()));
    REQUIRE(device->destroy_shader(fragment_shader));
    REQUIRE(device->destroy_shader(vertex_shader));
}

} // namespace

int main() {
    try {
        baseline_comparison_rejects_platform_and_pixel_drift();
        native_overlay_capture_matches_visible_package_clusters();
        std::cout << "PASS native web-geometry overlay capture\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL native web-geometry overlay capture: " << exception.what() << '\n';
        return 1;
    }
}
