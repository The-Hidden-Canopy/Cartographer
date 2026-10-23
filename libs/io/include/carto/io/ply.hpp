#pragma once

#include <carto/io/obj.hpp>

namespace carto::io {

enum class PlyFormat {
    ascii,
    binary_little_endian,
};

struct PlyImportResult {
    geometry::EditableMesh mesh;
    IoReport report;
};

[[nodiscard]] core::Result<IoReport> export_ply(
    const geometry::EditableMesh& mesh,
    const std::filesystem::path& path,
    PlyFormat format = PlyFormat::ascii);

[[nodiscard]] core::Result<PlyImportResult> import_ply(
    const std::filesystem::path& path);

} // namespace carto::io
