#pragma once

#include <carto/project/project.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace carto::test_support {

inline std::string current_project_header() {
    return "CARTOGRAPHER_PROJECT " + std::to_string(project::ProjectDocument::kSchemaVersion) + "\n";
}

inline void erase_empty_project_table(std::string& text, std::string_view table) {
    const auto offset = text.find(table);
    if (offset == std::string::npos)
        throw std::runtime_error("historical project fixture is missing an expected empty table");
    text.erase(offset, table.size());
}

// Schema 40 joined the realism records to the canonical schema history. They
// must not remain in fixtures that exercise the earlier canonical layouts.
inline void erase_schema40_tables(std::string& text) {
    erase_empty_project_table(text, "RIG_TEMPLATE_BINDINGS 0\n");
    erase_empty_project_table(text, "AGING_LAYERS 0\n");
}

} // namespace carto::test_support
