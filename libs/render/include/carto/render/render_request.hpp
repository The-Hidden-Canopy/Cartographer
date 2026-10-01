#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/scene/scene.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace carto::render {

struct Extent2D {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class RenderQualityProfile {
    draft,
    interactive,
    final_fast,
    final_high,
    reference,
};

enum class OutputFormat {
    png,
    exr,
};

enum class CaptureResource {
    linear_hdr_color,
    tone_mapped_color,
    depth,
    normals,
    object_id,
};

struct RenderQualitySettings {
    std::uint32_t shadow_map_size = 1024U;
    std::uint32_t shadow_cascades = 2U;
    std::uint32_t samples = 1U;
    std::uint32_t anisotropy = 1U;
    bool ambient_occlusion = false;
    bool screen_space_reflections = false;
    bool exr_capable = false;

    [[nodiscard]] core::Result<void> validate() const;
};

[[nodiscard]] core::Result<RenderQualitySettings> resolve_quality(
    RenderQualityProfile profile);

struct RenderRequest {
    core::Revision source_revision;
    scene::ObjectId camera;
    Extent2D extent;
    RenderQualityProfile quality = RenderQualityProfile::interactive;
    std::uint32_t samples = 0U;
    OutputFormat output = OutputFormat::png;
    bool transparent_background = false;
    bool authoring_overlays = false;
    std::vector<CaptureResource> captures;

    [[nodiscard]] core::Result<RenderQualitySettings> resolve() const;
    [[nodiscard]] core::Result<void> validate() const;
};

enum class RenderStatus {
    completed,
    failed,
    cancelled,
};

struct RenderReceipt {
    RenderStatus status = RenderStatus::failed;
    core::Revision source_revision;
    scene::ObjectId camera;
    Extent2D extent;
    RenderQualitySettings settings;
    OutputFormat output = OutputFormat::png;
    std::filesystem::path output_path;
    std::string output_hash;
    std::uint64_t duration_milliseconds = 0U;
    std::uint64_t peak_gpu_bytes = 0U;
    std::vector<std::string> diagnostics;
};

} // namespace carto::render
