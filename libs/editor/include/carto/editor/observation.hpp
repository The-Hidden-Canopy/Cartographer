#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/core/revision.hpp>
#include <carto/geometry/curve_path.hpp>
#include <carto/geometry/ids.hpp>
#include <carto/render/render_scene.hpp>
#include <carto/scene/scene.hpp>
#include <carto/terrain/terrain_tile.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::editor {

struct PointSightlineQuery final {
    core::Vec3d observer{};
    core::Vec3d target{};
    core::Revision source_revision;
    std::optional<scene::ObjectId> observer_object;
    std::optional<scene::ObjectId> target_object;
};

struct PointSightlineOptions final {
    double endpoint_epsilon = 1.0e-6;
    double parallel_epsilon = 1.0e-12;
    double max_distance = 1.0e6;
    std::uint32_t max_occluders = 4096U;
    std::uint32_t max_triangles = 262144U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PointSightlineResult final {
    static constexpr std::string_view kAlgorithm = "carto.point-sightline/1";

    std::string_view algorithm = kAlgorithm;
    core::Revision source_revision;
    core::Vec3d observer{};
    core::Vec3d target{};
    double segment_length = 0.0;
    bool visible = true;
    std::optional<scene::ObjectId> blocker_object;
    std::optional<std::uint64_t> blocker_mesh_asset;
    std::optional<core::Revision> blocker_mesh_revision;
    std::optional<geometry::FaceId> blocker_face;
    double blocker_distance = 0.0;
    std::uint32_t triangles_tested = 0U;

    // A result is usable only against the exact project snapshot queried.
    [[nodiscard]] core::Result<void> validate(
        const PointSightlineQuery& query,
        const PointSightlineOptions& options,
        core::Revision current_source_revision) const;
};

// Read-only point-to-point visibility over a captured render scene. The
// observer and target objects may be excluded to avoid self-occlusion. Every
// tested instance must carry an asset identity and a mesh revision matching
// its immutable compiled-mesh snapshot; budget or provenance failures reject
// the whole query instead of returning a partial visibility answer.
[[nodiscard]] core::Result<PointSightlineResult> evaluate_point_sightline(
    const render::RenderScene& scene,
    const PointSightlineQuery& query,
    const PointSightlineOptions& options = {});

struct RouteVisibilityQuery final {
    // Ordered observer stations along the authored route. Results describe
    // visibility from these exact samples; no unrequested interpolation is
    // implied.
    std::vector<core::Vec3d> observer_positions;
    std::vector<std::optional<scene::ObjectId>> observer_objects;
    core::Vec3d target{};
    core::Revision source_revision;
    std::optional<scene::ObjectId> target_object;
};

struct RouteVisibilityOptions final {
    PointSightlineOptions sightline{};
    std::uint32_t max_samples = 256U;
    double max_route_length = 1.0e6;
    std::uint64_t max_total_triangles = 1'000'000U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct RouteVisibilityResult final {
    static constexpr std::string_view kAlgorithm = "carto.route-landmark-visibility/1";

    std::string_view algorithm = kAlgorithm;
    core::Revision source_revision;
    core::Vec3d target{};
    std::vector<double> cumulative_route_distance;
    std::vector<PointSightlineResult> samples;
    double route_length = 0.0;
    std::uint64_t triangles_tested = 0U;

    [[nodiscard]] core::Result<void> validate(
        const RouteVisibilityQuery& query,
        const RouteVisibilityOptions& options,
        core::Revision current_source_revision) const;
};

// Route visibility query derived from one exact, authored cubic centerline.
// The curve receipt and project revision are separate: changing any project
// source invalidates the viewport snapshot, while changing this curve also
// invalidates its identity/revision/digest binding.
struct CurveRouteVisibilityQuery final {
    std::string curve_identity;
    core::Revision curve_revision;
    assets::Sha256Digest curve_digest;
    std::uint32_t samples_per_segment = 0U;
    std::optional<scene::ObjectId> observer_object;
    std::optional<scene::ObjectId> target_object;
    RouteVisibilityQuery route;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CurvePath& curve,
        core::Revision current_project_revision,
        const RouteVisibilityOptions& options = {}) const;
};

struct CurveRouteVisibilityResult final {
    CurveRouteVisibilityQuery query;
    RouteVisibilityResult visibility;

    [[nodiscard]] core::Result<void> validate(
        const geometry::CurvePath& curve,
        core::Revision current_project_revision,
        const RouteVisibilityOptions& options = {}) const;
};

[[nodiscard]] core::Result<CurveRouteVisibilityQuery>
build_curve_route_visibility_query(
    const geometry::CurvePath& curve,
    core::Vec3d target,
    core::Revision project_revision,
    std::uint32_t samples_per_segment,
    std::optional<scene::ObjectId> observer_object = std::nullopt,
    std::optional<scene::ObjectId> target_object = std::nullopt,
    const RouteVisibilityOptions& options = {});

[[nodiscard]] core::Result<CurveRouteVisibilityResult>
evaluate_curve_route_visibility(
    const render::RenderScene& scene,
    const geometry::CurvePath& curve,
    const CurveRouteVisibilityQuery& query,
    core::Revision current_project_revision,
    const RouteVisibilityOptions& options = {});

// Measures visibility of one landmark from explicitly supplied route
// stations. The result is all-or-nothing: invalid/stale station geometry or
// aggregate work beyond budget rejects the complete query, never a partial
// visibility mask.
[[nodiscard]] core::Result<RouteVisibilityResult> evaluate_route_visibility(
    const render::RenderScene& scene,
    const RouteVisibilityQuery& query,
    const RouteVisibilityOptions& options = {});

struct LandmarkViewshedQuery final {
    core::Vec3d observer{};
    std::optional<scene::ObjectId> observer_object;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    // Exact target positions in row-major grid order. Elevation is supplied
    // by the caller from a current terrain/source sample; it is never guessed.
    std::vector<core::Vec3d> target_positions;
    core::Revision source_revision;
};

// A bounded sample grid derived from one validated terrain product. Target
// heights are evaluated on the product's existing triangulated surface and
// remain bound to its source receipt and the project revision.
struct TerrainLandmarkViewshedGrid final {
    static constexpr std::uint32_t kMaxAxis = 32U;

    std::string terrain_identity;
    core::Revision terrain_revision;
    assets::Sha256Digest terrain_digest;
    std::uint32_t terrain_lod_step = 0U;
    LandmarkViewshedQuery query;

    [[nodiscard]] core::Result<void> validate(
        const terrain::TerrainTileProduct& terrain_product,
        core::Revision current_project_revision) const;
};

[[nodiscard]] core::Result<TerrainLandmarkViewshedGrid>
build_terrain_landmark_viewshed_grid(
    const terrain::TerrainTileProduct& terrain_product,
    core::Vec3d observer,
    core::Revision project_revision,
    std::optional<scene::ObjectId> observer_object = std::nullopt,
    std::uint32_t max_axis = TerrainLandmarkViewshedGrid::kMaxAxis);

struct LandmarkViewshedOptions final {
    PointSightlineOptions sightline{};
    std::uint32_t max_cells = 4096U;
    std::uint64_t max_total_triangles = 1'000'000U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct LandmarkViewshedResult final {
    static constexpr std::string_view kAlgorithm = "carto.landmark-viewshed/1";

    std::string_view algorithm = kAlgorithm;
    core::Revision source_revision;
    core::Vec3d observer{};
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<PointSightlineResult> cells;
    std::uint32_t visible_cells = 0U;
    std::uint64_t triangles_tested = 0U;

    [[nodiscard]] core::Result<void> validate(
        const LandmarkViewshedQuery& query,
        const LandmarkViewshedOptions& options,
        core::Revision current_source_revision) const;
};

// Evaluates a bounded row-major set of exact target positions from one
// landmark observer. Results are an all-or-nothing geometric visibility mask
// suitable for an authoring heatmap; terrain interpolation and renderer
// visibility are intentionally outside this query.
[[nodiscard]] core::Result<LandmarkViewshedResult> evaluate_landmark_viewshed(
    const render::RenderScene& scene,
    const LandmarkViewshedQuery& query,
    const LandmarkViewshedOptions& options = {});

enum class ObservationTargetKind : std::uint8_t {
    generic_object,
    landmark,
    sign,
    interaction,
    clutter,
    biological_reference,
};

enum class ObservationDisposition : std::uint8_t {
    filtered_by_kind,
    outside_range,
    outside_view_cone,
    visible,
    occluded,
};

[[nodiscard]] constexpr std::uint32_t observation_target_kind_bit(
    ObservationTargetKind kind) noexcept {
    switch (kind) {
    case ObservationTargetKind::generic_object: return 1U << 0U;
    case ObservationTargetKind::landmark: return 1U << 1U;
    case ObservationTargetKind::sign: return 1U << 2U;
    case ObservationTargetKind::interaction: return 1U << 3U;
    case ObservationTargetKind::clutter: return 1U << 4U;
    case ObservationTargetKind::biological_reference: return 1U << 5U;
    }
    return 0U;
}

inline constexpr std::uint32_t kAllObservationTargetKinds = (1U << 6U) - 1U;

struct ObservationTarget final {
    // Caller-supplied stable identity (for example an authored anchor ID).
    std::uint64_t identity = 0U;
    scene::ObjectId object;
    core::Vec3d position{};
    ObservationTargetKind kind = ObservationTargetKind::generic_object;
};

struct AuthoringObservationQuery final {
    core::Vec3d observer{};
    core::Vec3d forward{0.0, 0.0, -1.0};
    core::Vec3d up{0.0, 1.0, 0.0};
    double horizontal_fov_radians = 1.5707963267948966;
    double vertical_fov_radians = 1.0471975511965976;
    core::Revision source_revision;
    std::optional<scene::ObjectId> observer_object;
    // Candidate targets and positions are supplied by the authoring source;
    // this query never invents object centers or semantic classifications.
    std::vector<ObservationTarget> targets;
};

struct AuthoringObservationOptions final {
    PointSightlineOptions sightline{};
    double max_distance = 1000.0;
    std::uint32_t included_target_kinds = kAllObservationTargetKinds;
    std::uint32_t max_targets = 256U;
    std::uint64_t max_total_triangles = 1'000'000U;

    [[nodiscard]] core::Result<void> validate() const;
};

struct ObservationProjectionSample final {
    std::uint64_t target_identity = 0U;
    scene::ObjectId object;
    core::Vec3d position{};
    ObservationTargetKind kind = ObservationTargetKind::generic_object;
    ObservationDisposition disposition = ObservationDisposition::outside_view_cone;
    double distance = 0.0;
    double horizontal_bearing_radians = 0.0;
    double vertical_bearing_radians = 0.0;
    std::optional<PointSightlineResult> sightline;
};

struct AuthoringObservationResult final {
    static constexpr std::string_view kAlgorithm = "carto.authoring-observation/1";

    std::string_view algorithm = kAlgorithm;
    core::Revision source_revision;
    core::Vec3d observer{};
    core::Vec3d forward{};
    core::Vec3d right{};
    core::Vec3d up{};
    double horizontal_fov_radians = 0.0;
    double vertical_fov_radians = 0.0;
    std::vector<ObservationProjectionSample> targets;
    std::uint32_t visible_targets = 0U;
    std::uint32_t occluded_targets = 0U;
    std::uint32_t filtered_targets = 0U;
    std::uint32_t outside_range_targets = 0U;
    std::uint32_t outside_view_cone_targets = 0U;
    std::uint64_t triangles_tested = 0U;

    [[nodiscard]] core::Result<void> validate(
        const AuthoringObservationQuery& query,
        const AuthoringObservationOptions& options,
        core::Revision current_source_revision) const;
};

// Projects an explicit, semantically typed target list through a bounded
// observer cone, then performs revision-bound geometric visibility only for
// admitted targets. This is a deterministic authoring report, not runtime
// perception, sign-legibility, or renderer-visibility truth.
[[nodiscard]] core::Result<AuthoringObservationResult>
evaluate_authoring_observation(
    const render::RenderScene& scene,
    const AuthoringObservationQuery& query,
    const AuthoringObservationOptions& options = {});

} // namespace carto::editor
