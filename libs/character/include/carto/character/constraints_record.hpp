#pragma once

#include <carto/character/ids.hpp>
#include <carto/character/time.hpp>

#include <carto/core/result.hpp>
#include <carto/kinematic/graph.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace carto::character {

enum class ConstraintType : std::uint8_t {
    ik_pin,
    fk_blend,
    contact,
    orient,
    limit,
};

enum class ConstraintStage : std::uint8_t {
    preprocess,
    pre_deformation,
    post_deformation,
    post_simulation,
};

// Declared rule for resolving a conflict between two constraint types that
// target the same referent. `none` means undeclared: validation fails closed
// when a conflicting pair lacks an explicit rule.
enum class ArbitrationRule : std::uint8_t {
    none,
    priority_wins,
    blend,
    sequential,
};

[[nodiscard]] std::string_view constraint_type_name(ConstraintType type) noexcept;
[[nodiscard]] std::string_view constraint_stage_name(ConstraintStage stage) noexcept;
[[nodiscard]] std::string_view arbitration_rule_name(ArbitrationRule rule) noexcept;

// A referent a constraint acts on: one joint or one control, never both.
struct ConstraintTargetRef {
    kinematic::JointId joint;
    ControlId control;

    [[nodiscard]] bool is_joint() const noexcept { return joint.value != 0U; }
    [[nodiscard]] bool is_control() const noexcept { return control.value != 0U; }
    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator==(const ConstraintTargetRef&) const noexcept = default;
    [[nodiscard]] bool operator<(const ConstraintTargetRef&) const noexcept;
};

struct ConstraintDefinition {
    ConstraintId id;
    ConstraintType type = ConstraintType::ik_pin;
    ConstraintTargetRef source;
    ConstraintTargetRef target;
    std::uint32_t priority = 0U;
    // Solver tolerance in scene units; must be non-negative and finite.
    double tolerance = 0.0;
    // Validity window on one rational timebase; end must not precede start.
    RationalTime validity_start;
    RationalTime validity_end;
    ConstraintStage stage = ConstraintStage::pre_deformation;
    // Optional dependency on another constraint in the same graph; the
    // depends_on relation must be acyclic.
    std::optional<ConstraintId> depends_on;

    [[nodiscard]] core::Result<void> validate() const;
};

// Declared arbitration for an unordered constraint-type pair that may target
// the same referent.
struct ConstraintArbitration {
    ConstraintType first;
    ConstraintType second;
    ArbitrationRule rule = ArbitrationRule::none;

    [[nodiscard]] core::Result<void> validate() const;
};

// The authored constraint graph. Validation guarantees: distinct IDs,
// acyclic depends_on DAG, and declared conflict arbitration — every pair of
// constraints that share a target referent with different types must have a
// non-`none` arbitration rule for that unordered type pair.
class ConstraintGraph final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxConstraints = 4'096U;
    static constexpr std::size_t kMaxArbitrations = 64U;

    [[nodiscard]] core::Result<void> insert(ConstraintDefinition constraint);
    [[nodiscard]] core::Result<void> declare_arbitration(ConstraintArbitration arbitration);

    [[nodiscard]] const std::map<ConstraintId, ConstraintDefinition>& constraints()
        const noexcept {
        return constraints_;
    }
    [[nodiscard]] const std::vector<ConstraintArbitration>& arbitrations() const noexcept {
        return arbitrations_;
    }

    [[nodiscard]] bool empty() const noexcept { return constraints_.empty(); }
    [[nodiscard]] core::Result<void> validate() const;

private:
    std::map<ConstraintId, ConstraintDefinition> constraints_;
    std::vector<ConstraintArbitration> arbitrations_;
};

} // namespace carto::character
