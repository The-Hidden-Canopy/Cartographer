#pragma once

#include <carto/io/obj.hpp>

namespace carto::io {

struct PlyImportResult {
    geometry::EditableMesh mesh;
    IoReport report;
};

[[nodiscard]] core::Result<IoReport> export_ply(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path);

[[nodiscard]] core::Result<PlyImportResult> import_ply(
    const std::filesystem::path& path);

} // namespace carto::io
