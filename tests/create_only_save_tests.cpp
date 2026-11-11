#include "application_access.hpp"
#include <carto/journal/journal.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TempDirectory {
public:
    TempDirectory() : path(std::filesystem::temp_directory_path() /
        ("cartographer-create-only-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "fixture file is readable");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    require(static_cast<bool>(output), "fixture file is writable");
}

void create_only_preserves_existing_targets_and_session() {
    using namespace carto;
    TempDirectory temp;
    application::ApplicationSession session;
    require(static_cast<bool>(application::testing::dispatch(session,
        application::CreatePlaneAction{"Unsaved plane", 2.0, 3.0})), "plane created");
    const auto before = session.snapshot();
    const auto rejected = [&](const std::filesystem::path& path) {
        const auto result = application::testing::dispatch(session,
            application::SaveProjectAction{path, true});
        require(!result && result.error().code == core::ErrorCode::invalid_state,
            "create-only save rejects an occupied target");
        const auto after = session.snapshot();
        require(after.project_path == before.project_path && after.dirty == before.dirty &&
                after.project_revision == before.project_revision &&
                after.source_namespace == before.source_namespace &&
                after.objects.size() == before.objects.size() &&
                after.undo_count == before.undo_count && after.redo_count == before.redo_count,
            "refusal leaves the current session and history unchanged");
    };
    const auto occupied = temp.path / "occupied.carto";
    write(occupied, "Existing bytes must survive even without a journal.");
    const auto occupied_before = read(occupied);
    rejected(occupied);
    require(read(occupied) == occupied_before &&
            !std::filesystem::exists(occupied.string() + ".journal"),
        "existing unjournaled project is not replaced or given a new journal");
    for (const auto& contents : {std::string{}, std::string{"orphan recovery evidence"}}) {
        const auto orphan = temp.path / (contents.empty() ? "empty.carto" : "orphan.carto");
        const auto journal = orphan.string() + ".journal";
        write(journal, contents);
        rejected(orphan);
        require(!std::filesystem::exists(orphan) && read(journal) == contents,
            "orphan journal, including an empty file, is preserved without a new project");
    }
    const auto directory = temp.path / "directory.carto";
    std::filesystem::create_directory(directory);
    rejected(directory);
    require(std::filesystem::is_directory(directory), "directory target is preserved");
    const auto dangling = temp.path / "dangling.carto";
    std::error_code error;
    std::filesystem::create_symlink(temp.path / "absent.carto", dangling, error);
    if (!error) {
        rejected(dangling);
        require(std::filesystem::is_symlink(std::filesystem::symlink_status(dangling)),
            "dangling target remains a link");
    } else {
        std::cout << "SKIP dangling-link fixture: " << error.message() << '\n';
    }

    const auto fresh = temp.path / "fresh.carto";
    require(static_cast<bool>(application::testing::dispatch(session,
        application::SaveProjectAction{fresh, true})), "fresh create-only save succeeds");
    require(!session.snapshot().dirty && session.snapshot().project_path == fresh,
        "fresh save binds the project and clears dirty state");
    const auto bytes = read(fresh);
    const auto journal = read(fresh.string() + ".journal");
    require(!application::testing::dispatch(session, application::SaveProjectAction{fresh, true}),
        "repeat create-only save cannot overwrite its own target");
    require(read(fresh) == bytes && read(fresh.string() + ".journal") == journal,
        "repeat refusal preserves both project and journal");
    require(static_cast<bool>(application::testing::dispatch(session,
        application::SaveProjectAction{})), "ordinary Save remains supported");
    application::ApplicationSession reopened;
    require(static_cast<bool>(application::testing::dispatch(reopened,
        application::OpenProjectAction{fresh})), "created project reopens with its journal");
    require(reopened.snapshot().objects.size() == 1U, "created project retains authored geometry");
}

void concurrent_creators_have_one_winner() {
    using namespace carto;
    TempDirectory temp;
    const auto path = temp.path / "contested.carto";
    std::array<application::ApplicationSession, 2> sessions;
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        require(static_cast<bool>(application::testing::dispatch(sessions[index],
            application::NewProjectAction{"Creator " + std::to_string(index)})),
            "independent creator starts");
    }
    std::array<bool, 2> saved{};
    std::barrier start(3);
    const auto attempt = [&](std::size_t index) {
        start.arrive_and_wait();
        saved[index] = static_cast<bool>(application::testing::dispatch(sessions[index],
            application::SaveProjectAction{path, true}));
    };
    std::jthread first(attempt, 0U);
    std::jthread second(attempt, 1U);
    start.arrive_and_wait();
    first.join();
    second.join();
    require(saved[0] != saved[1], "only one cooperating creator can own the target");
    const std::size_t winner = saved[0] ? 0U : 1U;
    application::ApplicationSession reopened;
    require(static_cast<bool>(application::testing::dispatch(reopened,
        application::OpenProjectAction{path})), "winning snapshot and journal are coherent");
    require(reopened.snapshot().source_namespace == sessions[winner].snapshot().source_namespace &&
            !sessions[1U - winner].snapshot().project_path.has_value(),
        "winner owns the saved namespace and loser remains unbound");
    journal::Journal journal(path.string() + ".journal");
    require(static_cast<bool>(journal.verify()), "winning journal verifies");
}

} // namespace

int main() {
    try {
        create_only_preserves_existing_targets_and_session();
        concurrent_creators_have_one_winner();
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
