#pragma once

#include <carto/io/obj.hpp>

namespace carto::io {

struct StlImportResult {
    geometry::EditableMesh mesh;
    IoReport report;
};

[[nodiscard]] core::Result<IoReport> export_stl(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path);

[[nodiscard]] core::Result<StlImportResult> import_stl(
    const std::filesystem::path& path);

} // namespace carto::io
