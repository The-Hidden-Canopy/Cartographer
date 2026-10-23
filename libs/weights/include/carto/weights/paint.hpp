#pragma once

#include <carto/assets/blob_store.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/kinematic/skin.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace carto::weights {

// Headless, deterministic weight painting (Chapter 08 "Painting system").
// Strokes are revision-bound pure operations over canonical source-mesh vertex
// indices: they never depend on a rendered frame's vertex ordering, they are
// undo-friendly because each operation returns a new binding plus a receipt
// while the caller keeps the previous binding, and every affected vertex is
// re-admitted through character::validate_influences.

enum class BrushFalloff : std::uint8_t {
    // Strength scales linearly with distance from the brush center.
    spherical,
    // Flat strength for every vertex inside the radius.
    vertex,
};

enum class PaintOp : std::uint8_t {
    // Increase the owning joint's weight by strength * falloff * mask.
    add,
    // Set the owning joint's weight to strength * falloff * mask.
    set,
    // Move each weight toward the topological neighborhood average.
    smooth,
};

[[nodiscard]] std::string_view brush_falloff_name(BrushFalloff falloff) noexcept;
[[nodiscard]] std::string_view paint_op_name(PaintOp op) noexcept;

struct BrushStamp {
    core::Vec3d center;
    // Strictly positive, finite.
    double radius = 0.0;
    // Finite, inside [0, 1].
    double strength = 0.0;
    BrushFalloff falloff = BrushFalloff::spherical;
    PaintOp op = PaintOp::add;

    [[nodiscard]] core::Result<void> validate() const;
};

// Per-vertex paint mask (vertex index -> factor inside [0, 1]). Absent
// vertices have factor 1; a factor of 0 excludes the vertex. Content-
// addressable so a receipt can bind the exact mask used.
class WeightMask final {
public:
    static constexpr std::size_t kMaxEntries = 262'144U;

    [[nodiscard]] core::Result<void> set(std::uint32_t vertex, double factor);
    // Factor for a vertex; 1.0 when unmasked.
    [[nodiscard]] double factor(std::uint32_t vertex) const noexcept;
    [[nodiscard]] const std::map<std::uint32_t, double>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<std::uint32_t, double> entries_;
};

// Locked (vertex, joint) pairs whose weights must not change. Locking is how
// an artist protects finished regions; locked-weight sums above one refuse
// the stroke instead of silently renormalizing.
class WeightLocks final {
public:
    static constexpr std::size_t kMaxEntries = 1'048'576U;

    [[nodiscard]] core::Result<void> lock(std::uint32_t vertex, kinematic::JointId joint);
    [[nodiscard]] bool locked(std::uint32_t vertex, kinematic::JointId joint) const;
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<std::uint32_t, std::vector<kinematic::JointId>> locked_;
};

// One deterministic stroke: an ordered list of stamps the caller sampled.
// No sampling happens inside the library, so receipts never depend on input
// hardware or wall time.
struct PaintStroke {
    static constexpr std::size_t kMaxStamps = 4'096U;

    std::vector<BrushStamp> stamps;

    [[nodiscard]] core::Result<void> validate() const;
};

struct PaintReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U;

    std::uint64_t stamp_index = 0U;
    PaintOp op = PaintOp::add;
    BrushFalloff falloff = BrushFalloff::spherical;
    core::Vec3d center;
    double radius = 0.0;
    double strength = 0.0;
    kinematic::JointId joint;
    // Zero digests mean "no mask"/"no locks" for that stroke.
    assets::Sha256Digest mask_digest;
    assets::Sha256Digest locks_digest;
    assets::Sha256Digest input_binding_digest;
    assets::Sha256Digest output_binding_digest;
    std::uint64_t affected_vertex_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<PaintReceipt> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct PaintOutput {
    kinematic::SkinBinding binding;
    PaintReceipt receipt;
};

struct PaintStrokeOutput {
    kinematic::SkinBinding binding;
    // One receipt per stamp, in stroke order.
    std::vector<PaintReceipt> receipts;
};

// Applies one stamp to `binding` against `mesh`. `mask` and `locks` may be
// null. Fails closed without modifying anything when: locked weights would
// exceed a sum of one, an edited vertex would be left with no valid positive
// influence, or any resulting vertex fails character::validate_influences.
[[nodiscard]] core::Result<PaintOutput> apply_brush_stamp(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const BrushStamp& stamp,
    kinematic::JointId joint,
    const WeightMask* mask,
    const WeightLocks* locks);

[[nodiscard]] core::Result<PaintStrokeOutput> apply_paint_stroke(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const PaintStroke& stroke,
    kinematic::JointId joint,
    const WeightMask* mask,
    const WeightLocks* locks);

// Whole-region weight commands (Chapter 08 "flood, prune, mirror"). Like the
// brush they are pure: each returns a new binding plus a digest-bound receipt
// while the caller keeps the previous binding for undo, every edited vertex is
// renormalized through the same lock-aware rule as strokes, and any refusal
// leaves the input untouched.

enum class WeightEditKind : std::uint8_t {
    flood,
    prune,
    mirror,
};

enum class MirrorAxis : std::uint8_t { x, y, z };

enum class MirrorDirection : std::uint8_t {
    // Copy weights from the positive half-space onto the negative one.
    positive_to_negative,
    negative_to_positive,
};

[[nodiscard]] std::string_view weight_edit_kind_name(WeightEditKind kind) noexcept;
[[nodiscard]] std::string_view mirror_axis_name(MirrorAxis axis) noexcept;
[[nodiscard]] std::string_view mirror_direction_name(MirrorDirection direction) noexcept;

struct FloodRequest {
    kinematic::JointId joint;
    // add or set only; smoothing is a brush operation.
    PaintOp op = PaintOp::set;
    // Finite, inside [0, 1].
    double value = 0.0;
    // Canonical mesh vertex indices, unique. Each must already carry an
    // influence; flooding never invents coverage for rigid vertices.
    std::vector<std::uint32_t> vertices;

    [[nodiscard]] core::Result<void> validate() const;
};

// Joint pairing for mirrored weights. Pairs are symmetric (left<->right);
// joints absent from the map are centre joints and mirror onto themselves.
class JointMirrorMap final {
public:
    static constexpr std::size_t kMaxPairs = 4'096U;

    [[nodiscard]] core::Result<void> pair(kinematic::JointId first, kinematic::JointId second);
    [[nodiscard]] kinematic::JointId mirrored(kinematic::JointId joint) const noexcept;
    [[nodiscard]] const std::map<kinematic::JointId, kinematic::JointId>& entries() const noexcept {
        return mirror_;
    }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;

private:
    std::map<kinematic::JointId, kinematic::JointId> mirror_;
};

struct MirrorRequest {
    MirrorAxis axis = MirrorAxis::x;
    MirrorDirection direction = MirrorDirection::positive_to_negative;
    // Positional matching tolerance for the reflected vertex and the plane
    // band, strictly positive and finite.
    double tolerance = 1e-5;

    [[nodiscard]] core::Result<void> validate() const;
};

struct WeightEditReceipt {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U;

    WeightEditKind kind = WeightEditKind::flood;
    // flood: target joint, op, value. prune: value is the threshold.
    kinematic::JointId joint;
    PaintOp op = PaintOp::set;
    double value = 0.0;
    // mirror only.
    MirrorAxis axis = MirrorAxis::x;
    MirrorDirection direction = MirrorDirection::positive_to_negative;
    double tolerance = 0.0;
    assets::Sha256Digest joint_map_digest;
    // Zero digest means "no locks".
    assets::Sha256Digest locks_digest;
    assets::Sha256Digest input_binding_digest;
    assets::Sha256Digest output_binding_digest;
    std::uint64_t affected_vertex_count = 0U;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] static core::Result<WeightEditReceipt> deserialize(std::string_view text);
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

struct WeightEditOutput {
    kinematic::SkinBinding binding;
    WeightEditReceipt receipt;
};

// Adds to or sets one joint's weight on every listed vertex, then
// renormalizes. Fails closed on rigid or out-of-range vertices, an unknown
// joint, or lock conflicts.
[[nodiscard]] core::Result<WeightEditOutput> flood_weights(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const FloodRequest& request,
    const WeightLocks* locks);

// Removes unlocked influences whose weight is strictly below threshold
// (finite, inside (0, 1)) and renormalizes. A vertex whose every influence is
// below threshold keeps its largest one so coverage is never lost.
[[nodiscard]] core::Result<WeightEditOutput> prune_weights(
    const kinematic::SkinBinding& binding,
    double threshold,
    const WeightLocks* locks);

// Copies weights across a symmetry plane through the origin. Each
// destination vertex takes the remapped influences of the source vertex
// nearest its reflection (lowest index on ties); vertices inside the plane
// band average their own weights with their remapped copy. Refuses, without
// mutation, when a destination has no source within tolerance, a matched
// source is rigid, a destination carries a locked weight, or a mapped joint
// is absent from the binding's graph-bound joint set.
[[nodiscard]] core::Result<WeightEditOutput> mirror_weights(
    const kinematic::SkinBinding& binding,
    const geometry::CompiledMesh& mesh,
    const JointMirrorMap& joints,
    const MirrorRequest& request,
    const WeightLocks* locks);

} // namespace carto::weights
