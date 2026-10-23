#pragma once

#include <carto/gpu/rhi.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::vulkan::detail {

struct HeadlessAdapterProfile {
    std::uint32_t api_version = 0U;
    bool stable_device_uuid = false;
    bool rgba16f_color_attachment = false;
    bool depth32_attachment = false;
    gpu::DeviceCapabilities capabilities;
};

[[nodiscard]] inline std::vector<std::string> headless_adapter_rejections(
    const HeadlessAdapterProfile& candidate,
    std::uint32_t required_api_version) {
    std::vector<std::string> reasons;
    if (candidate.api_version < required_api_version) {
        reasons.emplace_back("requires Vulkan API 1.1 or newer");
    }
    if (!candidate.stable_device_uuid) {
        reasons.emplace_back("does not expose a stable device UUID");
    }
    if (!candidate.capabilities.graphics_queue) {
        reasons.emplace_back("has no graphics-capable queue family");
    }
    if (!candidate.rgba16f_color_attachment) {
        reasons.emplace_back("lacks RGBA16F color-attachment format support");
    }
    if (!candidate.depth32_attachment) {
        reasons.emplace_back("lacks D32 depth-attachment format support");
    }
    if (candidate.capabilities.max_texture_dimension_2d == 0U) {
        reasons.emplace_back("reports a zero maximum 2D texture dimension");
    }
    if (candidate.capabilities.max_color_attachments == 0U) {
        reasons.emplace_back("reports a zero maximum color-attachment count");
    }
    if (!std::isfinite(candidate.capabilities.max_sampler_anisotropy) ||
        candidate.capabilities.max_sampler_anisotropy < 1.0F) {
        reasons.emplace_back("reports an invalid maximum sampler anisotropy");
    }

    if (reasons.empty()) {
        const auto capabilities = gpu::validate(candidate.capabilities);
        if (!capabilities) {
            reasons.push_back(capabilities.error().message);
        }
    }
    return reasons;
}

} // namespace carto::vulkan::detail
