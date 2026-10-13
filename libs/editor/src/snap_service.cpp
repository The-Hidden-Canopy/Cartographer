#include <carto/editor/snap_service.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::editor {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

bool supported_kind(SnapKind kind) noexcept {
    return kind == SnapKind::vertex || kind == SnapKind::edge_midpoint ||
        kind == SnapKind::face_center;
}

std::uint64_t candidate_id(const SnapCandidate& candidate) noexcept {
    if (candidate.vertex.has_value()) return candidate.vertex->value;
    if (candidate.edge.has_value()) return candidate.edge->value;
    if (candidate.face.has_value()) return candidate.face->value;
    return 0U;
}

std::tuple<std::uint64_t, std::uint64_t, std::uint8_t, std::uint64_t> identity(
    const SnapCandidate& candidate) noexcept {
    return {
        candidate.object.has_value() ? candidate.object->value : 0U,
        candidate.mesh_asset.value_or(0U),
        static_cast<std::uint8_t>(candidate.kind),
        candidate_id(candidate)};
}

bool same_identity(const SnapCandidate& left, const SnapCandidate& right) noexcept {
    return identity(left) == identity(right);
}

core::Result<void> validate_candidate(const SnapCandidate& candidate) {
    if (!supported_kind(candidate.kind) || !candidate.position.finite() ||
        !std::isfinite(candidate.distance) || candidate.distance < 0.0) {
        return core::Result<void>::failure(invalid(
            "snap service candidate has invalid kind, position, or distance"));
    }
    const int identity_count = static_cast<int>(candidate.vertex.has_value()) +
        static_cast<int>(candidate.edge.has_value()) +
        static_cast<int>(candidate.face.has_value());
    const bool matching_identity =
        (candidate.kind == SnapKind::vertex && candidate.vertex.has_value() &&
            !candidate.edge.has_value() && !candidate.face.has_value()) ||
        (candidate.kind == SnapKind::edge_midpoint && candidate.edge.has_value() &&
            !candidate.vertex.has_value() && !candidate.face.has_value()) ||
        (candidate.kind == SnapKind::face_center && candidate.face.has_value() &&
            !candidate.vertex.has_value() && !candidate.edge.has_value());
    const bool matching_scene_owner =
        candidate.object.has_value() == candidate.mesh_asset.has_value() &&
        (!candidate.object.has_value() ||
            (static_cast<bool>(*candidate.object) && *candidate.mesh_asset != 0U));
    if (identity_count != 1 || !matching_identity || !matching_scene_owner) {
        return core::Result<void>::failure(invalid(
            "snap service candidate identity or scene owner is invalid"));
    }
    return core::Result<void>::success();
}

int priority_of(const SnapProfile& profile, SnapKind kind) noexcept {
    for (std::size_t index = 0U; index < profile.priority.size(); ++index) {
        if (profile.priority[index] == kind) return static_cast<int>(index);
    }
    return static_cast<int>(profile.priority.size());
}

bool precedes(const SnapProfile& profile,
    const SnapCandidate& left, double left_distance,
    const SnapCandidate& right, double right_distance) noexcept {
    const auto left_score = std::tuple{
        priority_of(profile, left.kind), left_distance, identity(left)};
    const auto right_score = std::tuple{
        priority_of(profile, right.kind), right_distance, identity(right)};
    return left_score < right_score;
}

} // namespace

core::Result<void> SnapProfile::validate() const {
    if (!std::isfinite(acquire_radius) || acquire_radius <= 0.0 ||
        !std::isfinite(retain_radius) || retain_radius < acquire_radius ||
        !std::isfinite(break_radius) || break_radius < retain_radius ||
        !std::isfinite(switch_margin) || switch_margin < 0.0) {
        return core::Result<void>::failure(invalid(
            "snap profile radii or switch margin are invalid"));
    }
    std::array<bool, 3> seen{};
    for (const auto kind : priority) {
        if (!supported_kind(kind)) {
            return core::Result<void>::failure(invalid(
                "snap profile contains an unsupported candidate priority"));
        }
        const auto slot = kind == SnapKind::vertex ? 0U :
            kind == SnapKind::edge_midpoint ? 1U : 2U;
        if (seen[slot]) {
            return core::Result<void>::failure(invalid(
                "snap profile candidate priorities must be unique"));
        }
        seen[slot] = true;
    }
    return core::Result<void>::success();
}

core::Result<SnapDecision> acquire_snap(
    std::span<const SnapCandidate> candidates,
    core::Vec3d origin,
    core::Revision source_revision,
    const SnapProfile& profile,
    SnapSession& session) {
    if (!origin.finite()) {
        return core::Result<SnapDecision>::failure(invalid(
            "snap service origin must be finite"));
    }
    if (auto result = profile.validate(); !result) {
        return core::Result<SnapDecision>::failure(result.error());
    }
    if (session.lock_.has_value() &&
        session.lock_->source_revision != source_revision) {
        return core::Result<SnapDecision>::failure(stale(
            "snap lock belongs to a different source revision"));
    }
    if (!profile.enabled) {
        session.reset();
        return core::Result<SnapDecision>::success({});
    }

    struct Measured final {
        SnapCandidate candidate;
        double distance = 0.0;
    };
    std::vector<Measured> measured;
    measured.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (auto result = validate_candidate(candidate); !result) {
            return core::Result<SnapDecision>::failure(result.error());
        }
        if (candidate.source_revision != source_revision) {
            return core::Result<SnapDecision>::failure(stale(
                "snap candidate belongs to a different source revision"));
        }
        const double distance = (candidate.position - origin).length();
        if (!std::isfinite(distance)) {
            return core::Result<SnapDecision>::failure(invalid(
                "snap candidate distance is non-finite"));
        }
        measured.push_back({candidate, distance});
    }

    const auto best_within = [&]() -> std::optional<Measured> {
        std::optional<Measured> best;
        for (const auto& item : measured) {
            if (item.distance > profile.acquire_radius) continue;
            if (!best.has_value() || precedes(profile, item.candidate, item.distance,
                    best->candidate, best->distance)) {
                best = item;
            }
        }
        return best;
    };

    SnapDecision decision;
    const auto best = best_within();
    const bool had_lock = session.lock_.has_value();
    if (session.lock_.has_value()) {
        const auto locked = std::find_if(measured.begin(), measured.end(),
            [&session](const Measured& item) {
                return same_identity(item.candidate, *session.lock_);
            });
        if (locked != measured.end() && locked->distance <= profile.break_radius) {
            const bool competitor_wins = best.has_value() &&
                !same_identity(best->candidate, locked->candidate) &&
                best->distance + profile.switch_margin < locked->distance;
            if (!competitor_wins) {
                decision.candidate = locked->candidate;
                decision.candidate->distance = locked->distance;
                decision.retained = true;
                session.lock_ = decision.candidate;
                return core::Result<SnapDecision>::success(std::move(decision));
            }
        }
        session.reset();
    }

    if (best.has_value()) {
        decision.candidate = best->candidate;
        decision.candidate->distance = best->distance;
        decision.switched = had_lock;
        session.lock_ = decision.candidate;
    }
    return core::Result<SnapDecision>::success(std::move(decision));
}

} // namespace carto::editor
