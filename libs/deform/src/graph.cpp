#include <carto/deform/graph.hpp>

#include <carto/core/diagnostic.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace carto::deform {

std::string_view morph_space_name(MorphSpace space) noexcept {
    switch (space) {
    case MorphSpace::bind_pre_skin:
        return "bind_pre_skin";
    case MorphSpace::post_skin_model:
        return "post_skin_model";
    }
    return "unknown";
}

core::Result<void> DeformationStage::validate() const {
    switch (kind) {
    case Kind::skin:
        return core::Result<void>::success();
    case Kind::morph:
        if (!morph_target) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "morph stage requires a morph target id"));
        }
        if (!std::isfinite(morph_weight) || morph_weight < 0.0 || morph_weight > 1.0) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "morph stage weight must be finite and within [0, 1]"));
        }
        return core::Result<void>::success();
    }
    return core::Result<void>::failure(core::Diagnostic(
        core::ErrorCode::invalid_argument, "unknown deformation stage kind"));
}

core::Result<void> DeformationGraph::validate() const {
    if (stages.empty() || stages.size() > kMaxStages) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "deformation graph must declare 1..64 stages"));
    }
    std::size_t skin_stages = 0U;
    std::size_t skin_position = 0U;
    for (std::size_t index = 0U; index < stages.size(); ++index) {
        const core::Result<void> stage_valid = stages[index].validate();
        if (!stage_valid) {
            return stage_valid;
        }
        if (stages[index].kind == DeformationStage::Kind::skin) {
            ++skin_stages;
            skin_position = index;
        }
    }
    if (skin_stages != 1U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "deformation graph requires exactly one skin stage"));
    }
    std::vector<character::MorphTargetId> morph_targets;
    for (std::size_t index = 0U; index < stages.size(); ++index) {
        const DeformationStage& stage = stages[index];
        if (stage.kind != DeformationStage::Kind::morph) {
            continue;
        }
        if (stage.space == MorphSpace::bind_pre_skin && index > skin_position) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "bind-space morph stage must precede the skin stage"));
        }
        if (stage.space == MorphSpace::post_skin_model && index < skin_position) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "post-skin morph stage must follow the skin stage"));
        }
        morph_targets.push_back(stage.morph_target);
    }
    std::sort(morph_targets.begin(), morph_targets.end());
    if (std::adjacent_find(morph_targets.begin(), morph_targets.end()) !=
        morph_targets.end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "deformation graph applies a morph target more than once"));
    }
    return core::Result<void>::success();
}

core::Result<DeformationEvaluationResult> evaluate(
    const DeformationGraph& graph,
    const DeformationEvaluationInput& input) {
    const core::Result<void> graph_valid = graph.validate();
    if (!graph_valid) {
        return core::Result<DeformationEvaluationResult>::failure(graph_valid.error());
    }
    if (input.binding == nullptr || input.poses == nullptr) {
        return core::Result<DeformationEvaluationResult>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "deformation evaluation requires a binding and a joint pose set"));
    }
    if (input.bind_positions.empty()) {
        return core::Result<DeformationEvaluationResult>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "deformation evaluation requires bind positions"));
    }
    if (input.mesh_content_digest != input.binding->mesh_digest) {
        return core::Result<DeformationEvaluationResult>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "deformation graph source mesh digest does not match the binding"));
    }

    // Corrective drivers are resolved once per evaluation in declared order;
    // chained inputs see the results of earlier drivers in the same pass.
    std::map<character::CorrectiveDriverId, DriverEvaluationResult> driver_weights;
    if (!input.corrective_drivers.empty()) {
        const core::Result<
            std::map<character::CorrectiveDriverId, DriverEvaluationResult>>
            evaluated = evaluate_driver_stack(
                input.corrective_drivers, input.driver_channels);
        if (!evaluated) {
            return core::Result<DeformationEvaluationResult>::failure(evaluated.error());
        }
        driver_weights = evaluated.value();
    }

    // The canonical bind positions stay index-stable for the whole stack, so
    // every stage's source-mesh attestation binds against the graph-level
    // mesh digest even after earlier stages have moved the points.
    std::vector<core::Vec3d> current(
        input.bind_positions.begin(), input.bind_positions.end());
    std::vector<StageReceipt> stage_receipts;
    stage_receipts.reserve(graph.stages.size());

    for (const DeformationStage& stage : graph.stages) {
        if (stage.kind == DeformationStage::Kind::skin) {
            // The skin stage deforms the current bind-space stream (the
            // canonical bind positions plus any pre-skin morph deltas); the
            // correspondence attestation was verified at graph entry.
            SkinEvaluationInput skin_input{
                current,
                input.mesh_content_digest,
                false,
                input.binding,
                input.poses,
            };
            const core::Result<SkinEvaluationResult> skinned =
                stage.method == SkinMethod::linear_blend
                ? evaluate_lbs(skin_input)
                : evaluate_dqs(skin_input);
            if (!skinned) {
                return core::Result<DeformationEvaluationResult>::failure(skinned.error());
            }
            current = skinned.value().positions;
            const core::Result<assets::Sha256Digest> binding_digest =
                input.binding->canonical_digest();
            if (!binding_digest) {
                return core::Result<DeformationEvaluationResult>::failure(
                    binding_digest.error());
            }
            stage_receipts.push_back(StageReceipt{
                std::string{"skin:"} + std::string{skin_method_name(stage.method)},
                positions_digest(current),
                binding_digest.value(),
            });
            continue;
        }

        const auto morph_source = input.morphs.find(stage.morph_target);
        if (morph_source == input.morphs.end() || morph_source->second.target == nullptr) {
            return core::Result<DeformationEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::not_found,
                "deformation graph references a morph target with no source"));
        }
        double weight = stage.morph_weight;
        if (stage.corrective_driver.has_value()) {
            const auto driver = driver_weights.find(*stage.corrective_driver);
            if (driver == driver_weights.end()) {
                return core::Result<DeformationEvaluationResult>::failure(
                    core::Diagnostic(
                        core::ErrorCode::not_found,
                        "morph stage references a corrective driver outside the "
                        "evaluated stack"));
            }
            weight *= driver->second.weight;
        }
        MorphApplicationInput morph_input{
            current,
            morph_source->second.target,
            morph_source->second.delta_blob,
            input.mesh_content_digest,
            morph_source->second.topology_correspondence_digest,
            weight,
        };
        const core::Result<MorphApplicationResult> morphed =
            apply_morph_target(morph_input);
        if (!morphed) {
            return core::Result<DeformationEvaluationResult>::failure(morphed.error());
        }
        current = morphed.value().positions;
        std::string label = std::string{"morph:"} +
            std::to_string(stage.morph_target.value);
        if (stage.corrective_driver.has_value()) {
            label += "+corrective:" +
                std::to_string(stage.corrective_driver->value);
        }
        stage_receipts.push_back(StageReceipt{
            std::move(label),
            positions_digest(current),
            morph_policy_digest(*morph_source->second.target),
        });
    }

    DeformationEvaluationResult result;
    result.positions = std::move(current);
    result.receipt.stages = std::move(stage_receipts);
    if (!input.reference_positions.empty()) {
        if (input.reference_positions.size() != result.positions.size()) {
            return core::Result<DeformationEvaluationResult>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "reference evaluation must match the evaluated point count"));
        }
        const core::Result<DeviationSummary> deviation =
            compute_deviation(result.positions, input.reference_positions);
        if (!deviation) {
            return core::Result<DeformationEvaluationResult>::failure(deviation.error());
        }
        result.receipt.has_reference_deviation = true;
        result.receipt.reference_deviation = deviation.value();
    }
    const core::Result<void> receipt_valid = result.receipt.validate();
    if (!receipt_valid) {
        return core::Result<DeformationEvaluationResult>::failure(receipt_valid.error());
    }
    return core::Result<DeformationEvaluationResult>::success(std::move(result));
}

} // namespace carto::deform
