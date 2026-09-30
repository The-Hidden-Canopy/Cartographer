#pragma once

#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/core/math.hpp>
#include <carto/scene/scene.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace carto::simulation {

enum class InterventionKind : std::uint8_t {
    spawn,
    destroy,
    teleport,
};

[[nodiscard]] const char* to_string(InterventionKind kind) noexcept;

struct SimulationConfig {
    std::string scenario_id{"cartographer-simulation"};
    std::uint64_t seed{0};
};

struct Observer {
    std::uint64_t observer_id{0};
    core::Vec3d position{};
    double range{10.0};
    double attention{1.0};
    bool recording_enabled{true};
};

enum class ObservationQuality : std::uint8_t {
    blocked,
    uncertain,
    valid,
};

[[nodiscard]] const char* to_string(ObservationQuality quality) noexcept;

struct InterventionRequest {
    InterventionKind kind{InterventionKind::teleport};
    scene::ObjectId subject{};
    core::Vec3d destination{};
};

struct AnomalyRecord {
    std::uint64_t anomaly_id{0};
    InterventionKind kind{InterventionKind::teleport};
    std::uint64_t tick{0};
    core::Revision source_authoring_revision{};
    core::Revision before_simulation_revision{};
    core::Revision after_simulation_revision{};
    scene::ObjectId subject{};
    bool before_present{false};
    core::Vec3d before_position{};
    bool after_present{false};
    core::Vec3d after_position{};
    double magnitude{0.0};
    // This is an impact score for derived presentation, not a probability.
    double impact{0.0};
    bool source_redacted{true};
};

struct ObservationRecord {
    std::uint64_t tick{0};
    core::Revision simulation_revision{};
    std::uint64_t observer_id{0};
    ObservationQuality quality{ObservationQuality::blocked};
    double confidence{0.0};
    std::vector<scene::ObjectId> visible_objects;
    std::vector<std::uint64_t> observed_anomalies;
};

struct InterventionReceipt {
    core::Revision before_simulation_revision{};
    core::Revision after_simulation_revision{};
    AnomalyRecord anomaly{};
};

struct ReplayEvent {
    std::uint64_t tick{0};
    core::Revision before_simulation_revision{};
    core::Revision after_simulation_revision{};
    InterventionRequest request{};
};

// A session-local projection over authoring objects. It never mutates the
// source Scene and never promotes simulation output into project truth.
class SimulationSession {
public:
    [[nodiscard]] static core::Result<SimulationSession> create(
        const scene::Scene& authoring_scene,
        SimulationConfig config = {});

    [[nodiscard]] core::Result<void> advance_to(std::uint64_t tick);
    [[nodiscard]] core::Result<InterventionReceipt> intervene(
        const scene::Scene& authoring_scene,
        const InterventionRequest& request);

    [[nodiscard]] ObservationRecord observe(const Observer& observer) const;
    [[nodiscard]] const std::vector<AnomalyRecord>& anomalies() const noexcept { return anomalies_; }
    [[nodiscard]] const std::vector<ReplayEvent>& replay_events() const noexcept { return replay_events_; }
    [[nodiscard]] const std::string& scenario_id() const noexcept { return scenario_id_; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] core::Revision simulation_revision() const noexcept { return simulation_revision_; }
    [[nodiscard]] core::Revision source_authoring_revision() const noexcept {
        return source_authoring_revision_;
    }
    [[nodiscard]] std::string digest() const;

private:
    struct EntityState {
        core::Vec3d position{};
        bool present{true};
    };

    std::string scenario_id_{};
    std::uint64_t seed_{0};
    std::uint64_t tick_{0};
    core::Revision source_authoring_revision_{};
    core::Revision simulation_revision_{};
    std::map<scene::ObjectId, EntityState> entities_{};
    std::vector<AnomalyRecord> anomalies_{};
    std::vector<ReplayEvent> replay_events_{};
};

} // namespace carto::simulation
