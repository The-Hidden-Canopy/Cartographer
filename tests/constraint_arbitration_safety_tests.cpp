#include <carto/character/constraints_record.hpp>
#include <carto/core/diagnostic.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/pose/constraints.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>

using namespace carto;
namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(EXIT_FAILURE); }
}
character::RationalTime time(std::int64_t tick) { return {tick, 24U}; }
kinematic::KinematicGraph chain() {
    kinematic::KinematicGraph graph;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        kinematic::KinematicNode node;
        node.id = {i};
        if (i > 1) { node.parent = kinematic::JointId{i - 1}; node.rest.translation.x = 1; }
        node.kind = kinematic::JointKind::ball;
        require(static_cast<bool>(graph.insert_node(node)), "graph fixture");
    }
    return graph;
}
character::ConstraintDefinition make(std::uint64_t id, character::ConstraintType type,
                                     std::uint64_t source, std::uint64_t target,
                                     std::uint32_t priority = 0,
                                     character::ConstraintStage stage = character::ConstraintStage::pre_deformation) {
    character::ConstraintDefinition value;
    value.id = {id}; value.type = type; value.source.joint = {source}; value.target.joint = {target};
    value.priority = priority; value.tolerance = 1e-6; value.validity_start = time(0);
    value.validity_end = time(100); value.stage = stage;
    return value;
}
void insert(character::ConstraintGraph& graph, character::ConstraintDefinition value) {
    require(static_cast<bool>(graph.insert(std::move(value))), "insert constraint");
}
void rule(character::ConstraintGraph& graph, character::ConstraintType first,
          character::ConstraintType second, character::ArbitrationRule value) {
    require(static_cast<bool>(graph.declare_arbitration({first, second, value})), "declare arbitration");
}
void goal(pose::ConstraintEvalContext& context, std::uint64_t id, double x, double y = 0) {
    core::Transform value;
    value.translation = {x, y, 0};
    context.blend_targets[{id}] = value;
    context.blend_weights[{id}] = 1.0;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

void tie_order_and_dependencies() {
    const auto graph = chain();
    pose::ConstraintEvalContext context; context.time = time(0);
    goal(context, 1, 3); goal(context, 2, 7);
    character::ConstraintGraph equal_priority;
    insert(equal_priority, make(2, character::ConstraintType::fk_blend, 1, 1, 3));
    insert(equal_priority, make(1, character::ConstraintType::fk_blend, 1, 1, 3));
    auto equal = pose::evaluate_constraints(equal_priority, graph, context);
    require(equal && near(equal.value().solved_locals.at({1}).translation.x, 7),
        "equal priority runs smaller ID first");
    require(context.base_locals.empty() && graph.find({1})->rest.translation.x == 0,
        "evaluation does not mutate source inputs");

    character::ConstraintGraph ordered;
    auto dependent = make(1, character::ConstraintType::fk_blend, 1, 1, 10);
    dependent.depends_on = character::ConstraintId{2};
    insert(ordered, dependent);
    insert(ordered, make(2, character::ConstraintType::fk_blend, 1, 1, 1));
    auto result = pose::evaluate_constraints(ordered, graph, context);
    require(result && near(result.value().solved_locals.at({1}).translation.x, 3),
        "same-target sequence preserves dependency before priority");

    character::ConstraintGraph interleaved;
    insert(interleaved, make(1, character::ConstraintType::fk_blend, 1, 1, 10));
    insert(interleaved, make(2, character::ConstraintType::fk_blend, 1, 2, 5));
    auto later = make(3, character::ConstraintType::fk_blend, 1, 1, 1);
    later.depends_on = character::ConstraintId{2};
    insert(interleaved, later);
    goal(context, 3, 9);
    const auto refused = pose::evaluate_constraints(interleaved, graph, context);
    require(!refused && refused.error().message.find("interleaved dependency") != std::string::npos,
        "cross-target dependency cannot be silently moved ahead");
    require(context.base_locals.empty() && graph.find({1})->rest.translation.x == 0,
        "interleaving refusal preserves source inputs");

    character::ConstraintGraph independent;
    insert(independent, make(1, character::ConstraintType::fk_blend, 1, 1, 10));
    insert(independent, make(2, character::ConstraintType::fk_blend, 1, 2, 5));
    auto separate = pose::evaluate_constraints(independent, graph, context);
    require(separate && near(separate.value().solved_locals.at({1}).translation.x, 3) &&
        near(separate.value().solved_locals.at({2}).translation.x, 7),
        "independent targets still evaluate");

    character::ConstraintGraph inactive;
    auto prerequisite = make(1, character::ConstraintType::fk_blend, 1, 2);
    prerequisite.validity_end = time(1);
    insert(inactive, prerequisite);
    auto needs_inactive = make(2, character::ConstraintType::fk_blend, 1, 1);
    needs_inactive.depends_on = character::ConstraintId{1};
    insert(inactive, needs_inactive);
    context.time = time(10);
    require(!pose::evaluate_constraints(inactive, graph, context), "inactive prerequisite refuses");
    context.time = time(0);

    character::ConstraintGraph later_stage;
    insert(later_stage, make(1, character::ConstraintType::fk_blend, 1, 2, 1,
                             character::ConstraintStage::post_deformation));
    auto premature = make(2, character::ConstraintType::fk_blend, 1, 1, 1);
    premature.depends_on = character::ConstraintId{1};
    insert(later_stage, premature);
    require(!pose::evaluate_constraints(later_stage, graph, context), "later-stage prerequisite refuses");

    character::ConstraintGraph losing_dependency;
    insert(losing_dependency, make(1, character::ConstraintType::fk_blend, 1, 1, 1));
    insert(losing_dependency, make(2, character::ConstraintType::orient, 1, 1, 10));
    auto needs_loser = make(3, character::ConstraintType::fk_blend, 1, 2, 1);
    needs_loser.depends_on = character::ConstraintId{1};
    insert(losing_dependency, needs_loser);
    rule(losing_dependency, character::ConstraintType::fk_blend, character::ConstraintType::orient,
         character::ArbitrationRule::priority_wins);
    require(!pose::evaluate_constraints(losing_dependency, graph, context),
        "a superseded prerequisite cannot satisfy a dependent constraint");
}

void mixed_rules_and_blends_fail_closed() {
    const auto graph = chain();
    pose::ConstraintEvalContext context; context.time = time(0); goal(context, 1, 2);
    character::ConstraintGraph mixed;
    insert(mixed, make(1, character::ConstraintType::fk_blend, 1, 1, 3));
    insert(mixed, make(2, character::ConstraintType::orient, 1, 1, 2));
    insert(mixed, make(3, character::ConstraintType::limit, 1, 1, 1));
    rule(mixed, character::ConstraintType::fk_blend, character::ConstraintType::orient,
         character::ArbitrationRule::priority_wins);
    rule(mixed, character::ConstraintType::fk_blend, character::ConstraintType::limit,
         character::ArbitrationRule::sequential);
    rule(mixed, character::ConstraintType::orient, character::ConstraintType::limit,
         character::ArbitrationRule::priority_wins);
    const auto refused = pose::evaluate_constraints(mixed, graph, context);
    require(!refused && refused.error().message.find("conflicting arbitration rules") != std::string::npos,
        "incompatible pair rules are not silently discarded");

    character::ConstraintGraph blend;
    insert(blend, make(1, character::ConstraintType::fk_blend, 1, 1, 1));
    insert(blend, make(2, character::ConstraintType::orient, 1, 1, 2));
    rule(blend, character::ConstraintType::fk_blend, character::ConstraintType::orient,
         character::ArbitrationRule::blend);
    const auto unsupported = pose::evaluate_constraints(blend, graph, context);
    require(!unsupported && unsupported.error().code == core::ErrorCode::unsupported,
        "blend refuses before claiming unmeasured final residuals");
    require(context.base_locals.empty() && graph.find({1})->rest.translation.x == 0,
        "blend refusal preserves inputs");

    character::ConstraintGraph priority;
    insert(priority, make(1, character::ConstraintType::fk_blend, 1, 1, 1));
    insert(priority, make(2, character::ConstraintType::orient, 1, 1, 2));
    rule(priority, character::ConstraintType::fk_blend, character::ConstraintType::orient,
         character::ArbitrationRule::priority_wins);
    const auto selected = pose::evaluate_constraints(priority, graph, context);
    require(selected && selected.value().outcomes.at({1}).superseded &&
        !selected.value().outcomes.at({2}).superseded, "uniform priority rule selects documented winner");

    character::ConstraintGraph equal_priority;
    insert(equal_priority, make(2, character::ConstraintType::orient, 1, 1, 4));
    insert(equal_priority, make(1, character::ConstraintType::fk_blend, 1, 1, 4));
    rule(equal_priority, character::ConstraintType::fk_blend, character::ConstraintType::orient,
         character::ArbitrationRule::priority_wins);
    const auto tie = pose::evaluate_constraints(equal_priority, graph, context);
    require(tie && !tie.value().outcomes.at({1}).superseded && tie.value().outcomes.at({2}).superseded,
        "equal priority winner uses ascending constraint ID");

    character::ConstraintGraph sequence;
    auto follower = make(2, character::ConstraintType::orient, 1, 1, 10);
    follower.depends_on = character::ConstraintId{1};
    insert(sequence, follower);
    insert(sequence, make(1, character::ConstraintType::fk_blend, 1, 1, 1));
    rule(sequence, character::ConstraintType::fk_blend, character::ConstraintType::orient,
         character::ArbitrationRule::sequential);
    const auto sequential = pose::evaluate_constraints(sequence, graph, context);
    require(sequential && sequential.value().outcomes.size() == 2 &&
        !sequential.value().outcomes.at({1}).superseded &&
        !sequential.value().outcomes.at({2}).superseded &&
        near(sequential.value().solved_locals.at({1}).translation.x, 2),
        "uniform sequential rule preserves the dependency order");
}

void references_and_final_contact_evidence() {
    const auto graph = chain();
    pose::ConstraintEvalContext context; context.time = time(0);
    character::ConstraintGraph missing;
    insert(missing, make(1, character::ConstraintType::orient, 1, 99));
    auto absent = pose::evaluate_constraints(missing, graph, context);
    require(!absent && absent.error().code == core::ErrorCode::not_found,
        "missing joint is refused before dereference");
    character::ConstraintGraph control;
    auto unsupported = make(1, character::ConstraintType::orient, 1, 1);
    unsupported.target.joint = {}; unsupported.target.control = character::ControlId{1};
    insert(control, unsupported);
    auto control_result = pose::evaluate_constraints(control, graph, context);
    require(!control_result && control_result.error().code == core::ErrorCode::unsupported,
        "control referent is explicitly unsupported");
    character::ConstraintGraph bad_goal;
    insert(bad_goal, make(1, character::ConstraintType::fk_blend, 1, 1));
    goal(context, 1, 0);
    context.blend_targets.at({1}).translation.x = std::numeric_limits<double>::quiet_NaN();
    require(!pose::evaluate_constraints(bad_goal, graph, context),
        "non-finite authored blend goal refuses");
    context.blend_targets.clear(); context.blend_weights.clear();
    pose::ConstraintEvalContext overflow; overflow.time = time(0);
    core::Transform huge;
    huge.scale = {1e308, 1e308, 1e308};
    overflow.base_locals[{1}] = huge;
    require(!pose::evaluate_constraints(character::ConstraintGraph{}, graph, overflow),
        "finite local inputs cannot publish an overflowed world pose");

    character::ConstraintGraph superseded;
    insert(superseded, make(1, character::ConstraintType::contact, 1, 3, 1));
    insert(superseded, make(2, character::ConstraintType::orient, 1, 3, 2));
    rule(superseded, character::ConstraintType::contact, character::ConstraintType::orient,
         character::ArbitrationRule::priority_wins);
    context.pin_targets[{1}] = {2, 0, 0};
    const auto hidden = pose::evaluate_constraints(superseded, graph, context);
    require(hidden && hidden.value().outcomes.at({1}).superseded && !hidden.value().all_converged,
        "superseded contact stays unverified without inferring hardness");

    character::ConstraintGraph moved;
    insert(moved, make(1, character::ConstraintType::contact, 1, 3, 1));
    insert(moved, make(2, character::ConstraintType::fk_blend, 1, 1, 1,
                       character::ConstraintStage::post_deformation));
    goal(context, 2, 0, 1);
    const auto moved_receipt = pose::evaluate_constraints(moved, graph, context);
    require(moved_receipt && near(moved_receipt.value().outcomes.at({1}).residual, 1) &&
        !moved_receipt.value().outcomes.at({1}).converged && !moved_receipt.value().all_converged,
        "later motion updates final contact residual and leaves proposal unconverged");
    character::ConstraintGraph stationary;
    insert(stationary, make(1, character::ConstraintType::contact, 1, 3));
    const auto held = pose::evaluate_constraints(stationary, graph, context);
    require(held && held.value().outcomes.at({1}).converged && held.value().all_converged &&
        near(held.value().outcomes.at({1}).residual, 0), "unmoved contact remains measured as satisfied");

    character::ConstraintGraph moved_pin;
    insert(moved_pin, make(1, character::ConstraintType::ik_pin, 1, 3, 1));
    insert(moved_pin, make(2, character::ConstraintType::fk_blend, 1, 1, 1,
                           character::ConstraintStage::post_deformation));
    const auto pin_receipt = pose::evaluate_constraints(moved_pin, graph, context);
    require(pin_receipt && near(pin_receipt.value().outcomes.at({1}).residual, 1) &&
        !pin_receipt.value().outcomes.at({1}).converged && !pin_receipt.value().all_converged,
        "later motion updates final IK pin residual");

    character::ConstraintGraph moved_orient;
    insert(moved_orient, make(1, character::ConstraintType::orient, 1, 3, 1));
    insert(moved_orient, make(2, character::ConstraintType::fk_blend, 1, 2, 1,
                              character::ConstraintStage::post_deformation));
    core::Transform rotated;
    rotated.translation = {1, 0, 0};
    const double half = std::sqrt(0.5);
    rotated.rotation = {0, 0, half, half};
    context.blend_targets[{2}] = rotated; context.blend_weights[{2}] = 1;
    const auto orient_receipt = pose::evaluate_constraints(moved_orient, graph, context);
    require(orient_receipt && orient_receipt.value().outcomes.at({1}).residual > 1.5 &&
        !orient_receipt.value().outcomes.at({1}).converged && !orient_receipt.value().all_converged,
        "later motion updates final orientation residual");

    character::ConstraintGraph orient_only;
    insert(orient_only, make(1, character::ConstraintType::orient, 1, 3));
    pose::ConstraintEvalContext rotated_input; rotated_input.time = time(0);
    core::Transform rotated_tip = rotated;
    rotated_input.base_locals[{3}] = rotated_tip;
    const auto corrected = pose::evaluate_constraints(orient_only, graph, rotated_input);
    require(corrected && corrected.value().outcomes.at({1}).converged &&
        near(corrected.value().outcomes.at({1}).residual, 0) && corrected.value().all_converged,
        "orientation is measured after a successful correction");
    rotated_tip.rotation = {0, 0, 0, -1};
    rotated_input.base_locals[{3}] = rotated_tip;
    const auto hemisphere = pose::evaluate_constraints(orient_only, graph, rotated_input);
    require(hemisphere && hemisphere.value().outcomes.at({1}).converged &&
        near(hemisphere.value().outcomes.at({1}).residual, 0),
        "equivalent opposite-sign quaternions have zero orientation residual");
    character::ConstraintGraph shortest_rotation;
    insert(shortest_rotation, make(1, character::ConstraintType::orient, 1, 3));
    insert(shortest_rotation, make(2, character::ConstraintType::fk_blend, 1, 2, 1,
                                   character::ConstraintStage::post_deformation));
    insert(shortest_rotation, make(3, character::ConstraintType::fk_blend, 1, 3, 1,
                                   character::ConstraintStage::post_deformation));
    pose::ConstraintEvalContext composed; composed.time = time(0);
    core::Transform turn;
    turn.translation = {1, 0, 0};
    turn.rotation = {0, 0, std::sqrt(3.0) / 2.0, 0.5};
    composed.blend_targets[{2}] = turn; composed.blend_weights[{2}] = 1;
    composed.blend_targets[{3}] = turn; composed.blend_weights[{3}] = 1;
    require(!pose::evaluate_constraints(shortest_rotation, graph, composed),
        "mixed constraints without declared arbitration refuse before reporting a residual");
    rule(shortest_rotation, character::ConstraintType::orient, character::ConstraintType::fk_blend,
         character::ArbitrationRule::sequential);
    const auto shortest = pose::evaluate_constraints(shortest_rotation, graph, composed);
    if (!shortest) std::cerr << "DIAGNOSTIC shortest: " << shortest.error().message << "\n";
    else std::cerr << "DIAGNOSTIC shortest residual=" << shortest.value().outcomes.at({1}).residual << "\n";
    require(shortest && near(shortest.value().outcomes.at({1}).residual, 2 * std::acos(-1.0) / 3) &&
        !shortest.value().outcomes.at({1}).converged,
        "final 240-degree composition reports its shortest 120-degree residual");
    character::ConstraintGraph descendant_source;
    insert(descendant_source, make(1, character::ConstraintType::orient, 3, 1));
    rotated_tip.rotation = {0, 0, half, half};
    rotated_input.base_locals[{3}] = rotated_tip;
    const auto coupled = pose::evaluate_constraints(descendant_source, graph, rotated_input);
    require(coupled && coupled.value().outcomes.at({1}).residual > 1.5 &&
        !coupled.value().outcomes.at({1}).converged && !coupled.value().all_converged,
        "moving an ancestor cannot claim alignment to its moving descendant");

    character::ConstraintGraph no_promotion;
    insert(no_promotion, make(1, character::ConstraintType::contact, 1, 3, 1));
    insert(no_promotion, make(2, character::ConstraintType::fk_blend, 1, 1, 1,
                              character::ConstraintStage::post_deformation));
    context.pin_targets[{1}] = {3, 0, 0};
    goal(context, 2, 1);
    const auto unpromoted = pose::evaluate_constraints(no_promotion, graph, context);
    require(unpromoted && near(unpromoted.value().outcomes.at({1}).residual, 0) &&
        !unpromoted.value().outcomes.at({1}).converged && !unpromoted.value().all_converged,
        "a later satisfying pose does not erase an earlier solver failure");

    character::ConstraintGraph budgeted_contact;
    auto loose = make(1, character::ConstraintType::contact, 1, 3);
    loose.tolerance = 10;
    insert(budgeted_contact, loose);
    pose::ConstraintEvalContext budgeted; budgeted.time = time(0);
    budgeted.pin_targets[{1}] = {1, 1, 0};
    budgeted.budget.max_iterations = 1;
    budgeted.budget.convergence_tolerance = 1e-15;
    const auto stopped = pose::evaluate_constraints(budgeted_contact, graph, budgeted);
    require(stopped && stopped.value().outcomes.at({1}).residual < loose.tolerance &&
        !stopped.value().outcomes.at({1}).converged && !stopped.value().all_converged,
        "loose contact tolerance cannot promote a stopped solver");
}

void sparse_zero_iteration_jacobian() {
    const auto graph = chain();
    pose::LocalPose base;
    core::Transform ancestor;
    ancestor.translation = {4, 2, 0};
    base[{1}] = ancestor;
    pose::JacobianIkRequest request;
    request.chain = {{2}, {3}};
    request.target = {6, 2, 0};
    const auto solved = pose::solve_chain_jacobian(graph, base, nullptr, request);
    require(solved && solved.value().converged && solved.value().iterations == 0 &&
        solved.value().solved_locals.size() == 2 && base.size() == 1,
        "already-satisfied sparse chain returns rest locals without changing base");
    auto merged = base;
    for (const auto& [joint, local] : solved.value().solved_locals) merged[joint] = local;
    const auto pose = pose::evaluate_fk(graph, merged);
    require(pose && near(pose.value().world.at({3}).translation.x, 6) &&
        near(pose.value().world.at({3}).translation.y, 2) &&
        near(merged.at({1}).translation.x, 4),
        "rest fallback preserves posed ancestors and the complete composed pose");
}
} // namespace

int main() {
    tie_order_and_dependencies();
    mixed_rules_and_blends_fail_closed();
    references_and_final_contact_evidence();
    sparse_zero_iteration_jacobian();
    std::cout << "constraint arbitration safety tests passed\n";
}
