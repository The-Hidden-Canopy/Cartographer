#include <carto/pose/contact.hpp>

#include <carto/character/time.hpp>

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

} // namespace

core::Result<ContactHoldReport> evaluate_contact_hold(
    const character::ConstraintGraph& constraints,
    const kinematic::KinematicGraph& graph,
    const std::map<character::ConstraintId, core::Vec3d>& pin_targets,
    const std::function<core::Result<LocalPose>(character::RationalTime)>& base_pose_at,
    const ContactHoldRequest& request) {
    if (auto result = constraints.validate(); !result) {
        return core::Result<ContactHoldReport>::failure(result.error().with_context(
            "contact hold requires a valid constraint graph"));
    }
    if (auto result = graph.validate(); !result) {
        return core::Result<ContactHoldReport>::failure(result.error().with_context(
            "contact hold requires a valid kinematic graph"));
    }
    if (request.samples < 2U || request.samples > ContactHoldRequest::kMaxSamples) {
        return core::Result<ContactHoldReport>::failure(invalid(
            "contact hold sample count must be within its bounded range"));
    }
    if (!character::same_timebase(request.window_start, request.window_end)) {
        return core::Result<ContactHoldReport>::failure(invalid(
            "contact hold window must share one rational timebase"));
    }
    if (auto result = request.window_start.validate(); !result) {
        return core::Result<ContactHoldReport>::failure(result.error());
    }
    if (auto result = request.window_end.validate(); !result) {
        return core::Result<ContactHoldReport>::failure(result.error());
    }
    if (character::compare(request.window_end, request.window_start) ==
        std::strong_ordering::less) {
        return core::Result<ContactHoldReport>::failure(validation(
            "contact hold window must not invert"));
    }
    const std::int64_t span = request.window_end.tick - request.window_start.tick;
    const std::int64_t steps = static_cast<std::int64_t>(request.samples) - 1;
    if (span % steps != 0) {
        return core::Result<ContactHoldReport>::failure(validation(
            "contact hold sample grid must divide the rational window exactly"));
    }
    const std::int64_t step = span / steps;

    struct ContactProbe {
        character::ConstraintId id;
        kinematic::JointId joint;
        core::Vec3d goal;
        double tolerance;
    };
    std::vector<ContactProbe> probes;
    for (const character::ConstraintId id : request.contacts) {
        const character::ConstraintDefinition* constraint =
            [&]() -> const character::ConstraintDefinition* {
                const auto found = constraints.constraints().find(id);
                return found == constraints.constraints().end() ? nullptr : &found->second;
            }();
        if (constraint == nullptr) {
            return core::Result<ContactHoldReport>::failure(invalid(
                "contact hold references a missing constraint"));
        }
        if (constraint->type != character::ConstraintType::contact ||
            !constraint->target.is_joint()) {
            return core::Result<ContactHoldReport>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "contact hold only accepts contact constraints on joints"));
        }
        if (!character::same_timebase(constraint->validity_start, request.window_start) ||
            character::compare(constraint->validity_start, request.window_start) ==
                std::strong_ordering::greater ||
            character::compare(request.window_end, constraint->validity_end) ==
                std::strong_ordering::greater) {
            return core::Result<ContactHoldReport>::failure(validation(
                "contact constraint validity window must cover the hold window"));
        }
        const auto goal = pin_targets.find(id);
        if (goal == pin_targets.end() || !goal->second.finite()) {
            return core::Result<ContactHoldReport>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "contact hold requires a finite authored goal position per contact"));
        }
        probes.push_back(ContactProbe{id, constraint->target.joint, goal->second,
            constraint->tolerance});
    }
    if (probes.empty()) {
        return core::Result<ContactHoldReport>::failure(invalid(
            "contact hold requires at least one contact constraint"));
    }

    ContactHoldReport report;
    report.window_start = request.window_start;
    report.window_end = request.window_end;
    report.sample_count = request.samples;
    for (const ContactProbe& probe : probes) {
        report.worst_residuals.emplace(probe.id, 0.0);
    }
    report.held = true;

    // Every intermediate sample is evaluated; start and endpoints included on
    // the exact rational grid.
    for (std::uint32_t sample = 0U; sample < request.samples; ++sample) {
        const character::RationalTime time{
            request.window_start.tick + step * static_cast<std::int64_t>(sample),
            request.window_start.ticks_per_second,
        };
        auto locals = base_pose_at(time);
        if (!locals) {
            return core::Result<ContactHoldReport>::failure(locals.error().with_context(
                "contact hold sample evaluation failed"));
        }
        auto fk = evaluate_fk(graph, locals.value());
        if (!fk) {
            return core::Result<ContactHoldReport>::failure(fk.error().with_context(
                "contact hold sample FK failed"));
        }
        for (const ContactProbe& probe : probes) {
            const core::Vec3d position =
                fk.value().world.at(probe.joint).translation;
            const double residual = (position - probe.goal).length();
            double& worst = report.worst_residuals.at(probe.id);
            worst = std::max(worst, residual);
            report.worst_residual = std::max(report.worst_residual, residual);
            if (residual > probe.tolerance) report.held = false;
        }
    }
    return core::Result<ContactHoldReport>::success(std::move(report));
}

} // namespace carto::pose
