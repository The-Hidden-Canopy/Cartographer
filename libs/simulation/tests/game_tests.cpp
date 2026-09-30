#include <carto/simulation/game.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_game_loop_runs_from_authoring_scene_to_history_scar() {
    auto created = carto::simulation::GameSession::create();
    require(static_cast<bool>(created), "Cartographer GMIB game must create its authoring room");
    auto game = std::move(created.value());
    const auto initial = game.frame();
    require(initial.tick == 100 && initial.red_cube_present,
            "game must start at the deterministic opening tick with a cube");
    require(!initial.scientist.visible_objects.empty() && !initial.camera.visible_objects.empty(),
            "opening game frame must expose authoring objects to observers");

    const auto teleport = game.teleport_cube({12.0, 0.0, 0.0});
    require(static_cast<bool>(teleport), "game teleport must commit to session simulation state");
    const auto after_teleport = game.frame();
    require(after_teleport.phase == "anomaly-detected" &&
                after_teleport.latest_anomaly.has_value() &&
                after_teleport.latest_anomaly->source_redacted,
            "teleport must become a redacted anomaly game phase");
    require(!after_teleport.camera.observed_anomalies.empty(),
            "camera must observe the teleport anomaly");

    require(static_cast<bool>(game.step()), "game step must advance session time");
    require(static_cast<bool>(game.destroy_cube()), "game delete must create a history scar");
    const auto deleted = game.frame();
    require(!deleted.red_cube_present && deleted.phase == "history-scar",
            "delete must remove the projected cube without editing authoring Scene");
    require(game.simulation().replay_events().size() == 2U,
            "game interventions must remain replayable");
    require(game.authoring_scene().size() == 4U,
            "simulation actions must not delete authoring objects");
    const auto before_repeat_delete = game.frame();
    const auto repeated_delete = game.destroy_cube();
    require(!repeated_delete && repeated_delete.error().code == carto::core::ErrorCode::not_found,
            "deleting an absent cube must be rejected");
    const auto after_repeat_delete = game.frame();
    require(after_repeat_delete.simulation_revision == before_repeat_delete.simulation_revision &&
                after_repeat_delete.phase == before_repeat_delete.phase,
            "repeated delete must not mutate the game frame");
}

void test_game_rejects_nonfinite_action_without_state_change() {
    auto created = carto::simulation::GameSession::create();
    require(static_cast<bool>(created), "invalid-action fixture must create");
    auto game = std::move(created.value());
    const auto before = game.frame();
    const auto digest_before = game.simulation().digest();
    const auto rejected = game.teleport_cube({std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0});
    require(!rejected && rejected.error().code == carto::core::ErrorCode::invalid_argument,
            "non-finite game action must be rejected");
    const auto after = game.frame();
    require(after.tick == before.tick && after.simulation_revision == before.simulation_revision &&
                after.phase == before.phase && game.simulation().digest() == digest_before,
            "rejected game action must not mutate session state");
}

} // namespace

int main() {
    try {
        test_game_loop_runs_from_authoring_scene_to_history_scar();
        test_game_rejects_nonfinite_action_without_state_change();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "cartographer gmib game tests passed\n";
    return 0;
}
