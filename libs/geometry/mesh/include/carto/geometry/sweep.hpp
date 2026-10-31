#pragma once

#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/curve_path.hpp>
#include <carto/geometry/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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
    static constexpr std::uint32_t kSchemaVersion = 3U;
    static constexpr std::size_t kMaxIdentityBytes = 128U;
    static constexpr std::size_t kMaxProfilePoints = 64U;
    static constexpr std::size_t kMaxStations = 8'192U;
    static constexpr std::size_t kMaxGeneratedElements = 262'144U;
    static constexpr std::size_t kMaxSerializedBytes = 64U * 1024U;

    std::string source_identity;
    core::Revision source_revision;
    std::string centerline_identity;
    core::Revision centerline_revision;
    // Zero preserves legacy ownerless sources. When set, the centerline and
    // swept result are expressed in this scene object's local coordinate frame.
    std::uint64_t owner_object_id = 0U;
    std::vector<core::Vec2d> profile;
    core::Vec3d reference_up{0.0, 1.0, 0.0};
    std::uint32_t samples_per_segment = 8U;
    double start_scale = 1.0;
    double end_scale = 1.0;
    SweepCapPolicy cap_policy = SweepCapPolicy::both;
    // Zero retains the legacy solid sweep. A value strictly between zero and
    // one adds an inward-facing shell, scaled about the profile origin. The
    // convex outer profile must strictly contain that origin. Caps join the
    // two shells with annular rims; they never fill the bore.
    double inner_profile_scale = 0.0;

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
    std::uint64_t owner_object_id = 0U;
    std::optional<core::Transform> owner_world_transform;
    core::Revision source_project_revision;
    core::Revision owner_scene_revision;
};

// Generates only from the exact source/path pair in the source's local frame.
// Output topology IDs are allocated by EditableMesh and the returned product
// preserves its inputs' identities, revisions, and optional scene owner. The
// pure geometry builder leaves owner_world_transform unresolved; the
// ApplicationSession preview fills it from one current project/scene snapshot.
[[nodiscard]] core::Result<SweptMeshProduct> build_sweep_mesh(
    const SweepSource& source,
    const CurvePath& centerline);

} // namespace carto::geometry
