#include <carto/simulation/game.hpp>

#include <limits>
#include <utility>

namespace carto::simulation {

namespace {

[[nodiscard]] core::Transform at(core::Vec3d position) {
    core::Transform transform = core::Transform::identity();
    transform.translation = position;
    return transform;
}

[[nodiscard]] core::Result<GameSession> failure(core::ErrorCode code, std::string message) {
    return core::Result<GameSession>::failure(core::Diagnostic(code, std::move(message)));
}

} // namespace

core::Result<GameSession> GameSession::create(GameConfig config) {
    if (config.scenario_id.empty()) {
        return failure(core::ErrorCode::invalid_argument, "GMIB game scenario id cannot be empty");
    }

    GameSession game;
    game.phase_ = "setup";
    const auto create = [&game](const char* name, core::Vec3d position,
                                scene::ObjectId* output) -> core::Result<void> {
        auto object = game.authoring_scene_.create_object(name, at(position));
        if (!object) return core::Result<void>::failure(object.error());
        *output = object.value();
        return core::Result<void>::success();
    };
    if (auto result = create("Red cube", {3.0, 0.0, 0.0}, &game.red_cube_); !result) {
        return failure(result.error().code, result.error().message);
    }
    if (auto result = create("Scientist", {0.0, 0.0, 0.0}, &game.scientist_); !result) {
        return failure(result.error().code, result.error().message);
    }
    if (auto result = create("Skeptic", {0.0, 0.0, 0.0}, &game.skeptic_); !result) {
        return failure(result.error().code, result.error().message);
    }
    if (auto result = create("Camera", {0.0, 0.0, 0.0}, &game.camera_); !result) {
        return failure(result.error().code, result.error().message);
    }

    auto simulation = SimulationSession::create(
        game.authoring_scene_, SimulationConfig{config.scenario_id, config.seed});
    if (!simulation) {
        return core::Result<GameSession>::failure(simulation.error());
    }
    game.simulation_ = std::move(simulation.value());
    if (auto result = game.simulation_.advance_to(100); !result) {
        return core::Result<GameSession>::failure(result.error());
    }
    game.red_cube_present_ = true;
    game.phase_ = "observation";
    return core::Result<GameSession>::success(std::move(game));
}

Observer GameSession::observer(scene::ObjectId object, std::uint64_t observer_id,
                               double range) const {
    const auto transform = authoring_scene_.world_transform(object);
    return Observer{observer_id,
                    transform ? transform.value().translation : core::Vec3d{},
                    range,
                    1.0,
                    true};
}

core::Result<void> GameSession::step() {
    if (simulation_.tick() == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "GMIB game tick space is exhausted"));
    }
    auto result = simulation_.advance_to(simulation_.tick() + 1U);
    if (result) phase_ = "world-tick";
    return result;
}

core::Result<InterventionReceipt> GameSession::teleport_cube(core::Vec3d destination) {
    if (!red_cube_present_) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "GMIB game cube is already absent"));
    }
    const auto result = simulation_.intervene(
        authoring_scene_, {InterventionKind::teleport, red_cube_, destination});
    if (!result) return result;
    latest_anomaly_ = result.value().anomaly;
    phase_ = "anomaly-detected";
    return result;
}

core::Result<InterventionReceipt> GameSession::destroy_cube() {
    if (!red_cube_present_) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "GMIB game cube is already absent"));
    }
    const auto result = simulation_.intervene(
        authoring_scene_, {InterventionKind::destroy, red_cube_, {}});
    if (!result) return result;
    red_cube_present_ = false;
    latest_anomaly_ = result.value().anomaly;
    phase_ = "history-scar";
    return result;
}

GameFrame GameSession::frame() const {
    GameFrame result{};
    result.tick = simulation_.tick();
    result.simulation_revision = simulation_.simulation_revision();
    result.phase = phase_;
    result.red_cube_present = red_cube_present_;
    result.latest_anomaly = latest_anomaly_;
    result.scientist = simulation_.observe(observer(scientist_, 1, 20.0));
    result.skeptic = simulation_.observe(observer(skeptic_, 2, 5.0));
    result.camera = simulation_.observe(observer(camera_, 3, 100.0));
    return result;
}

} // namespace carto::simulation
