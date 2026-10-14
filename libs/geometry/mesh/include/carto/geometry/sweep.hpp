#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/curve_path.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace carto::geometry {

enum class SweepCapPolicy : std::uint8_t {
    open = 0U,
    both = 1U,
    start = 2U,
    end = 3U,
};

// Serializable Cartographer-owned source for sweeping a strictly convex
// planar profile along an exact, revision-bound CurvePath. `reference_up`
// establishes the initial profile frame; it is not a global axis convention.
struct SweepSource final {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxProfilePoints = 64U;
    static constexpr std::size_t kMaxStations = 8'192U;
    static constexpr std::size_t kMaxGeneratedElements = 262'144U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    std::string source_identity;
    core::Revision source_revision;
    std::string centerline_identity;
    core::Revision centerline_revision;
    std::vector<core::Vec2d> profile;
    core::Vec3d reference_up{0.0, 1.0, 0.0};
    std::uint32_t samples_per_segment = 8U;
    double start_scale = 1.0;
    double end_scale = 1.0;
    SweepCapPolicy cap_policy = SweepCapPolicy::both;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<SweepSource> deserialize(std::string_view text);
};

struct SweptMeshProduct final {
    EditableMesh mesh;
    std::string source_identity;
    core::Revision source_revision;
    std::string centerline_identity;
    core::Revision centerline_revision;
};

// Generates only from the exact source/path pair. Output topology IDs are
// allocated by EditableMesh and the returned product preserves its inputs'
// identities and revisions for callers that persist or review the result.
[[nodiscard]] core::Result<SweptMeshProduct> build_sweep_mesh(
    const SweepSource& source,
    const CurvePath& centerline);

} // namespace carto::geometry
