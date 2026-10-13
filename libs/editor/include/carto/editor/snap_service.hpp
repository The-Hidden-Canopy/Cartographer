#pragma once

#include <carto/editor/snap_candidates.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace carto::editor {

// Bounded hysteresis policy for geometry candidates. The profile is pure UI
// state; it does not choose or apply an authoring command.
struct SnapProfile final {
    bool enabled = true;
    double acquire_radius = 0.25;
    double retain_radius = 0.40;
    double break_radius = 0.75;
    double switch_margin = 0.05;
    // Lower array index wins before distance. All three currently supported
    // candidate kinds must appear exactly once.
    std::array<SnapKind, 3> priority{
        SnapKind::vertex, SnapKind::edge_midpoint, SnapKind::face_center};

    [[nodiscard]] core::Result<void> validate() const;
};

// A held snap is transient interaction state. Its source revision must match
// the current query before it can be retained.
class SnapSession final {
public:
    void reset() noexcept { lock_.reset(); }
    [[nodiscard]] bool locked() const noexcept { return lock_.has_value(); }
    [[nodiscard]] const std::optional<SnapCandidate>& lock() const noexcept {
        return lock_;
    }

private:
    friend core::Result<struct SnapDecision> acquire_snap(
        std::span<const SnapCandidate>, core::Vec3d, core::Revision,
        const SnapProfile&, SnapSession&);
    std::optional<SnapCandidate> lock_;
};

struct SnapDecision final {
    std::optional<SnapCandidate> candidate;
    bool retained = false;
    bool switched = false;
};

// Applies a revision-bound priority/hysteresis policy to already compiled
// candidates. Candidate positions are remeasured against `origin`; caller
// supplied distances are never trusted as authority.
[[nodiscard]] core::Result<SnapDecision> acquire_snap(
    std::span<const SnapCandidate> candidates,
    core::Vec3d origin,
    core::Revision source_revision,
    const SnapProfile& profile,
    SnapSession& session);

} // namespace carto::editor
