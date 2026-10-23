#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::geometry {

struct CubicBezierSegment final {
    core::Vec3d p0;
    core::Vec3d p1;
    core::Vec3d p2;
    core::Vec3d p3;
};

struct CurveSample final {
    core::Vec3d position;
    core::Vec3d tangent;
    std::uint32_t segment_index = 0U;
    double parameter = 0.0;
};

// Provider-neutral authored cubic path. Adjacent segments share an exact
// endpoint; boundary samples are retained per segment so a deliberate tangent
// discontinuity is not hidden from downstream road/utility generators.
class CurvePath final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxSegments = 4'096U;
    static constexpr std::size_t kMaxSamples = 65'536U;
    static constexpr std::size_t kMaxSerializedBytes = 8U * 1024U * 1024U;
    static constexpr std::uint32_t kMaxSamplesPerSegment = 4'096U;
    static constexpr double kMaxCoordinateMagnitude = 1.0e9;

    std::string source_identity;
    core::Revision source_revision;
    std::vector<CubicBezierSegment> segments;

    [[nodiscard]] core::Result<void> validate() const;
    // `samples_per_segment` partitions each segment's parameter interval [0,1]
    // uniformly; it is not an equal-arc-length guarantee. Both sides of a
    // segment joint are retained so their tangents can differ explicitly.
    [[nodiscard]] core::Result<std::vector<CurveSample>> sample_uniform(
        std::uint32_t samples_per_segment) const;
    // Adaptive equal-arc-length sampling: each segment is subdivided
    // deterministically by chord/flatness error, integrated into a cumulative
    // arc-length table, and resampled at stations spaced no farther apart
    // than `target_spacing`. Emission fails closed when the bounded sample
    // budget cannot honor the requested spacing.
    [[nodiscard]] core::Result<std::vector<CurveSample>> sample_arc_length(
        double target_spacing) const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<CurvePath> deserialize(std::string_view text);
};

} // namespace carto::geometry
