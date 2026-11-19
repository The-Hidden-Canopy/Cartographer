#pragma once

#include <carto/io/gltf_export.hpp>

#include <cstdint>
#include <filesystem>

namespace carto::desktop {
[[nodiscard]] inline io::GltfExportOptions interchange_export_options(
    const std::filesystem::path& project_path, bool bake_graphs, std::uint32_t bake_resolution) {
    return io::GltfExportOptions{project_path, {}, bake_graphs ? bake_resolution : 0U, {},
                                 64ULL * 1024ULL * 1024ULL, false, {}};
}
} // namespace carto::desktop
