#include <carto/geometry/curve_path.hpp>
#include <carto/journal/journal.hpp>
#include <carto/project/project.hpp>
#include <carto/project/transaction.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace carto;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TempDirectory final {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("cartographer-curve-path-project-tests-" + std::to_string(stamp));
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

geometry::CurvePath path_source(core::Revision revision) {
    geometry::CurvePath path;
    path.source_identity = "road.rural-main";
    path.source_revision = revision;
    path.segments.push_back({
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
        {2.0, 0.0, 0.0}, {3.0, 0.0, 0.0}});
    return path;
}

void curve_source_uses_project_transactions_and_persists() {
    TempDirectory directory;
    project::ProjectDocument document;
    journal::Journal journal(directory.path() / "curve-path-events.log");

    auto create = project::ProjectTransaction::begin(document, journal, "curve-source-tests");
    require(create && create.value().set_curve_path(path_source(core::Revision{1U})),
        "new curve source is staged through the project transaction");
    const auto created = create.value().commit("curve.path.create");
    require(created && created.value().revision_before == core::Revision{} &&
            created.value().revision_after == core::Revision{1U},
        "curve source creation advances the project revision once");

    auto updated_path = path_source(core::Revision{2U});
    updated_path.segments.front().p1.y = 0.25;
    updated_path.segments.front().p2.y = 0.25;
    auto update = project::ProjectTransaction::begin(document, journal, "curve-source-tests");
    require(update && update.value().set_curve_path(updated_path),
        "curve edits advance their stable source identity revision");
    const auto updated = update.value().commit("curve.path.update");
    require(updated && updated.value().revision_after == core::Revision{2U},
        "curve source update is durably journaled with project revision");

    auto stale = project::ProjectTransaction::begin(document, journal, "curve-source-tests");
    require(stale && !stale.value().set_curve_path(path_source(core::Revision{1U})),
        "stale source revision cannot overwrite a newer curve");
    require(static_cast<bool>(stale.value().rollback()),
        "rejected stale curve transaction rolls back");
    require(document.curve_paths().at("road.rural-main").serialize() ==
                updated_path.serialize(),
        "stale curve refusal leaves project source unchanged");

    auto invalid_path = path_source(core::Revision{3U});
    invalid_path.source_identity = "../outside";
    auto invalid = project::ProjectTransaction::begin(document, journal, "curve-source-tests");
    require(invalid && !invalid.value().set_curve_path(std::move(invalid_path)),
        "invalid curve identity is refused before journal publication");
    require(static_cast<bool>(invalid.value().rollback()),
        "invalid curve transaction rolls back cleanly");

    const auto events = journal.read_all();
    require(events && events.value().size() == 2U &&
            events.value()[0U].event_type == "curve.path.create" &&
            events.value()[1U].event_type == "curve.path.update",
        "only successful source transitions emit journal events");

    const auto encoded = document.serialize();
    const auto reopened = project::ProjectDocument::deserialize(encoded);
    require(reopened && reopened.value().curve_paths().size() == 1U &&
            reopened.value().curve_paths().at("road.rural-main").serialize() ==
                updated_path.serialize(),
        "project schema v17 round-trips the exact authored curve source");
    const auto path = directory.path() / "rural-road.carto";
    require(static_cast<bool>(document.save_atomic(path)),
        "curve source project atomically saves");
    const auto loaded = project::ProjectDocument::load(path);
    require(loaded && loaded.value().curve_paths().at("road.rural-main").serialize() ==
            updated_path.serialize(),
        "curve source survives project file close and reopen");
}

} // namespace

int main() {
    curve_source_uses_project_transactions_and_persists();
    std::cout << "curve path project tests passed\n";
    return EXIT_SUCCESS;
}
