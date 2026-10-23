#include <carto/pose/ik_fk_match.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
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

core::Result<void> validate_request(const IkFkMatchRequest& request) {
    if (!request.chain.root || !request.chain.mid || !request.chain.effector ||
        request.chain.root == request.chain.mid ||
        request.chain.mid == request.chain.effector ||
        request.chain.root == request.chain.effector) {
        return core::Result<void>::failure(invalid(
            "IK/FK match requires three distinct chain joints"));
    }
    if (!std::isfinite(request.max_position_jump) || request.max_position_jump <= 0.0) {
        return core::Result<void>::failure(invalid(
            "IK/FK match position jump bound must be positive and finite"));
    }
    return request.budget.validate();
}

double rotation_angle_between(const core::Quaternion& first, const core::Quaternion& second) {
    const core::Quaternion a = first.normalized();
    const core::Quaternion b = second.normalized();
    const double dot = std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0 * std::acos(std::clamp(dot, 0.0, 1.0));
}

LocalPose compose(const LocalPose& base, const LocalPose& overrides) {
    LocalPose merged = base;
    for (const auto& [joint, transform] : overrides) merged[joint] = transform;
    return merged;
}

struct ChainWorld {
    core::Vec3d root;
    core::Vec3d mid;
    core::Vec3d effector;
    core::Quaternion effector_rotation;
};

core::Result<ChainWorld> chain_world(
    const kinematic::KinematicGraph& graph,
    const LocalPose& locals,
    const TwoBoneChain& chain) {
    auto posed = evaluate_fk(graph, locals);
    if (!posed) return core::Result<ChainWorld>::failure(posed.error());
    const auto root = posed.value().world.find(chain.root);
    const auto mid = posed.value().world.find(chain.mid);
    const auto effector = posed.value().world.find(chain.effector);
    if (root == posed.value().world.end() || mid == posed.value().world.end() ||
        effector == posed.value().world.end()) {
        return core::Result<ChainWorld>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "IK/FK match chain joint is absent from the graph"));
    }
    return core::Result<ChainWorld>::success(ChainWorld{
        root->second.translation, mid->second.translation,
        effector->second.translation, effector->second.rotation});
}

double position_jump(const ChainWorld& first, const ChainWorld& second) {
    return std::max({(first.root - second.root).length(),
                     (first.mid - second.mid).length(),
                     (first.effector - second.effector).length()});
}

TwoBoneIkRequest ik_request(
    const IkFkMatchRequest& request,
    const IkControlState& ik) {
    TwoBoneIkRequest solve;
    solve.root = request.chain.root;
    solve.mid = request.chain.mid;
    solve.effector = request.chain.effector;
    solve.target = ik.target;
    solve.pole_vector = ik.pole_vector;
    solve.budget = request.budget;
    solve.allow_partial = true;
    return solve;
}

LocalPose chain_fk_locals(const LocalPose& solved, const TwoBoneChain& chain) {
    LocalPose locals;
    for (const kinematic::JointId joint : {chain.root, chain.mid, chain.effector}) {
        const auto found = solved.find(joint);
        if (found != solved.end()) locals.emplace(joint, found->second);
    }
    return locals;
}

} // namespace

core::Result<IkFkMatchResult> match_ik_to_fk(
    const kinematic::KinematicGraph& graph,
    const LocalPose& fk_locals,
    const kinematic::JointLimitSet* limits,
    const IkFkMatchRequest& request) {
    if (auto result = validate_request(request); !result) {
        return core::Result<IkFkMatchResult>::failure(result.error());
    }
    const auto source = chain_world(graph, fk_locals, request.chain);
    if (!source) return core::Result<IkFkMatchResult>::failure(source.error());

    IkControlState ik;
    ik.target = source.value().effector;
    const core::Vec3d aim_vector = source.value().effector - source.value().root;
    const double reach = aim_vector.length();
    if (!std::isfinite(reach) || reach <= 1e-12) {
        return core::Result<IkFkMatchResult>::failure(validation(
            "IK/FK match requires a non-degenerate root-to-effector span"));
    }
    const core::Vec3d aim = aim_vector * (1.0 / reach);
    const core::Vec3d offset = source.value().mid - source.value().root;
    const core::Vec3d bend = offset - aim * core::dot(offset, aim);
    // A bend smaller than this fraction of the reach is a straight limb whose
    // bend plane is undefined; it matches without a pole.
    if (bend.length() > 1e-9 * reach) ik.pole_vector = bend.normalized();

    const auto solved = solve_two_bone(graph, fk_locals, limits, ik_request(request, ik));
    if (!solved) return core::Result<IkFkMatchResult>::failure(solved.error());
    const LocalPose matched_locals = compose(fk_locals, solved.value().solved_locals);
    const auto matched = chain_world(graph, matched_locals, request.chain);
    if (!matched) return core::Result<IkFkMatchResult>::failure(matched.error());

    IkFkMatchResult result;
    result.ik = ik;
    result.fk_locals = chain_fk_locals(solved.value().solved_locals, request.chain);
    result.position_jump = position_jump(source.value(), matched.value());
    result.effector_rotation_jump = rotation_angle_between(
        source.value().effector_rotation, matched.value().effector_rotation);
    result.residual = solved.value().residual;
    if (!(result.position_jump <= request.max_position_jump)) {
        return core::Result<IkFkMatchResult>::failure(validation(
            "switching to IK would move the limb; the match is refused")
            .with_context("position_jump=" + std::to_string(result.position_jump)));
    }
    return core::Result<IkFkMatchResult>::success(std::move(result));
}

core::Result<IkFkMatchResult> match_fk_to_ik(
    const kinematic::KinematicGraph& graph,
    const LocalPose& base_locals,
    const kinematic::JointLimitSet* limits,
    const IkControlState& ik,
    const IkFkMatchRequest& request) {
    if (auto result = validate_request(request); !result) {
        return core::Result<IkFkMatchResult>::failure(result.error());
    }
    if (!ik.target.finite() || (ik.pole_vector.has_value() && !ik.pole_vector->finite())) {
        return core::Result<IkFkMatchResult>::failure(invalid(
            "IK/FK match controls must be finite"));
    }
    const auto solved = solve_two_bone(graph, base_locals, limits, ik_request(request, ik));
    if (!solved) return core::Result<IkFkMatchResult>::failure(solved.error());
    const LocalPose ik_locals = compose(base_locals, solved.value().solved_locals);
    const auto ik_pose = chain_world(graph, ik_locals, request.chain);
    if (!ik_pose) return core::Result<IkFkMatchResult>::failure(ik_pose.error());

    IkFkMatchResult result;
    result.ik = ik;
    result.fk_locals = chain_fk_locals(solved.value().solved_locals, request.chain);
    const auto fk_pose = chain_world(graph, compose(base_locals, result.fk_locals), request.chain);
    if (!fk_pose) return core::Result<IkFkMatchResult>::failure(fk_pose.error());
    result.position_jump = position_jump(ik_pose.value(), fk_pose.value());
    result.effector_rotation_jump = rotation_angle_between(
        ik_pose.value().effector_rotation, fk_pose.value().effector_rotation);
    result.residual = solved.value().residual;
    if (!(result.residual <= request.max_position_jump) ||
        !(result.position_jump <= request.max_position_jump)) {
        return core::Result<IkFkMatchResult>::failure(validation(
            "IK controls do not reach their target; switching to FK would jump")
            .with_context("residual=" + std::to_string(result.residual)));
    }
    return core::Result<IkFkMatchResult>::success(std::move(result));
}

} // namespace carto::pose
