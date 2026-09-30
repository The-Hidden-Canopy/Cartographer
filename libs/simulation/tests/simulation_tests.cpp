#include <carto/simulation/simulation.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

carto::scene::Scene make_scene(carto::scene::ObjectId* cube) {
    carto::scene::Scene scene;
    auto created = scene.create_object("Red cube");
    require(static_cast<bool>(created), "authoring scene object must be created");
    *cube = created.value();
    return scene;
}

void test_authoring_scene_becomes_session_projection_without_mutation() {
    carto::scene::ObjectId cube{};
    auto scene = make_scene(&cube);
    const auto scene_revision = scene.revision();
    auto created = carto::simulation::SimulationSession::create(
        scene, {.scenario_id = "gmib-room", .seed = 901});
    require(static_cast<bool>(created), "simulation session must accept a valid authoring scene");
    auto session = std::move(created.value());
    require(session.source_authoring_revision() == scene_revision,
            "session must retain the authoring revision used for its inputs");

    require(static_cast<bool>(session.advance_to(10)), "session must advance monotonically");
    const auto receipt = session.intervene(
        scene, {carto::simulation::InterventionKind::teleport, cube, {5.0, 0.0, 0.0}});
    require(static_cast<bool>(receipt), "teleport intervention must be admitted in the session");
    require(receipt.value().anomaly.source_redacted, "session projection must not store an actor source");
    require(receipt.value().anomaly.magnitude == 5.0,
            "teleport magnitude must reflect the authoring-space displacement");
    require(scene.find(cube)->local_transform.translation.x == 0.0,
            "session intervention must not mutate the authoring scene");

    const carto::simulation::Observer observer{77, {5.0, 0.0, 0.0}, 10.0, 1.0, true};
    const auto observation = session.observe(observer);
    require(observation.quality == carto::simulation::ObservationQuality::valid,
            "nearby observer must receive valid session evidence");
    require(!observation.observed_anomalies.empty(), "observer must see the anomaly evidence");
    require(!observation.visible_objects.empty() && observation.visible_objects.front() == cube,
            "observer must see the projected object");
}

void test_stale_authoring_and_invalid_observer_fail_closed() {
    carto::scene::ObjectId cube{};
    auto scene = make_scene(&cube);
    auto created = carto::simulation::SimulationSession::create(scene);
    require(static_cast<bool>(created), "baseline session must be created");
    auto session = std::move(created.value());
    const auto scene_revision = scene.revision();
    require(static_cast<bool>(scene.set_local_transform(cube, carto::core::Transform{
                .translation = {1.0, 0.0, 0.0},
                .rotation = carto::core::Quaternion::identity(),
                .scale = {1.0, 1.0, 1.0}})), "authoring edit must commit for stale fixture");
    require(scene.revision() != scene_revision, "fixture must advance the authoring revision");
    const auto stale = session.intervene(
        scene, {carto::simulation::InterventionKind::teleport, cube, {2.0, 0.0, 0.0}});
    require(!stale && stale.error().code == carto::core::ErrorCode::stale_data,
            "stale authoring scene must not drive a session intervention");
    require(session.anomalies().empty(), "stale intervention must not append anomaly state");

    const carto::simulation::Observer invalid{
        77, {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}, 10.0, 1.0, true};
    require(session.observe(invalid).quality == carto::simulation::ObservationQuality::blocked,
            "non-finite observer input must be blocked");
}

void test_deterministic_replay_digest_and_identity_guards() {
    carto::scene::ObjectId cube{};
    auto scene = make_scene(&cube);
    auto first_result = carto::simulation::SimulationSession::create(
        scene, {.scenario_id = "replay", .seed = 902});
    auto second_result = carto::simulation::SimulationSession::create(
        scene, {.scenario_id = "replay", .seed = 902});
    require(static_cast<bool>(first_result) && static_cast<bool>(second_result),
            "both replay sessions must be constructible");
    auto first = std::move(first_result.value());
    auto second = std::move(second_result.value());
    require(static_cast<bool>(first.advance_to(3)) && static_cast<bool>(second.advance_to(3)),
            "both sessions must reach the same tick");
    const auto first_receipt = first.intervene(
        scene, {carto::simulation::InterventionKind::teleport, cube, {3.0, 0.0, 0.0}});
    const auto second_receipt = second.intervene(
        scene, {carto::simulation::InterventionKind::teleport, cube, {3.0, 0.0, 0.0}});
    require(static_cast<bool>(first_receipt) && static_cast<bool>(second_receipt),
            "matching replay inputs must be accepted");
    require(first.digest() == second.digest(), "matching session inputs must produce the same digest");

    const auto bad_spawn = first.intervene(
        scene, {carto::simulation::InterventionKind::spawn, cube, {8.0, 0.0, 0.0}});
    require(!bad_spawn && bad_spawn.error().code == carto::core::ErrorCode::invalid_state,
            "spawn must not reuse an existing stable object identity");
    require(first.replay_events().size() == 1U, "rejected intervention must not enter replay history");

    const auto invalid_kind = first.intervene(
        scene, {static_cast<carto::simulation::InterventionKind>(255), cube, {4.0, 0.0, 0.0}});
    require(!invalid_kind && invalid_kind.error().code == carto::core::ErrorCode::invalid_argument,
            "unknown intervention kind must be rejected");
    require(first.replay_events().size() == 1U, "unknown intervention must not enter replay history");
}

void test_overflowing_teleport_is_atomic() {
    carto::scene::Scene scene;
    const auto created = scene.create_object("Overflow fixture", carto::core::Transform{
        .translation = {1.0e308, 0.0, 0.0},
        .rotation = carto::core::Quaternion::identity(),
        .scale = {1.0, 1.0, 1.0}});
    require(static_cast<bool>(created), "finite overflow fixture must be authorable");
    auto session_result = carto::simulation::SimulationSession::create(scene);
    require(static_cast<bool>(session_result), "overflow fixture session must be constructible");
    auto session = std::move(session_result.value());
    const std::string digest_before = session.digest();
    const auto rejected = session.intervene(
        scene, {carto::simulation::InterventionKind::teleport, created.value(), {-1.0e308, 0.0, 0.0}});
    require(!rejected && rejected.error().code == carto::core::ErrorCode::validation_failed,
            "overflowing displacement must be rejected");
    require(session.digest() == digest_before && session.anomalies().empty() &&
                session.replay_events().empty(),
            "overflow rejection must not partially mutate session or replay state");
}

} // namespace

int main() {
    try {
        test_authoring_scene_becomes_session_projection_without_mutation();
        test_stale_authoring_and_invalid_observer_fail_closed();
        test_deterministic_replay_digest_and_identity_guards();
        test_overflowing_teleport_is_atomic();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "cartographer simulation tests passed\n";
    return 0;
}
