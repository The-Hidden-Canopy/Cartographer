#include <carto/simulation/game.hpp>

#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace {

void print_frame(const char* label, const carto::simulation::GameFrame& frame) {
    std::cout << label << " tick=" << frame.tick
              << " revision=" << frame.simulation_revision.value()
              << " phase=" << frame.phase
              << " cube_present=" << (frame.red_cube_present ? "true" : "false")
              << " scientist_anomalies=" << frame.scientist.observed_anomalies.size()
              << " camera_anomalies=" << frame.camera.observed_anomalies.size() << '\n';
}

} // namespace

namespace {

int run_interactive() {
    auto created = carto::simulation::GameSession::create();
    if (!created) {
        std::cerr << "GMIB Cartographer game setup failed: " << created.error().message << '\n';
        return 1;
    }
    auto game = std::move(created.value());
    std::cout << "commands: step | teleport X Y Z | delete | observe | quit\n";
    print_frame("opening", game.frame());

    std::string line;
    while (true) {
        std::cout << "gmib> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command == "quit" || command == "exit") break;
        if (command == "help") {
            std::cout << "commands: step | teleport X Y Z | delete | observe | quit\n";
        } else if (command == "step") {
            const auto result = game.step();
            if (!result) std::cout << "step rejected: " << result.error().message << '\n';
            print_frame("step", game.frame());
        } else if (command == "teleport") {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            const auto result = (input >> x >> y >> z)
                ? game.teleport_cube({x, y, z})
                : carto::core::Result<carto::simulation::InterventionReceipt>::failure(
                      carto::core::Diagnostic(carto::core::ErrorCode::invalid_argument,
                                              "teleport requires X Y Z"));
            if (!result) std::cout << "teleport rejected: " << result.error().message << '\n';
            print_frame("teleport", game.frame());
        } else if (command == "delete") {
            const auto result = game.destroy_cube();
            if (!result) std::cout << "delete rejected: " << result.error().message << '\n';
            print_frame("delete", game.frame());
        } else if (command == "observe" || command.empty()) {
            print_frame("observe", game.frame());
        } else {
            std::cout << "unknown command; use help\n";
        }
    }
    return 0;
}

int run_scripted() {
    auto created = carto::simulation::GameSession::create();
    if (!created) {
        std::cerr << "GMIB Cartographer game setup failed: " << created.error().message << '\n';
        return 1;
    }
    auto game = std::move(created.value());
    print_frame("opening", game.frame());
    if (!game.teleport_cube({12.0, 0.0, 0.0})) {
        std::cerr << "teleport action failed\n";
        return 1;
    }
    print_frame("teleport", game.frame());
    if (!game.step() || !game.destroy_cube()) {
        std::cerr << "delete action failed\n";
        return 1;
    }
    print_frame("delete", game.frame());
    std::cout << "replay_entries=" << game.simulation().replay_events().size()
              << " simulation_digest=" << game.simulation().digest() << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return argc > 1 && std::string(argv[1]) == "--interactive"
        ? run_interactive()
        : run_scripted();
}
