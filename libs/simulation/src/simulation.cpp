#include <carto/simulation/simulation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace carto::simulation {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void mix(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        hash ^= (value >> (index * 8U)) & 0xffULL;
        hash *= kFnvPrime;
    }
}

void mix(std::uint64_t& hash, double value) noexcept {
    mix(hash, std::bit_cast<std::uint64_t>(value));
}

void mix(std::uint64_t& hash, const core::Vec3d& value) noexcept {
    mix(hash, value.x);
    mix(hash, value.y);
    mix(hash, value.z);
}

void mix(std::uint64_t& hash, scene::ObjectId id) noexcept {
    mix(hash, id.value);
}

void mix(std::uint64_t& hash, std::string_view value) noexcept {
    mix(hash, static_cast<std::uint64_t>(value.size()));
    for (const char byte : value) {
        mix(hash, static_cast<std::uint64_t>(static_cast<unsigned char>(byte)));
    }
}

[[nodiscard]] double distance(core::Vec3d left, core::Vec3d right) noexcept {
    return (right - left).length();
}

[[nodiscard]] core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, std::move(message)));
}

} // namespace

const char* to_string(InterventionKind kind) noexcept {
    switch (kind) {
    case InterventionKind::spawn:
        return "spawn";
    case InterventionKind::destroy:
        return "destroy";
    case InterventionKind::teleport:
        return "teleport";
    }
    return "unknown";
}

const char* to_string(ObservationQuality quality) noexcept {
    switch (quality) {
    case ObservationQuality::blocked:
        return "blocked";
    case ObservationQuality::uncertain:
        return "uncertain";
    case ObservationQuality::valid:
        return "valid";
    }
    return "unknown";
}

core::Result<SimulationSession> SimulationSession::create(
    const scene::Scene& authoring_scene,
    SimulationConfig config) {
    if (config.scenario_id.empty()) return core::Result<SimulationSession>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, "simulation scenario id cannot be empty"));
    if (auto validation = authoring_scene.validate(); !validation) {
        return core::Result<SimulationSession>::failure(validation.error().with_context("simulation source scene"));
    }

    SimulationSession session;
    session.scenario_id_ = std::move(config.scenario_id);
    session.seed_ = config.seed;
    session.source_authoring_revision_ = authoring_scene.revision();
    for (const scene::SceneObject& object : authoring_scene.objects_sorted()) {
        const auto transform = authoring_scene.world_transform(object.id);
        if (!transform) {
            return core::Result<SimulationSession>::failure(transform.error().with_context(
                "simulation source world transform"));
        }
        if (!transform.value().finite()) {
            return core::Result<SimulationSession>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed, "simulation source contains a non-finite transform"));
        }
        session.entities_.emplace(object.id, EntityState{transform.value().translation, true});
    }
    return core::Result<SimulationSession>::success(std::move(session));
}

core::Result<void> SimulationSession::advance_to(std::uint64_t tick) {
    if (tick < tick_) return invalid("simulation ticks cannot move backwards");
    tick_ = tick;
    return core::Result<void>::success();
}

core::Result<InterventionReceipt> SimulationSession::intervene(
    const scene::Scene& authoring_scene,
    const InterventionRequest& request) {
    if (authoring_scene.revision() != source_authoring_revision_) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "simulation intervention source scene revision is stale"));
    }
    if (request.subject.value == 0 || !request.destination.finite()) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "simulation intervention requires a valid subject and finite destination"));
    }
    if (request.kind != InterventionKind::spawn && request.kind != InterventionKind::destroy &&
        request.kind != InterventionKind::teleport) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "simulation intervention kind is unsupported"));
    }
    if (simulation_revision_.exhausted()) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "simulation revision space is exhausted"));
    }

    const auto iterator = entities_.find(request.subject);
    if (request.kind == InterventionKind::spawn) {
        if (iterator != entities_.end()) {
            return core::Result<InterventionReceipt>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "simulation spawn would reuse an existing object identity"));
        }
    } else if (iterator == entities_.end() || !iterator->second.present) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "simulation intervention subject is not present"));
    }

    const core::Revision before_revision = simulation_revision_;
    const auto before_position = iterator == entities_.end() ? core::Vec3d{} : iterator->second.position;
    const bool before_present = iterator != entities_.end() && iterator->second.present;
    const bool after_present = request.kind != InterventionKind::destroy;
    const core::Vec3d after_position = request.kind == InterventionKind::destroy
        ? before_position
        : request.destination;
    const double magnitude = request.kind == InterventionKind::teleport
        ? distance(before_position, after_position)
        : 1.0;
    if (!std::isfinite(magnitude)) {
        return core::Result<InterventionReceipt>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "simulation intervention magnitude is non-finite"));
    }

    switch (request.kind) {
    case InterventionKind::spawn:
        entities_.emplace(request.subject, EntityState{after_position, true});
        break;
    case InterventionKind::destroy:
        iterator->second.present = false;
        break;
    case InterventionKind::teleport:
        iterator->second.position = after_position;
        break;
    }
    simulation_revision_ = simulation_revision_.next();

    AnomalyRecord anomaly{};
    anomaly.anomaly_id = static_cast<std::uint64_t>(anomalies_.size()) + 1U;
    anomaly.kind = request.kind;
    anomaly.tick = tick_;
    anomaly.source_authoring_revision = source_authoring_revision_;
    anomaly.before_simulation_revision = before_revision;
    anomaly.after_simulation_revision = simulation_revision_;
    anomaly.subject = request.subject;
    anomaly.before_present = before_present;
    anomaly.before_position = before_position;
    anomaly.after_present = after_present;
    anomaly.after_position = after_position;
    anomaly.magnitude = magnitude;
    anomaly.impact = std::min(1.0, anomaly.magnitude / 10.0);
    anomalies_.push_back(anomaly);
    replay_events_.push_back({tick_, before_revision, simulation_revision_, request});
    return core::Result<InterventionReceipt>::success({before_revision, simulation_revision_, anomaly});
}

ObservationRecord SimulationSession::observe(const Observer& observer) const {
    ObservationRecord result{};
    result.tick = tick_;
    result.simulation_revision = simulation_revision_;
    result.observer_id = observer.observer_id;
    if (observer.observer_id == 0 || !observer.position.finite() || !std::isfinite(observer.range) ||
        observer.range < 0.0 || !std::isfinite(observer.attention) || observer.attention < 0.0 ||
        observer.attention > 1.0 || !observer.recording_enabled || observer.attention == 0.0) {
        return result;
    }

    auto confidence_for = [&observer](core::Vec3d position) {
        const double distance_to_observer = distance(observer.position, position);
        if (observer.range == 0.0) return distance_to_observer == 0.0 ? 1.0 : 0.0;
        if (distance_to_observer > observer.range) return 0.0;
        return std::max(0.0, 1.0 - distance_to_observer / observer.range);
    };
    double confidence = 0.0;
    for (const auto& [id, state] : entities_) {
        if (!state.present) continue;
        const double geometric_confidence = confidence_for(state.position);
        if (geometric_confidence <= 0.0) continue;
        result.visible_objects.push_back(id);
        confidence = std::max(confidence, geometric_confidence * observer.attention);
    }
    for (const AnomalyRecord& anomaly : anomalies_) {
        if ((anomaly.before_present && confidence_for(anomaly.before_position) > 0.0) ||
            (anomaly.after_present && confidence_for(anomaly.after_position) > 0.0)) {
            result.observed_anomalies.push_back(anomaly.anomaly_id);
            confidence = std::max(confidence, observer.attention);
        }
    }
    result.confidence = confidence;
    result.quality = confidence >= 0.5 ? ObservationQuality::valid : ObservationQuality::uncertain;
    return result;
}

std::string SimulationSession::digest() const {
    std::uint64_t hash = kFnvOffset;
    mix(hash, seed_);
    mix(hash, tick_);
    mix(hash, source_authoring_revision_.value());
    mix(hash, simulation_revision_.value());
    mix(hash, scenario_id_);
    for (const auto& [id, state] : entities_) {
        mix(hash, id);
        mix(hash, static_cast<std::uint64_t>(state.present));
        mix(hash, state.position);
    }
    for (const AnomalyRecord& anomaly : anomalies_) {
        mix(hash, anomaly.anomaly_id);
        mix(hash, static_cast<std::uint64_t>(anomaly.kind));
        mix(hash, anomaly.tick);
        mix(hash, anomaly.source_authoring_revision.value());
        mix(hash, anomaly.before_simulation_revision.value());
        mix(hash, anomaly.after_simulation_revision.value());
        mix(hash, anomaly.subject);
        mix(hash, static_cast<std::uint64_t>(anomaly.before_present));
        mix(hash, anomaly.before_position);
        mix(hash, static_cast<std::uint64_t>(anomaly.after_present));
        mix(hash, anomaly.after_position);
    }
    for (const ReplayEvent& event : replay_events_) {
        mix(hash, event.tick);
        mix(hash, event.before_simulation_revision.value());
        mix(hash, event.after_simulation_revision.value());
        mix(hash, static_cast<std::uint64_t>(event.request.kind));
        mix(hash, event.request.subject);
        mix(hash, event.request.destination);
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

} // namespace carto::simulation
