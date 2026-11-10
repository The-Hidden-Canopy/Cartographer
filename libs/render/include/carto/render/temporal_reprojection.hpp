#pragma once
#include <array>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <span>
#include <vector>

namespace carto::render {
// Exact object/primitive correspondence, not a color or lossy float/hash ID.
using TemporalSurface = std::array<std::uint64_t, 2>;
struct TemporalPreviousSample {
    // Previous texel-center coordinates (first center0,0), positive camera depth.
    double x = 0, y = 0, depth = 0;
    bool supported = false;
};
enum class TemporalRejection : unsigned {
    accepted,
    unavailable,
    camera_cut,
    revision,
    unsupported_motion,
    offscreen,
    identity,
    depth,
    count
};
struct TemporalReprojectionSettings {
    double feedback = .9, absolute_depth = .01, relative_depth = .005, clip_expansion = .05;
    [[nodiscard]] core::Result<void> validate() const;
};
struct TemporalReprojectionInput {
    std::uint32_t width = 0, height = 0;
    std::span<const core::Vec3d> current_hdr, history_hdr;
    std::span<const double> current_depth, history_depth;
    std::span<const TemporalSurface> current_surface, history_surface;
    std::span<const TemporalPreviousSample> previous;
    core::Revision current_revision, history_revision;
    bool history_valid = false, camera_cut = false, content_changed = false;
    TemporalReprojectionSettings settings;
};
struct TemporalReprojectionResult {
    std::vector<core::Vec3d> linear_hdr;
    // One exclusive reason per pixel: counts sum to width*height.
    std::vector<TemporalRejection> rejection;
    std::array<std::uint64_t, static_cast<unsigned>(TemporalRejection::count)> counts{};
    [[nodiscard]] bool history_used() const { return counts[0] != 0; }
};
// Linear, unexposed HDR only. Bilinear footprints require every contributing
// tap to match identity/depth; invalid taps never bleed into a valid surface.
// Clipping uses the current3x3 same-surface YCoCg bounds. No display transform.
[[nodiscard]] core::Result<TemporalReprojectionResult>
resolve_temporal_reprojected_reference(const TemporalReprojectionInput &);
} // namespace carto::render
