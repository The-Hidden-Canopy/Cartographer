#include <carto/project/project.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void require(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
}

int main() {
    using carto::project::ProjectDocument;
    const auto current = ProjectDocument{}.serialize();
    const std::string table = "UNADMITTED_MODE_KEY_DRAFTS 0\n";
    require(current.starts_with("CARTOGRAPHER_PROJECT 39\n") &&
        current.find(table) != std::string::npos, "v38 writes an explicit empty table");
    const auto empty = ProjectDocument::deserialize(current);
    require(empty && empty.value().unadmitted_mode_key_drafts().empty(), "v38 empty round trip");
    auto missing = current;
    missing.erase(missing.find(table), table.size());
    require(!ProjectDocument::deserialize(missing), "v38 missing mandatory table refused");
    auto v37 = missing;
    if (const auto control_workspace = v37.find("CONTROL_WORKSPACE 0\n"); control_workspace != std::string::npos)
        v37.erase(control_workspace, std::string("CONTROL_WORKSPACE 0\n").size());
    v37.replace(0, std::string("CARTOGRAPHER_PROJECT 39\n").size(), "CARTOGRAPHER_PROJECT 37\n");
    const auto migrated = ProjectDocument::deserialize(v37);
    require(migrated && migrated.value().unadmitted_mode_key_drafts().empty() &&
        migrated.value().serialize().starts_with("CARTOGRAPHER_PROJECT 39\n"), "v37 migrates with no drafts");
    auto mislabeled = current;
    if (const auto control_workspace = mislabeled.find("CONTROL_WORKSPACE 0\n"); control_workspace != std::string::npos)
        mislabeled.erase(control_workspace, std::string("CONTROL_WORKSPACE 0\n").size());
    mislabeled.replace(0, std::string("CARTOGRAPHER_PROJECT 39\n").size(), "CARTOGRAPHER_PROJECT 37\n");
    require(!ProjectDocument::deserialize(mislabeled), "v37 cannot carry v38 table");
    auto over = current;
    over.replace(over.find(table), table.size(), "UNADMITTED_MODE_KEY_DRAFTS 513\n");
    require(!ProjectDocument::deserialize(over), "over-budget draft count refused");
    auto malformed = current;
    malformed.replace(malformed.find(table), table.size(),
        "UNADMITTED_MODE_KEY_DRAFTS 1\nUNADMITTED_MODE_KEY_DRAFT \"x\" 1 invalid\n");
    require(!ProjectDocument::deserialize(malformed), "malformed draft row refused");
    std::cout << "unadmitted mode-key project format tests passed\n";
}
