#include "application_access.hpp"

#include <carto/application/application.hpp>
#include <carto/project/authoring_standards.hpp>
#include <carto/project/project.hpp>
#include <carto/ui/ui.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::current_path() /
            ("cartographer-project-standards-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

carto::project::ProjectAuthoringStandard door_width() {
    return {
        "guest-room-door-width",
        "Guest room door width",
        carto::project::ProjectStandardCategory::interior_door_width,
        "0.91",
        "m",
        "Approved by the project operator for guest-room doors",
        std::nullopt,
    };
}

std::string hex_encode(std::string_view bytes) {
    std::ostringstream encoded;
    encoded << std::hex << std::setfill('0');
    for (const char byte : bytes) {
        encoded << std::setw(2) << static_cast<unsigned int>(
            static_cast<unsigned char>(byte));
    }
    return encoded.str();
}

void standards_are_typed_bounded_and_strictly_serialized() {
    using namespace carto::project;
    ProjectAuthoringStandards standards;
    REQUIRE(standards.set(door_width()));
    REQUIRE(standards.find("guest-room-door-width") != nullptr);
    REQUIRE(standards.serialize().find("CARTOGRAPHER_PROJECT_AUTHORING_STANDARDS 2\n") == 0U);

    const auto decoded = ProjectAuthoringStandards::deserialize(standards.serialize());
    REQUIRE(decoded);
    REQUIRE(decoded.value().serialize() == standards.serialize());
    REQUIRE(!ProjectAuthoringStandards::deserialize(standards.serialize() + "unexpected\n"));

    ProjectAuthoringStandards invalid;
    auto malformed = door_width();
    malformed.value = "nan";
    REQUIRE(!invalid.set(std::move(malformed)));
}

void erase_v33_character_record_tables(std::string& serialized) {
    for (const std::string table : {
            "POSE_LIBRARIES 0\n",
            "WEIGHT_EDIT_RECEIPTS 0\n",
            "CURVE_EDIT_RECEIPTS 0\n"}) {
        const auto record = serialized.find(table);
        REQUIRE(record != std::string::npos);
        serialized.erase(record, table.size());
    }
    const auto kinematic_table = serialized.find("KINEMATIC_GRAPHS 0\n");
    if (kinematic_table != std::string::npos) {
        serialized.erase(kinematic_table, std::string("KINEMATIC_GRAPHS 0\n").size());
    }
}

void schema_35_preserves_kinematic_graph_table_and_migrates_schema_34() {
    using namespace carto::project;
    ProjectDocument document;
    REQUIRE(document.schema_version() == 38U);
    auto schema35 = document.serialize();
    const auto schema35_drafts = schema35.find("UNADMITTED_MODE_KEY_DRAFTS 0\n");
    REQUIRE(schema35_drafts != std::string::npos);
    schema35.erase(schema35_drafts, std::string("UNADMITTED_MODE_KEY_DRAFTS 0\n").size());
    const std::string accepted_table = "ACCEPTED_AUTORIGS 0\n";
    const auto accepted_pos = schema35.find(accepted_table);
    REQUIRE(accepted_pos != std::string::npos);
    schema35.erase(accepted_pos, accepted_table.size());
    schema35.replace(0U, std::string("CARTOGRAPHER_PROJECT 38\n").size(),
        "CARTOGRAPHER_PROJECT 35\n");
    const auto migrated_schema35 = ProjectDocument::deserialize(schema35);
    REQUIRE(migrated_schema35 && migrated_schema35.value().serialize().starts_with("CARTOGRAPHER_PROJECT 38\n"));
    std::ostringstream graph_record_bytes;
    graph_record_bytes << "CONNECTION_PORT_GRAPH "
                       << std::quoted(document.connection_port_graph().serialize()) << '\n';
    const auto empty_graph_record = graph_record_bytes.str();
    REQUIRE(schema35.find("AUTHORING_STANDARDS_HEX ") != std::string::npos);
    REQUIRE(schema35.find("CONNECTION_PORT_GRAPH ") != std::string::npos);
    REQUIRE(schema35.find("KINEMATIC_GRAPHS 0\n") != std::string::npos);
    auto missing_kinematic_graphs = schema35;
    const auto kinematic_table = missing_kinematic_graphs.find("KINEMATIC_GRAPHS ");
    const auto kinematic_table_end = missing_kinematic_graphs.find('\n', kinematic_table);
    REQUIRE(kinematic_table != std::string::npos && kinematic_table_end != std::string::npos);
    missing_kinematic_graphs.erase(kinematic_table,
        kinematic_table_end - kinematic_table + 1U);
    REQUIRE(!ProjectDocument::deserialize(missing_kinematic_graphs));

    auto schema34 = schema35;
    const auto v34_graph_table = schema34.find("KINEMATIC_GRAPHS 0\n");
    REQUIRE(v34_graph_table != std::string::npos);
    schema34.erase(v34_graph_table, std::string("KINEMATIC_GRAPHS 0\n").size());
    schema34.replace(0U, std::string("CARTOGRAPHER_PROJECT 35\n").size(),
        "CARTOGRAPHER_PROJECT 34\n");
    const auto migrated_schema34 = ProjectDocument::deserialize(schema34);
    REQUIRE(migrated_schema34 && migrated_schema34.value().kinematic_graphs().empty());
    REQUIRE(migrated_schema34.value().serialize().starts_with("CARTOGRAPHER_PROJECT 38\n"));

    auto missing_connection_graph = schema35;
    const auto graph_record = missing_connection_graph.find(empty_graph_record);
    REQUIRE(graph_record != std::string::npos);
    missing_connection_graph.erase(graph_record, empty_graph_record.size());
    REQUIRE(!ProjectDocument::deserialize(missing_connection_graph));

    auto schema33 = schema34;
    schema33.replace(0U, std::string("CARTOGRAPHER_PROJECT 34\n").size(),
        "CARTOGRAPHER_PROJECT 33\n");
    const auto schema33_graph = schema33.find(empty_graph_record);
    REQUIRE(schema33_graph != std::string::npos);
    schema33.erase(schema33_graph, empty_graph_record.size());
    const auto migrated_schema33 = ProjectDocument::deserialize(schema33);
    REQUIRE(migrated_schema33 && migrated_schema33.value().connection_port_graph().empty());

    auto schema31 = schema34;
    schema31.replace(0U, std::string("CARTOGRAPHER_PROJECT 34\n").size(),
        "CARTOGRAPHER_PROJECT 31\n");
    const auto standards_line = schema31.find("AUTHORING_STANDARDS_HEX ");
    REQUIRE(standards_line != std::string::npos);
    erase_v33_character_record_tables(schema31);
    auto standards_with_record = schema31;
    auto recorded_standards = ProjectAuthoringStandards{};
    REQUIRE(recorded_standards.set(door_width()));
    const auto record_end = standards_with_record.find('\n', standards_line);
    standards_with_record.replace(standards_line, record_end - standards_line,
        "AUTHORING_STANDARDS_HEX " + hex_encode(recorded_standards.serialize()));
    const auto retained = ProjectDocument::deserialize(standards_with_record);
    REQUIRE(retained);
    REQUIRE(retained.value().authoring_standards().serialize() ==
        recorded_standards.serialize());

    const auto stripped_line = schema31.find("AUTHORING_STANDARDS_HEX ");
    REQUIRE(stripped_line != std::string::npos);
    const auto stripped_end = schema31.find('\n', stripped_line);
    schema31.erase(stripped_line, stripped_end - stripped_line + 1U);
    const auto migrated = ProjectDocument::deserialize(schema31);
    REQUIRE(migrated);
    REQUIRE(migrated.value().authoring_standards().empty());

    auto missing_standards = schema33;
    const auto required_line = missing_standards.find("AUTHORING_STANDARDS_HEX ");
    const auto required_end = missing_standards.find('\n', required_line);
    missing_standards.erase(required_line, required_end - required_line + 1U);
    REQUIRE(!ProjectDocument::deserialize(missing_standards));
}

void human_update_is_journaled_undoable_stale_checked_and_persistent() {
    using namespace carto;
    application::ApplicationSession session;
    ui::UiController controller(session);
    project::ProjectAuthoringStandards standards;
    REQUIRE(standards.set(door_width()));
    const auto before = session.snapshot();

    const auto updated = controller.set_project_authoring_standards(standards);
    REQUIRE(updated);
    REQUIRE(updated.value().action == "Update Project Standards");
    REQUIRE(updated.value().document_changed);
    REQUIRE(updated.value().revision_before == before.project_revision);
    REQUIRE(updated.value().revision_after == before.project_revision.next());
    REQUIRE(controller.snapshot().authoring_standards.serialize() == standards.serialize());

    const auto no_change = controller.set_project_authoring_standards(standards);
    REQUIRE(!no_change);

    const auto undone = application::testing::dispatch(session, application::UndoAction{});
    REQUIRE(undone);
    REQUIRE(session.snapshot().authoring_standards.empty());
    const auto redone = application::testing::dispatch(session, application::RedoAction{});
    REQUIRE(redone);
    REQUIRE(session.snapshot().authoring_standards.serialize() == standards.serialize());

    auto stale_standards = standards;
    REQUIRE(stale_standards.set({
        "standard.grid", "Grid increment", project::ProjectStandardCategory::grid_increment,
        "0.25", "m", "Explicit floor plan increment", std::nullopt}));
    const auto current = session.snapshot();
    REQUIRE(!controller.dispatch(application::SetProjectAuthoringStandardsAction{
        current.project_generation, before.project_revision, std::move(stale_standards)}));
    REQUIRE(session.snapshot().authoring_standards.serialize() == standards.serialize());

    TempDirectory directory;
    const auto path = directory.path() / "standards.carto";
    REQUIRE(application::testing::dispatch(session, application::SaveProjectAction{path}));
    application::ApplicationSession reopened;
    REQUIRE(application::testing::dispatch(reopened,
        application::OpenProjectAction{path, false}));
    REQUIRE(reopened.snapshot().authoring_standards.serialize() == standards.serialize());
}

} // namespace

int main() {
    standards_are_typed_bounded_and_strictly_serialized();
    schema_35_preserves_kinematic_graph_table_and_migrates_schema_34();
    human_update_is_journaled_undoable_stale_checked_and_persistent();
    std::cout << "project authoring standards tests passed\n";
    return EXIT_SUCCESS;
}
