#pragma once

#include <carto/simulation/simulation.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace carto::simulation {

struct GameConfig {
    std::string scenario_id{"red-cube-vertical-slice"};
    std::uint64_t seed{44};
};

struct GameFrame {
    std::uint64_t tick{0};
    core::Revision simulation_revision{};
    std::string phase{};
    bool red_cube_present{false};
    std::optional<AnomalyRecord> latest_anomaly{};
    ObservationRecord scientist{};
    ObservationRecord skeptic{};
    ObservationRecord camera{};
};

// Cartographer's playable GMIB scenario. The authoring Scene is owned by this
// session as the level/model, while SimulationSession owns the disposable game
// state. Interventions never rewrite authoring geometry.
class GameSession {
public:
    [[nodiscard]] static core::Result<GameSession> create(GameConfig config = {});

    [[nodiscard]] const scene::Scene& authoring_scene() const noexcept { return authoring_scene_; }
    [[nodiscard]] const SimulationSession& simulation() const noexcept { return simulation_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return simulation_.tick(); }
    [[nodiscard]] GameFrame frame() const;
    [[nodiscard]] core::Result<void> step();
    [[nodiscard]] core::Result<InterventionReceipt> teleport_cube(core::Vec3d destination);
    [[nodiscard]] core::Result<InterventionReceipt> destroy_cube();

private:
    GameSession() = default;

    [[nodiscard]] Observer observer(scene::ObjectId object, std::uint64_t observer_id,
                                    double range) const;

    scene::Scene authoring_scene_{};
    SimulationSession simulation_{};
    scene::ObjectId red_cube_{};
    scene::ObjectId scientist_{};
    scene::ObjectId skeptic_{};
    scene::ObjectId camera_{};
    std::optional<AnomalyRecord> latest_anomaly_{};
    std::string phase_{};
    bool red_cube_present_{false};
};

} // namespace carto::simulation
