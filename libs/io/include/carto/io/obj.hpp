#pragma once

#include <carto/core/result.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace carto::io {

struct IoReport {
    std::size_t vertices = 0;
    std::size_t faces = 0;
    std::size_t triangles = 0;
    std::vector<std::string> warnings;
};

struct ImportResult {
    geometry::EditableMesh mesh;
    IoReport report;
};

[[nodiscard]] core::Result<IoReport> export_obj(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path);

[[nodiscard]] core::Result<ImportResult> import_obj(const std::filesystem::path& path);

} // namespace carto::io

