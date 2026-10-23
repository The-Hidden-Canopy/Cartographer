#pragma once

#include <carto/character/controls.hpp>
#include <carto/character/ids.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/core/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::face {

// Absolute ceiling for one morph blend term weight and for any observed
// blended morph weight. Normalized expressions stay inside [0, 1]; the ceiling
// exists so additive expressions and hostile input cannot drive unbounded
// deformation.
inline constexpr double kMaxMorphTermWeight = 8.0;
inline constexpr double kMaxObservedMorphWeight = 64.0;
inline constexpr double kMaxControlOffsetMagnitude = 1.0e6;

// Sum policy of one expression, documented contract:
// - normalized: the expression's morph term weights are rescaled so they sum
//   to exactly one (a convex combination) and the result is scaled by the
//   expression's blend weight. Renormalization does not commute with additive
//   blending, which is what makes order visible in the conflict receipt.
// - additive: each morph term contributes its authored weight times the blend
//   weight with no rescale. Pure additive blends commute.
enum class ExpressionSumPolicy : std::uint8_t {
    normalized,
    additive,
};

[[nodiscard]] std::string_view expression_sum_policy_name(ExpressionSumPolicy policy) noexcept;
[[nodiscard]] core::Result<ExpressionSumPolicy> expression_sum_policy_from_name(
    std::string_view text);

// One morph target term of an expression. Weight must be finite and within
// (0, kMaxMorphTermWeight].
struct MorphBlendTerm {
    character::MorphTargetId target;
    double weight = 0.0;

    // Identity is the target id: terms are unique per target, so ordering and
    // equality compare only the identity, never the weight.
    [[nodiscard]] bool operator==(const MorphBlendTerm& other) const noexcept {
        return target == other.target;
    }
    [[nodiscard]] auto operator<=>(const MorphBlendTerm& other) const noexcept {
        return target <=> other.target;
    }
};

// One control offset term of an expression. `axes_mask` must be nonempty and a
// subset of the control's allowed axes (enforced by
// validate_expression_controls); offsets on masked axes must be finite and
// within kMaxControlOffsetMagnitude and the control's per-axis limits.
struct ControlOffsetTerm {
    character::ControlId control;
    std::uint8_t axes_mask = 0U;
    std::array<double, character::kAxisCount> offsets{};

    // Identity is the control id: terms are unique per control, so ordering
    // and equality compare only the identity, never the axes or offsets.
    [[nodiscard]] bool operator==(const ControlOffsetTerm& other) const noexcept {
        return control == other.control;
    }
    [[nodiscard]] auto operator<=>(const ControlOffsetTerm& other) const noexcept {
        return control <=> other.control;
    }
};

// A named, bounded expression: a normalized (or declared additive) blend of
// morph targets plus control offsets. Records are authoring truth: digest-bound
// at canonical_digest(), versioned, and free of runtime state.
struct Expression {
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxNameBytes = 128U;
    static constexpr std::size_t kMaxMorphTerms = 256U;
    static constexpr std::size_t kMaxControlTerms = 128U;
    static constexpr std::size_t kMaxSerializedBytes = 256U * 1024U;

    std::uint32_t schema_version = kSchemaVersion;
    std::string name;
    ExpressionSumPolicy sum_policy = ExpressionSumPolicy::normalized;
    // Sorted by target id, unique.
    std::vector<MorphBlendTerm> morph_terms;
    // Sorted by control id, unique.
    std::vector<ControlOffsetTerm> control_terms;

    [[nodiscard]] core::Result<void> insert_morph_term(MorphBlendTerm term);
    [[nodiscard]] core::Result<void> insert_control_term(ControlOffsetTerm term);
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
    [[nodiscard]] static core::Result<Expression> deserialize(std::string_view text);
};

// Admission of one expression's control offsets against the control graph:
// every driven control must exist, every driven axis must be allowed by that
// control, and every offset on a driven axis must lie within the control's
// per-axis limits. Refusal is explicit; offsets are never silently clamped at
// admission time.
[[nodiscard]] core::Result<void> validate_expression_controls(
    const Expression& expression,
    const character::ControlGraph& controls);

// One weighted expression in a blend request.
struct ExpressionBlendTerm {
    const Expression* expression = nullptr;
    double weight = 0.0; // finite, within [0, 1]
};

// Result of blending weighted expressions. Morph weights are summed across
// expressions per target; control offsets are summed per axis. Maps iterate in
// stable id order so every canonical output derived from them is
// deterministic.
struct BlendedExpression {
    std::map<character::MorphTargetId, double> morph_weights;
    std::map<character::ControlId, std::array<double, character::kAxisCount>> control_offsets;
    // Sum of all blended morph weights; equals the sum of expression blend
    // weights for normalized-only blends, and is unbounded for additive terms.
    double total_morph_weight = 0.0;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Blends weighted expressions under the documented sum policy. Refuses
// non-finite blend weights, blend weights outside [0, 1], null expressions and
// outputs exceeding kMaxObservedMorphWeight. Inputs are never clamped.
[[nodiscard]] core::Result<BlendedExpression> blend_expressions(
    std::span<const ExpressionBlendTerm> terms);

// One conflict: two expressions drive the same morph target with nonzero
// weight while at least one of them applies normalized rescaling, so the
// result depends on application order. Pure additive pairs commute and are not
// conflicts.
struct ExpressionConflict {
    character::MorphTargetId target;
    std::string first_expression;
    std::string second_expression;

    [[nodiscard]] bool operator==(const ExpressionConflict&) const noexcept = default;
    [[nodiscard]] auto operator<=>(const ExpressionConflict&) const noexcept = default;
};

// Receipt for conflict detection over an expression set. Sorted by
// (target, first, second); an empty table is a positive result and is itself
// digest-stable.
struct ExpressionConflictReceipt {
    static constexpr std::size_t kMaxConflicts = 4'096U;
    static constexpr std::size_t kMaxSerializedBytes = 256U * 1024U;

    std::vector<ExpressionConflict> conflicts; // sorted

    [[nodiscard]] bool empty() const noexcept { return conflicts.empty(); }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] std::string serialize() const;
    [[nodiscard]] core::Result<assets::Sha256Digest> canonical_digest() const;
};

// Detects order-dependent morph target conflicts across an expression set.
// Every pair of distinct expressions that both drive one morph target with a
// nonzero weight, where at least one applies normalized rescaling, produces
// one sorted receipt entry. Expressions must be sorted-unique by name.
[[nodiscard]] core::Result<ExpressionConflictReceipt> detect_expression_conflicts(
    std::span<const Expression> expressions);

} // namespace carto::face
