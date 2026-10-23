#include <carto/pose/ik.hpp>

#include <carto/pose/rotation.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

namespace carto::pose {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

constexpr double kGeometryEpsilon = 1e-9;

core::Result<void> validate_budget(const IkBudget& budget) {
    if (budget.max_iterations == 0U || budget.max_iterations > IkBudget::kMaxIterations) {
        return core::Result<void>::failure(invalid(
            "IK iteration budget must be within its bounded range"));
    }
    if (!std::isfinite(budget.convergence_tolerance) || budget.convergence_tolerance < 0.0) {
        return core::Result<void>::failure(invalid(
            "IK convergence tolerance must be non-negative and finite"));
    }
    if (!std::isfinite(budget.damping) || budget.damping <= 0.0) {
        return core::Result<void>::failure(invalid(
            "IK damping must be positive and finite"));
    }
    if (!std::isfinite(budget.max_seconds) || budget.max_seconds < 0.0) {
        return core::Result<void>::failure(invalid(
            "IK time budget must be non-negative and finite"));
    }
    return core::Result<void>::success();
}

// Validates that `chain` is a contiguous ancestor line in the graph, each
// entry the parent of the next, and returns its FK base pose.
core::Result<FkPose> chain_base_pose(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const std::vector<kinematic::JointId>& chain) {
    if (chain.empty()) {
        return core::Result<FkPose>::failure(invalid(
            "IK chain must name at least one joint"));
    }
    for (std::size_t index = 0U; index < chain.size(); ++index) {
        const kinematic::JointId joint = chain[index];
        const kinematic::KinematicNode* node = graph.find(joint);
        if (node == nullptr) {
            return core::Result<FkPose>::failure(invalid(
                "IK chain references a joint outside the graph"));
        }
        if (index > 0U) {
            // Root-first chain: each entry's parent is the previous entry.
            if (!node->parent.has_value() || node->parent.value() != chain[index - 1U]) {
                return core::Result<FkPose>::failure(validation(
                    "IK chain must be a contiguous ancestor line root-first"));
            }
        }
    }
    return evaluate_fk(graph, base_locals);
}

core::Result<core::Transform> invert_uniform(const core::Transform& transform) {
    if (!transform.finite()) {
        return core::Result<core::Transform>::failure(invalid(
            "cannot invert a non-finite transform"));
    }
    const core::Vec3d scale = transform.scale;
    if (std::abs(scale.x - scale.y) > 1e-12 || std::abs(scale.y - scale.z) > 1e-12 ||
        std::abs(scale.x) <= 1e-12) {
        return core::Result<core::Transform>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "transform inversion requires uniform non-zero scale"));
    }
    const core::Quaternion rotation_inverse = inverse(transform.rotation);
    const double inverse_scale = 1.0 / scale.x;
    return core::Result<core::Transform>::success(core::Transform{
        rotation_inverse.rotate(transform.translation * -inverse_scale),
        rotation_inverse,
        {inverse_scale, inverse_scale, inverse_scale},
    });
}

// Clamps a solved local rotation back to the joint's enabled angular limits,
// expressed relative to the base local rotation under the library Euler
// convention. Returns the (possibly re-clamped) local rotation.
core::Quaternion clamp_relative_rotation(
    const core::Quaternion& base_local_rotation,
    const core::Quaternion& solved_local_rotation,
    const kinematic::JointLimit* limit) {
    if (limit == nullptr || !limit->angular_enabled) {
        return solved_local_rotation;
    }
    const core::Quaternion relative =
        (inverse(base_local_rotation) * solved_local_rotation).normalized();
    core::Vec3d euler = to_euler_xyz(relative);
    euler.x = std::clamp(euler.x, limit->angular_min.x, limit->angular_max.x);
    euler.y = std::clamp(euler.y, limit->angular_min.y, limit->angular_max.y);
    euler.z = std::clamp(euler.z, limit->angular_min.z, limit->angular_max.z);
    return (base_local_rotation * from_euler_xyz(euler)).normalized();
}

core::Diagnostic unconverged(const char* solver, double residual, bool reachable) {
    core::Diagnostic diagnostic = validation(std::string(solver) +
        " did not converge within its bounded budget");
    diagnostic = diagnostic.with_context("residual=" + std::to_string(residual));
    diagnostic = diagnostic.with_context(std::string("reachable=") + (reachable ? "true" : "false"));
    return diagnostic;
}

} // namespace

core::Result<void> IkBudget::validate() const {
    return validate_budget(*this);
}

core::Result<IkChainResult> solve_two_bone(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const kinematic::JointLimitSet* limits,
    const TwoBoneIkRequest& request) {
    if (auto result = validate_budget(request.budget); !result) {
        return core::Result<IkChainResult>::failure(result.error());
    }
    if (!request.root || !request.mid || !request.effector ||
        request.root == request.mid || request.mid == request.effector ||
        request.root == request.effector) {
        return core::Result<IkChainResult>::failure(invalid(
            "two-bone IK requires three distinct chain joints"));
    }
    if (!request.target.finite()) {
        return core::Result<IkChainResult>::failure(invalid(
            "two-bone IK target must be finite"));
    }
    if (request.pole_vector.has_value() && !request.pole_vector.value().finite()) {
        return core::Result<IkChainResult>::failure(invalid(
            "two-bone IK pole vector must be finite"));
    }
    const std::vector<kinematic::JointId> chain{request.root, request.mid, request.effector};
    auto base = chain_base_pose(graph, base_locals, chain);
    if (!base) return core::Result<IkChainResult>::failure(base.error());
    const FkPose& fk = base.value();
    const auto world = [&](kinematic::JointId joint) -> const core::Transform& {
        return fk.world.at(joint);
    };

    const core::Vec3d a = world(request.root).translation;
    const core::Vec3d b = world(request.mid).translation;
    const core::Vec3d c = world(request.effector).translation;
    const double l1 = (b - a).length();
    const double l2 = (c - b).length();
    if (l1 <= kGeometryEpsilon || l2 <= kGeometryEpsilon) {
        return core::Result<IkChainResult>::failure(validation(
            "two-bone chain has a degenerate bone length"));
    }
    const double distance_raw = (request.target - a).length();
    const double distance = std::clamp(
        distance_raw,
        std::abs(l1 - l2) + kGeometryEpsilon,
        l1 + l2 - kGeometryEpsilon);
    const bool reachable = distance_raw <= l1 + l2 - kGeometryEpsilon &&
        distance_raw >= std::abs(l1 - l2) + kGeometryEpsilon;

    // Aim direction: toward the target; a target on the root itself keeps the
    // authored chain direction rather than inventing one.
    const core::Vec3d base_chain = c - a;
    const core::Vec3d aim = distance_raw > kGeometryEpsilon
        ? (request.target - a) * (1.0 / distance_raw)
        : base_chain.normalized();
    if (!aim.finite()) {
        return core::Result<IkChainResult>::failure(validation(
            "two-bone aim direction is degenerate"));
    }

    // Bend direction: toward the pole (orthogonalized against the aim), or
    // the authored rest bend plane when no pole is declared.
    core::Vec3d bend;
    if (request.pole_vector.has_value()) {
        const core::Vec3d pole = request.pole_vector.value();
        bend = (pole - aim * dot(pole, aim)).normalized();
    } else {
        const core::Vec3d base_aim = base_chain.normalized();
        bend = (b - a - base_aim * dot(b - a, base_aim)).normalized();
    }
    if (!bend.finite()) {
        // Straight rest chain or a pole parallel to the aim: pick the
        // deterministic least-aligned coordinate axis as the bend plane.
        const core::Vec3d fallback = std::abs(aim.x) < 0.9
            ? core::Vec3d{1.0, 0.0, 0.0}
            : core::Vec3d{0.0, 1.0, 0.0};
        bend = cross(aim, fallback).normalized();
    }

    // Law of cosines at the root, then closed-form world positions.
    const double cos_theta = std::clamp(
        (l1 * l1 + distance * distance - l2 * l2) / (2.0 * l1 * distance),
        -1.0, 1.0);
    const double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
    const core::Vec3d b_solved = a + (aim * cos_theta + bend * sin_theta) * l1;
    const core::Vec3d c_solved =
        b_solved + (request.target - b_solved).normalized() * l2;

    // World orientations: shortest-arc deltas from the base pose preserve the
    // authored roll; the effector inherits the mid joint's delta.
    const core::Quaternion root_delta = rotate_between(b - a, b_solved - a);
    const core::Quaternion mid_delta = rotate_between(c - b, c_solved - b_solved);
    const core::Transform root_world_solved{
        a,
        (root_delta * world(request.root).rotation).normalized(),
        world(request.root).scale};
    const core::Transform mid_world_solved{
        b_solved,
        (mid_delta * world(request.mid).rotation).normalized(),
        world(request.mid).scale};
    const core::Transform effector_world_solved{
        c_solved,
        (mid_delta * world(request.effector).rotation).normalized(),
        world(request.effector).scale};

    // Back out locals against each joint's parent frame.
    const kinematic::KinematicNode* root_node = graph.find(request.root);
    core::Result<core::Transform> root_parent = root_node->parent.has_value()
        ? core::Result<core::Transform>::success(fk.world.at(root_node->parent.value()))
        : invert_uniform(fk.root_frame);
    if (!root_parent) return core::Result<IkChainResult>::failure(root_parent.error());
    auto root_local = local_from_world(root_parent.value(), root_world_solved);
    if (!root_local) return core::Result<IkChainResult>::failure(root_local.error());
    auto mid_local = local_from_world(root_world_solved, mid_world_solved);
    if (!mid_local) return core::Result<IkChainResult>::failure(mid_local.error());
    auto effector_local = local_from_world(mid_world_solved, effector_world_solved);
    if (!effector_local) return core::Result<IkChainResult>::failure(effector_local.error());

    // Joint-limit clamping (kinematic records win over the solve).
    const auto limit_for = [&](kinematic::JointId joint) -> const kinematic::JointLimit* {
        return limits == nullptr ? nullptr : limits->find(joint);
    };
    const auto base_local_rotation = [&](kinematic::JointId joint) -> core::Quaternion {
        const auto override = base_locals.find(joint);
        const core::Transform base_local = override != base_locals.end()
            ? override->second
            : graph.find(joint)->rest;
        return base_local.rotation;
    };
    root_local.value().rotation = clamp_relative_rotation(
        base_local_rotation(request.root), root_local.value().rotation, limit_for(request.root));
    mid_local.value().rotation = clamp_relative_rotation(
        base_local_rotation(request.mid), mid_local.value().rotation, limit_for(request.mid));
    effector_local.value().rotation = clamp_relative_rotation(
        base_local_rotation(request.effector), effector_local.value().rotation,
        limit_for(request.effector));

    // Honest residual: FK the clamped locals and measure where the effector
    // actually landed, not where the closed form put it.
    LocalPose solved;
    solved.emplace(request.root, root_local.value());
    solved.emplace(request.mid, mid_local.value());
    solved.emplace(request.effector, effector_local.value());
    auto final_fk = evaluate_fk(graph, solved);
    if (!final_fk) return core::Result<IkChainResult>::failure(final_fk.error());
    const double residual =
        (final_fk.value().world.at(request.effector).translation - request.target).length();
    const bool converged = reachable &&
        residual <= std::max(request.budget.convergence_tolerance, 1e-9);
    IkChainResult result{std::move(solved), residual, 1U, converged};
    if (!converged && !request.allow_partial) {
        return core::Result<IkChainResult>::failure(
            unconverged("two-bone IK", residual, reachable));
    }
    return core::Result<IkChainResult>::success(std::move(result));
}

core::Result<IkChainResult> solve_chain_jacobian(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const kinematic::JointLimitSet* limits,
    const JacobianIkRequest& request) {
    if (auto result = validate_budget(request.budget); !result) {
        return core::Result<IkChainResult>::failure(result.error());
    }
    if (!request.target.finite()) {
        return core::Result<IkChainResult>::failure(invalid(
            "Jacobian IK target must be finite"));
    }
    auto base = chain_base_pose(graph, base_locals, request.chain);
    if (!base) return core::Result<IkChainResult>::failure(base.error());
    const kinematic::JointId effector = request.chain.back();

    const auto limit_for = [&](kinematic::JointId joint) -> const kinematic::JointLimit* {
        return limits == nullptr ? nullptr : limits->find(joint);
    };
    const auto base_local = [&](kinematic::JointId joint) -> core::Transform {
        const auto override = base_locals.find(joint);
        return override != base_locals.end() ? override->second : graph.find(joint)->rest;
    };

    // Current locals start at base. Each chain joint's solved rotation is
    // tracked relative to base under the library Euler convention so enabled
    // angular limits clamp exactly (CPU reference small-angle model).
    LocalPose current_locals = base_locals;
    std::map<kinematic::JointId, core::Quaternion> relative_rotations;
    for (const kinematic::JointId joint : request.chain) {
        relative_rotations.emplace(joint, core::Quaternion::identity());
    }

    auto fk = evaluate_fk(graph, current_locals);
    if (!fk) return core::Result<IkChainResult>::failure(fk.error());
    double error =
        (fk.value().world.at(effector).translation - request.target).length();
    std::uint32_t iterations = 0U;
    const auto started = std::chrono::steady_clock::now();
    bool time_expired = false;

    while (error > request.budget.convergence_tolerance &&
           iterations < request.budget.max_iterations && !time_expired) {
        // Jacobian columns: each chain joint's world rotation axis crossed
        // with the effector lever arm. Deterministic column order: chain
        // order, then x/y/z.
        std::vector<core::Vec3d> columns;
        columns.reserve(request.chain.size() * 3U);
        for (const kinematic::JointId joint : request.chain) {
            const core::Transform& joint_world = fk.value().world.at(joint);
            const core::Vec3d lever =
                fk.value().world.at(effector).translation - joint_world.translation;
            for (core::Vec3d axis : {core::Vec3d{1.0, 0.0, 0.0},
                                     core::Vec3d{0.0, 1.0, 0.0},
                                     core::Vec3d{0.0, 0.0, 1.0}}) {
                columns.push_back(cross(joint_world.rotation.rotate(axis), lever));
            }
        }

        // Damped least squares: dtheta = J^T (J J^T + lambda^2 I)^-1 e.
        const core::Vec3d step_vector =
            request.target - fk.value().world.at(effector).translation;
        double normal[3][3];
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                double sum = 0.0;
                for (const core::Vec3d& j_column : columns) {
                    const double jr =
                        row == 0 ? j_column.x : (row == 1 ? j_column.y : j_column.z);
                    const double jc =
                        column == 0 ? j_column.x : (column == 1 ? j_column.y : j_column.z);
                    sum += jr * jc;
                }
                normal[row][column] = sum;
            }
        }
        const double lambda2 = request.budget.damping * request.budget.damping;
        normal[0][0] += lambda2;
        normal[1][1] += lambda2;
        normal[2][2] += lambda2;
        double rhs[3] = {step_vector.x, step_vector.y, step_vector.z};
        // Gaussian elimination with partial pivot on the 3x3 normal system.
        for (int pivot = 0; pivot < 3; ++pivot) {
            int best = pivot;
            for (int row = pivot + 1; row < 3; ++row) {
                if (std::abs(normal[row][pivot]) > std::abs(normal[best][pivot])) {
                    best = row;
                }
            }
            if (std::abs(normal[best][pivot]) <= 1e-15) {
                return core::Result<IkChainResult>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "Jacobian IK hit a singular normal system even with damping"));
            }
            if (best != pivot) {
                for (int column = pivot; column < 3; ++column) {
                    std::swap(normal[pivot][column], normal[best][column]);
                }
                std::swap(rhs[pivot], rhs[best]);
            }
            for (int row = pivot + 1; row < 3; ++row) {
                const double factor = normal[row][pivot] / normal[pivot][pivot];
                for (int column = pivot; column < 3; ++column) {
                    normal[row][column] -= factor * normal[pivot][column];
                }
                rhs[row] -= factor * rhs[pivot];
            }
        }
        double solution[3];
        for (int row = 2; row >= 0; --row) {
            double sum = rhs[row];
            for (int column = row + 1; column < 3; ++column) {
                sum -= normal[row][column] * solution[column];
            }
            solution[row] = sum / normal[row][row];
        }

        // Per-joint updates: dtheta_i = J_i . solution, clamped through the
        // joint's enabled angular limits each iteration.
        for (std::size_t joint_index = 0U; joint_index < request.chain.size();
             ++joint_index) {
            const kinematic::JointId joint = request.chain[joint_index];
            const core::Vec3d delta{
                dot(columns[joint_index * 3U + 0U],
                    {solution[0], solution[1], solution[2]}),
                dot(columns[joint_index * 3U + 1U],
                    {solution[0], solution[1], solution[2]}),
                dot(columns[joint_index * 3U + 2U],
                    {solution[0], solution[1], solution[2]}),
            };
            const kinematic::JointLimit* limit = limit_for(joint);
            core::Quaternion& relative = relative_rotations.at(joint);
            core::Vec3d euler = to_euler_xyz(relative);
            euler = euler + delta;
            if (limit != nullptr && limit->angular_enabled) {
                euler.x = std::clamp(euler.x, limit->angular_min.x, limit->angular_max.x);
                euler.y = std::clamp(euler.y, limit->angular_min.y, limit->angular_max.y);
                euler.z = std::clamp(euler.z, limit->angular_min.z, limit->angular_max.z);
            }
            relative = from_euler_xyz(euler);
            core::Transform local = base_local(joint);
            local.rotation = (local.rotation * relative).normalized();
            current_locals[joint] = local;
        }

        ++iterations;
        fk = evaluate_fk(graph, current_locals);
        if (!fk) return core::Result<IkChainResult>::failure(fk.error());
        error = (fk.value().world.at(effector).translation - request.target).length();
        if (request.budget.max_seconds > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                    .count() > request.budget.max_seconds) {
            time_expired = true;
        }
    }

    LocalPose solved;
    for (const kinematic::JointId joint : request.chain) {
        solved.emplace(joint, current_locals.at(joint));
    }
    const bool reachable = true; // bounded-DLS posture: residual is the honest measure
    const bool converged = !time_expired &&
        error <= std::max(request.budget.convergence_tolerance, 1e-12);
    IkChainResult result{std::move(solved), error, iterations, converged};
    if (!converged && !request.allow_partial) {
        core::Diagnostic diagnostic =
            unconverged("Jacobian chain IK", error, reachable);
        if (time_expired) {
            diagnostic = diagnostic.with_context("stop=time-budget");
        } else {
            diagnostic = diagnostic.with_context("stop=iteration-budget");
        }
        return core::Result<IkChainResult>::failure(std::move(diagnostic));
    }
    return core::Result<IkChainResult>::success(std::move(result));
}

} // namespace carto::pose

