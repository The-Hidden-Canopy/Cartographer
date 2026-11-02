#include <carto/project/project.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
int main() {
    using carto::project::ProjectDocument;
    const auto current = ProjectDocument{}.serialize();
    const std::string table = "CONTROL_WORKSPACE 0\n";
    require(current.starts_with("CARTOGRAPHER_PROJECT 39\n") && current.find(table) != std::string::npos,
        "v39 writes workspace table");
    auto decoded = ProjectDocument::deserialize(current);
    require(decoded && !decoded.value().control_workspace(), "v39 empty workspace roundtrip");
    auto missing = current;
    missing.erase(missing.find(table), table.size());
    require(!ProjectDocument::deserialize(missing), "v39 requires workspace table");
    auto v38 = missing;
    v38.replace(0, std::string("CARTOGRAPHER_PROJECT 39\n").size(), "CARTOGRAPHER_PROJECT 38\n");
    auto migrated = ProjectDocument::deserialize(v38);
    require(migrated && !migrated.value().control_workspace() &&
        migrated.value().serialize().starts_with("CARTOGRAPHER_PROJECT 39\n"), "v38 migrates without workspace");
    auto mislabeled = current;
    mislabeled.replace(0, std::string("CARTOGRAPHER_PROJECT 39\n").size(), "CARTOGRAPHER_PROJECT 38\n");
    require(!ProjectDocument::deserialize(mislabeled), "v38 refuses v39 table");
    auto malformed = current;
    malformed.replace(malformed.find(table), table.size(), "CONTROL_WORKSPACE 1\nCONTROL_WORKSPACE_SNAPSHOT 1 invalid\n");
    require(!ProjectDocument::deserialize(malformed), "malformed workspace refused");
    auto too_many = current;
    too_many.replace(too_many.find(table), table.size(), "CONTROL_WORKSPACE 2\n");
    require(!ProjectDocument::deserialize(too_many), "multiple workspaces refused");
}
