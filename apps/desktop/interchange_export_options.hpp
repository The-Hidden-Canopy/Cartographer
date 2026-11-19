#pragma once

#include <carto/io/gltf_export.hpp>

#include <cstdint>
#include <filesystem>

namespace carto::desktop {
[[nodiscard]] inline io::GltfExportOptions interchange_export_options(
    const std::filesystem::path& project_path, bool bake_graphs, std::uint32_t bake_resolution) {
    io::GltfExportOptions options{};
    options.project_path = project_path;
    options.graph_bake_resolution = bake_graphs ? bake_resolution : 0U;
    return options;
}
} // namespace carto::desktop
