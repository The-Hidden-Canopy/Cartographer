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
}

void schema_33_requires_standards_and_migrates_schema_31_empty() {
    using namespace carto::project;
    ProjectDocument document;
    REQUIRE(document.schema_version() == 33U);
    const auto schema33 = document.serialize();
    REQUIRE(schema33.find("AUTHORING_STANDARDS_HEX ") != std::string::npos);

    auto schema31 = schema33;
    schema31.replace(0U, std::string("CARTOGRAPHER_PROJECT 33\n").size(),
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
    schema_33_requires_standards_and_migrates_schema_31_empty();
    human_update_is_journaled_undoable_stale_checked_and_persistent();
    std::cout << "project authoring standards tests passed\n";
    return EXIT_SUCCESS;
}
