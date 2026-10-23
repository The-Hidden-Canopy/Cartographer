#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/gpu/rhi.hpp>

#include <array>
#include <cstdint>
#include <string>

namespace carto::vulkan {

struct RuntimeInfo {
    std::uint32_t api_version = 0;
    std::string device_name;
    std::uint32_t vendor_id = 0U;
    std::uint32_t device_id = 0U;
    std::uint32_t graphics_queue_family = 0U;
    bool logical_device_created = false;
    gpu::DeviceCapabilities capabilities;
    std::array<std::uint8_t, 16U> device_uuid{};
};

struct HeadlessTargetReceipt {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool rgba16f_target_created = false;
    bool depth32_target_created = false;
    bool compiled_mesh_uploaded = false;
    std::uint32_t compiled_mesh_vertex_count = 0U;
    std::uint32_t compiled_mesh_index_count = 0U;
    bool indexed_draw_submitted = false;
    bool readback_completed = false;
    core::Revision source_revision;
    std::array<std::uint8_t, 8U> first_rgba16f_pixel{};
    std::array<std::uint8_t, 8U> center_rgba16f_pixel{};
};

class Runtime {
public:
    [[nodiscard]] static core::Result<RuntimeInfo> probe();
    [[nodiscard]] static core::Result<HeadlessTargetReceipt> accept_headless_targets();
    [[nodiscard]] static core::Result<HeadlessTargetReceipt> accept_headless_targets(
        const geometry::CompiledMesh& mesh,
        core::Revision expected_source_revision,
        std::uint32_t width = 512U,
        std::uint32_t height = 512U);
};

} // namespace carto::vulkan
