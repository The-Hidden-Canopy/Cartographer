#pragma once

#include <carto/io/gltf_export.hpp>

#include <string>

namespace carto::desktop {

// The exporter describes any unsupported or projected source data in its
// report. The desktop must carry every warning to the person choosing export.
[[nodiscard]] inline std::string interchange_export_feedback(const io::GltfExportReport& report) {
    std::string message = "Exported " + report.path.string() + "\n" +
        std::to_string(report.meshes) + " mesh(es), " +
        std::to_string(report.triangles) + " triangle(s).";
    if (!report.warnings.empty()) {
        message += "\n\nExport limitations:";
        for (const auto& warning : report.warnings) message += "\n- " + warning;
    }
    return message;
}

} // namespace carto::desktop
