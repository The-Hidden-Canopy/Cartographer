#include <carto/vulkan/runtime.hpp>
#include <carto/geometry/primitives.hpp>
#include "../libs/vulkan/src/adapter_admission.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

#define REQUIRE(condition)                                                               \
    do {                                                                                 \
        if (!(condition)) throw TestFailure(std::string("requirement failed: ") + #condition); \
    } while (false)

void headless_adapter_admission_reports_each_failed_requirement() {
    carto::vulkan::detail::HeadlessAdapterProfile supported;
    supported.api_version = VK_API_VERSION_1_1;
    supported.stable_device_uuid = true;
    supported.rgba16f_color_attachment = true;
    supported.depth32_attachment = true;
    supported.capabilities.graphics_queue = true;
    supported.capabilities.max_texture_dimension_2d = 4096U;
    supported.capabilities.max_sampler_anisotropy = 1.0F;
    supported.capabilities.max_color_attachments = 8U;
    REQUIRE(carto::vulkan::detail::headless_adapter_rejections(
        supported, VK_API_VERSION_1_1).empty());

    auto unsupported = supported;
    unsupported.api_version = VK_API_VERSION_1_0;
    unsupported.stable_device_uuid = false;
    unsupported.capabilities.graphics_queue = false;
    unsupported.rgba16f_color_attachment = false;
    unsupported.depth32_attachment = false;
    unsupported.capabilities.max_texture_dimension_2d = 0U;
    const auto reasons = carto::vulkan::detail::headless_adapter_rejections(
        unsupported, VK_API_VERSION_1_1);
    const auto has_reason = [&reasons](const std::string& expected) {
        return std::find(reasons.begin(), reasons.end(), expected) != reasons.end();
    };
    REQUIRE(has_reason("requires Vulkan API 1.1 or newer"));
    REQUIRE(has_reason("does not expose a stable device UUID"));
    REQUIRE(has_reason("has no graphics-capable queue family"));
    REQUIRE(has_reason("lacks RGBA16F color-attachment format support"));
    REQUIRE(has_reason("lacks D32 depth-attachment format support"));
    REQUIRE(has_reason("reports a zero maximum 2D texture dimension"));

    auto invalid_capabilities = supported;
    invalid_capabilities.capabilities.max_color_attachments = 0U;
    const auto capability_reasons = carto::vulkan::detail::headless_adapter_rejections(
        invalid_capabilities, VK_API_VERSION_1_1);
    REQUIRE(capability_reasons.size() == 1U);
    REQUIRE(capability_reasons.front().find("zero maximum color-attachment count") !=
        std::string::npos);

    invalid_capabilities = supported;
    invalid_capabilities.capabilities.max_sampler_anisotropy =
        std::numeric_limits<float>::quiet_NaN();
    const auto invalid_anisotropy_reasons =
        carto::vulkan::detail::headless_adapter_rejections(
            invalid_capabilities, VK_API_VERSION_1_1);
    REQUIRE(invalid_anisotropy_reasons.size() == 1U);
    REQUIRE(invalid_anisotropy_reasons.front().find(
        "invalid maximum sampler anisotropy") != std::string::npos);
}

} // namespace

int main() {
    try {
        headless_adapter_admission_reports_each_failed_requirement();
        const auto runtime = carto::vulkan::Runtime::probe();
        if (std::getenv("CARTO_VULKAN_EXPECT_UNSUPPORTED") != nullptr) {
            REQUIRE(!runtime);
            REQUIRE(!runtime.error().message.empty());
            const auto& diagnostic = runtime.error().message;
            REQUIRE(diagnostic.find("VK_ERROR_") != std::string::npos ||
                diagnostic.find("requires Vulkan") != std::string::npos ||
                diagnostic.find("no physical device") != std::string::npos ||
                diagnostic.find("rejected adapters:") != std::string::npos);
            if (diagnostic.find("no adapter meeting") != std::string::npos) {
                REQUIRE(diagnostic.find("rejected adapters:") != std::string::npos);
            }
            std::cout << "unsupported Vulkan diagnostic: "
                      << diagnostic << '\n';
            return 0;
        }
        REQUIRE(runtime);
        REQUIRE(!runtime.value().device_name.empty());
        REQUIRE(runtime.value().logical_device_created);
        REQUIRE(runtime.value().capabilities.graphics_queue);
        REQUIRE(runtime.value().capabilities.max_texture_dimension_2d > 0U);
        REQUIRE(std::any_of(
            runtime.value().device_uuid.begin(),
            runtime.value().device_uuid.end(),
            [](const std::uint8_t value) { return value != 0U; }));
        REQUIRE(carto::gpu::validate(runtime.value().capabilities));
        std::cout << "headless Vulkan device: " << runtime.value().device_name << '\n';

        const auto source_mesh = carto::geometry::make_plane(1.3, 1.3);
        REQUIRE(source_mesh);
        const auto compiled_mesh = source_mesh.value().compile();
        REQUIRE(compiled_mesh);
        const auto invalid_mesh = carto::vulkan::Runtime::accept_headless_targets(
            carto::geometry::CompiledMesh{}, carto::core::Revision{});
        REQUIRE(!invalid_mesh);
        const auto invalid_extent = carto::vulkan::Runtime::accept_headless_targets(
            compiled_mesh.value(), compiled_mesh.value().source_revision, 0U, 512U);
        REQUIRE(!invalid_extent);
        if (runtime.value().capabilities.max_texture_dimension_2d <
            std::numeric_limits<std::uint32_t>::max()) {
            const auto oversized_extent = carto::vulkan::Runtime::accept_headless_targets(
                compiled_mesh.value(),
                compiled_mesh.value().source_revision,
                runtime.value().capabilities.max_texture_dimension_2d + 1U,
                1U);
            REQUIRE(!oversized_extent);
        }
        const auto stale_mesh = carto::vulkan::Runtime::accept_headless_targets(
            compiled_mesh.value(), compiled_mesh.value().source_revision.next());
        REQUIRE(!stale_mesh);
        const auto targets = carto::vulkan::Runtime::accept_headless_targets(
            compiled_mesh.value(), compiled_mesh.value().source_revision);
        REQUIRE(targets);
        REQUIRE(targets.value().width == 512U);
        REQUIRE(targets.value().height == 512U);
        REQUIRE(targets.value().rgba16f_target_created);
        REQUIRE(targets.value().depth32_target_created);
        REQUIRE(targets.value().compiled_mesh_uploaded);
        REQUIRE(targets.value().compiled_mesh_vertex_count == 4U);
        REQUIRE(targets.value().compiled_mesh_index_count == 6U);
        REQUIRE(targets.value().indexed_draw_submitted);
        REQUIRE(targets.value().readback_completed);
        REQUIRE(targets.value().source_revision == compiled_mesh.value().source_revision);
        REQUIRE(targets.value().first_rgba16f_pixel[0] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[1] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[2] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[3] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[4] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[5] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[6] == 0x00U);
        REQUIRE(targets.value().first_rgba16f_pixel[7] == 0x3cU);
        const bool center_has_color =
            targets.value().center_rgba16f_pixel[0] != 0x00U ||
            targets.value().center_rgba16f_pixel[1] != 0x00U ||
            targets.value().center_rgba16f_pixel[2] != 0x00U ||
            targets.value().center_rgba16f_pixel[3] != 0x00U ||
            targets.value().center_rgba16f_pixel[4] != 0x00U ||
            targets.value().center_rgba16f_pixel[5] != 0x00U;
        REQUIRE(center_has_color);
        REQUIRE(targets.value().center_rgba16f_pixel[6] == 0x00U);
        REQUIRE(targets.value().center_rgba16f_pixel[7] == 0x3cU);

        if (std::getenv("CARTO_VULKAN_8K") != nullptr) {
            const auto reference_targets = carto::vulkan::Runtime::accept_headless_targets(
                compiled_mesh.value(), compiled_mesh.value().source_revision, 8192U, 8192U);
            REQUIRE(reference_targets);
            REQUIRE(reference_targets.value().width == 8192U);
            REQUIRE(reference_targets.value().height == 8192U);
            REQUIRE(reference_targets.value().compiled_mesh_vertex_count == 4U);
            REQUIRE(reference_targets.value().compiled_mesh_index_count == 6U);
            REQUIRE(reference_targets.value().indexed_draw_submitted);
            REQUIRE(reference_targets.value().readback_completed);
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
