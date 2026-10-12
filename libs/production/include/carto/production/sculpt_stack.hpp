#pragma once

#include <carto/geometry/mesh.hpp>
#include <carto/production/bake_graph.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace carto::project {
class ProjectDocument;
class ProjectTransaction;
}

namespace carto::production {

inline constexpr std::uint32_t kSculptStackVersion = 4U;
inline constexpr std::uint32_t kSculptStackMinimumReadableVersion = 1U;
inline constexpr std::size_t kMaxSculptLayers = 64U;
inline constexpr std::size_t kMaxSculptStrokesPerLayer = 4096U;
inline constexpr std::size_t kMaxSculptSamplesPerStroke = 2048U;
inline constexpr std::uint64_t kMaxSculptKernelPairs = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxSculptVertices = 1'000'000ULL;
inline constexpr std::uint32_t kMaxSculptRadialSymmetryCopies = 32U;

enum class SculptStrokeMode : std::uint8_t {
    add = 0U,
    subtract = 1U,
    inflate = 2U,
    clay = 3U,
    grab = 4U,
    smooth = 5U,
    pinch = 6U,
    flatten = 7U,
    scrape = 8U,
    polish = 9U,
    crease = 10U,
    surface_noise = 11U,
};

enum class SculptSymmetryMode : std::uint8_t {
    none = 0U,
    mirror_x = 1U,
    mirror_plane = 2U,
    radial = 3U,
};

enum class SculptStrokeDirectionMode : std::uint8_t {
    surface_normal = 0U,
    surface_tangent = 1U,
    view_aligned = 2U,
};

struct SculptSurfaceAnchor {
    geometry::FaceId face;
    std::array<geometry::VertexId, 3> vertices{};
    std::array<double, 3> barycentric{};

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const SculptSurfaceAnchor&) const noexcept = default;
};

struct SculptStrokeSample {
    core::Vec3d position;
    double pressure = 1.0;
    std::optional<SculptSurfaceAnchor> surface_anchor = std::nullopt;
    // Captured at authoring time so evaluation never depends on a live view.
    // Tangent mode projects this axis into the local surface plane; a zero
    // projection contributes no displacement at that vertex.
    std::optional<core::Vec3d> direction_axis = std::nullopt;

    [[nodiscard]] bool operator==(const SculptStrokeSample& other) const noexcept {
        return position.x == other.position.x && position.y == other.position.y &&
            position.z == other.position.z && pressure == other.pressure &&
            surface_anchor == other.surface_anchor &&
            direction_axis.has_value() == other.direction_axis.has_value() &&
            (!direction_axis.has_value() ||
                (direction_axis->x == other.direction_axis->x &&
                 direction_axis->y == other.direction_axis->y &&
                 direction_axis->z == other.direction_axis->z));
    }
};

[[nodiscard]] core::Result<core::Vec3d> resolve_sculpt_surface_anchor(
    const geometry::EditableMesh& mesh,
    const SculptSurfaceAnchor& anchor);
[[nodiscard]] core::Result<SculptStrokeSample> make_sculpt_surface_sample(
    const geometry::EditableMesh& mesh,
    SculptSurfaceAnchor anchor,
    double pressure = 1.0);

// Samples and settings are authored source. Preview buffers and evaluated
// vertex positions are derived and can always be rebuilt from this record.
struct SculptStroke {
    std::string identity;
    SculptStrokeMode mode = SculptStrokeMode::add;
    SculptSymmetryMode symmetry = SculptSymmetryMode::none;
    // Applies to normal-displacement kernels; unsupported kernel combinations
    // are rejected instead of silently ignoring the authored setting.
    SculptStrokeDirectionMode direction_mode = SculptStrokeDirectionMode::surface_normal;
    bool enabled = true;
    // The legacy X plane remains for format versions 1 through 3. New plane
    // and radial settings are expressed in the bound mesh's local coordinates.
    double symmetry_plane_x = 0.0;
    core::Vec3d symmetry_origin_local{};
    core::Vec3d symmetry_axis_local{1.0, 0.0, 0.0};
    std::uint32_t radial_symmetry_count = 4U;
    double radius = 0.25;
    double strength = 0.05;
    // Vertex float32/float64 masks use 1 for protected and 0 for unmasked.
    std::string mask_layer_id;
    std::vector<SculptStrokeSample> samples;

    [[nodiscard]] core::Result<void> validate() const;
};

struct SculptLayer {
    std::string identity;
    std::string name;
    bool visible = true;
    double weight = 1.0;
    std::vector<SculptStroke> strokes;
};

struct SculptStack {
    std::string project_namespace;
    std::uint64_t mesh_asset = 0U;
    BakeMeshBinding source;
    std::vector<SculptLayer> layers;
    std::uint32_t format_version = kSculptStackVersion;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> digest() const;
    [[nodiscard]] static core::Result<SculptStack> deserialize(std::string_view text);
};

struct SculptEvaluationArtifact {
    SculptStack stack;
    std::string algorithm{"carto.sculpt.cpu.brush-kernels.v5"};
    std::string evidence_class{"carto.cpu-reference"};
    assets::Sha256Digest settings_digest;
    assets::Sha256Digest output_digest;
    assets::Sha256Digest artifact_digest;
    std::uint64_t byte_size = 0U;
    double maximum_displacement = 0.0;
    std::vector<geometry::Vertex> vertices;

    [[nodiscard]] core::Result<void> refresh_digests();
    [[nodiscard]] core::Result<void> validate(
        const geometry::EditableMesh& base_mesh) const;
};

[[nodiscard]] core::Result<SculptStack> make_sculpt_stack(
    const project::ProjectDocument& document,
    std::uint64_t mesh_asset);

[[nodiscard]] core::Result<SculptEvaluationArtifact> compile_sculpt_stack(
    const SculptStack& stack,
    const geometry::EditableMesh& base_mesh);

// A missing expected digest creates a new stack. Updating an existing stack
// requires its current digest, preventing silent replacement of authored work.
[[nodiscard]] core::Result<void> stage_sculpt_stack(
    project::ProjectTransaction& transaction,
    const SculptStack& stack,
    std::optional<assets::Sha256Digest> expected_current_digest = std::nullopt);

[[nodiscard]] std::string sculpt_stack_transaction_operation(
    const assets::Sha256Digest& stack_digest);

} // namespace carto::production
