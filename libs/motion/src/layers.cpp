#include <carto/motion/layers.hpp>

#include <algorithm>
#include <compare>
#include <set>
#include <utility>

namespace carto::motion {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid_layer_kind(LayerKind kind) {
    switch (kind) {
    case LayerKind::base:
    case LayerKind::override_layer:
    case LayerKind::additive:
        return true;
    }
    return false;
}

} // namespace

std::string_view layer_kind_name(LayerKind kind) noexcept {
    switch (kind) {
    case LayerKind::base: return "base";
    case LayerKind::override_layer: return "override";
    case LayerKind::additive: return "additive";
    }
    return "unknown";
}

core::Result<void> LayerCurve::validate() const {
    if (auto result = target.validate(); !result) return result;
    if (auto result = curve.validate(); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> AnimationLayer::validate() const {
    if (!id) {
        return core::Result<void>::failure(invalid(
            "animation layer requires a stable identity"));
    }
    if (!valid_layer_kind(kind)) {
        return core::Result<void>::failure(validation(
            "animation layer kind is unknown"));
    }
    if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
        return core::Result<void>::failure(invalid(
            "animation layer weight must be finite and within [0, 1]"));
    }
    if (auto result = window_start.validate(); !result) return result;
    if (auto result = window_end.validate(); !result) return result;
    if (character::compare(window_start, window_end) ==
            std::strong_ordering::greater) {
        return core::Result<void>::failure(validation(
            "animation layer window start must not exceed its end"));
    }
    if (!std::isfinite(additive_reference)) {
        return core::Result<void>::failure(invalid(
            "animation layer additive reference must be finite"));
    }
    if (curves.size() > kMaxCurvesPerLayer) {
        return core::Result<void>::failure(validation(
            "animation layer curve collection exceeds its bounded capacity"));
    }
    for (const LayerCurve& layer_curve : curves) {
        if (auto result = layer_curve.validate(); !result) return result;
        if (layer_curve.curve.timebase_ticks_per_second != window_start.ticks_per_second) {
            return core::Result<void>::failure(validation(
                "animation layer curves and windows must share one timebase"));
        }
    }
    for (std::size_t index = 1U; index < curves.size(); ++index) {
        if (!(curves[index - 1U] < curves[index])) {
            return core::Result<void>::failure(validation(
                "animation layer curves must be sorted by target and unique"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> TargetCheckReceipt::validate() const {
    if (skipped_targets.size() > kMaxSkipped) {
        return core::Result<void>::failure(validation(
            "target check receipt exceeds its bounded capacity"));
    }
    for (const character::MotionTargetRef& target : skipped_targets) {
        if (auto result = target.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < skipped_targets.size(); ++index) {
        if (!(skipped_targets[index - 1U] < skipped_targets[index])) {
            return core::Result<void>::failure(validation(
                "skipped targets must be sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> AnimationLayerStack::validate() const {
    if (!base_layer) {
        return core::Result<void>::failure(invalid(
            "animation layer stack requires a base layer identity"));
    }
    if (timebase_ticks_per_second == 0U ||
        timebase_ticks_per_second > character::MotionDocument::kMaxTicksPerSecond) {
        return core::Result<void>::failure(invalid(
            "animation layer stack timebase is out of range"));
    }
    if (layers.empty() || layers.size() > kMaxLayers) {
        return core::Result<void>::failure(validation(
            "animation layer stack requires between one and its bounded capacity of layers"));
    }
    bool base_seen = false;
    for (const AnimationLayer& layer : layers) {
        if (auto result = layer.validate(); !result) return result;
        if (layer.window_start.ticks_per_second != timebase_ticks_per_second) {
            return core::Result<void>::failure(validation(
                "animation layer windows must use the stack timebase"));
        }
        if (layer.id == base_layer) {
            if (base_seen || layer.kind != LayerKind::base) {
                return core::Result<void>::failure(validation(
                    "exactly one layer may be the base layer"));
            }
            base_seen = true;
        } else if (layer.kind == LayerKind::base) {
            return core::Result<void>::failure(validation(
                "only the designated base layer may have base kind"));
        }
    }
    if (!base_seen) {
        return core::Result<void>::failure(validation(
            "the designated base layer identity is not present in the stack"));
    }
    for (std::size_t index = 1U; index < layers.size(); ++index) {
        if (!(layers[index - 1U].id.value < layers[index].id.value)) {
            return core::Result<void>::failure(validation(
                "animation layers must be sorted by identity and unique"));
        }
    }
    return core::Result<void>::success();
}

core::Result<TargetCheckReceipt> AnimationLayerStack::check_targets(
    const character::MotionDocument& document) const {
    if (auto result = validate(); !result) {
        return core::Result<TargetCheckReceipt>::failure(result.error());
    }
    if (auto result = document.validate(); !result) {
        return core::Result<TargetCheckReceipt>::failure(result.error());
    }
    if (document.timebase_ticks_per_second != timebase_ticks_per_second) {
        return core::Result<TargetCheckReceipt>::failure(validation(
            "motion document and layer stack timebases must agree"));
    }
    std::set<character::MotionTargetRef> offenders;
    for (const AnimationLayer& layer : layers) {
        for (const LayerCurve& layer_curve : layer.curves) {
            const bool allowed = std::binary_search(
                document.targets.begin(), document.targets.end(), layer_curve.target);
            if (!allowed) offenders.insert(layer_curve.target);
        }
    }
    if (offenders.empty()) {
        return core::Result<TargetCheckReceipt>::success(TargetCheckReceipt{});
    }
    if (document.missing_target == character::MissingTargetHandling::fail) {
        core::Diagnostic diagnostic = validation(
            "animation layer addresses a target outside the motion document");
        for (const character::MotionTargetRef& offender : offenders) {
            diagnostic = diagnostic.with_context(
                offender.joint.value != 0U
                    ? "joint:" + std::to_string(offender.joint.value)
                    : "control:" + std::to_string(offender.control.value));
        }
        return core::Result<TargetCheckReceipt>::failure(std::move(diagnostic));
    }
    TargetCheckReceipt receipt;
    receipt.skipped_targets.assign(offenders.begin(), offenders.end());
    if (auto result = receipt.validate(); !result) {
        return core::Result<TargetCheckReceipt>::failure(result.error());
    }
    return core::Result<TargetCheckReceipt>::success(std::move(receipt));
}

core::Result<LayerSample> AnimationLayerStack::evaluate(
    const character::MotionTargetRef& target,
    character::RationalTime time) const {
    if (auto result = target.validate(); !result) {
        return core::Result<LayerSample>::failure(result.error());
    }
    if (time.ticks_per_second != timebase_ticks_per_second) {
        return core::Result<LayerSample>::failure(invalid(
            "evaluation time must be on the stack timebase"));
    }
    const AnimationLayer* base = nullptr;
    for (const AnimationLayer& layer : layers) {
        if (layer.id == base_layer) {
            base = &layer;
            break;
        }
    }
    auto base_curve = std::lower_bound(
        base->curves.begin(), base->curves.end(), target,
        [](const LayerCurve& curve, const character::MotionTargetRef& wanted) {
            return curve.target < wanted;
        });
    if (base_curve == base->curves.end() || !(base_curve->target == target)) {
        return core::Result<LayerSample>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "motion target is not addressed by the base layer"));
    }
    const auto base_sample = base_curve->curve.sample(time);
    if (!base_sample) return core::Result<LayerSample>::failure(base_sample.error());
    double value = base_sample.value();

    // Deterministic compositing: layers apply in ascending identity order.
    // A layer outside its window or muted occupies its slot but contributes
    // nothing.
    for (const AnimationLayer& layer : layers) {
        if (layer.id == base_layer || layer.muted) continue;
        if (character::compare(time, layer.window_start) ==
                std::strong_ordering::less ||
            character::compare(time, layer.window_end) ==
                std::strong_ordering::greater) {
            continue;
        }
        auto found = std::lower_bound(
            layer.curves.begin(), layer.curves.end(), target,
            [](const LayerCurve& curve, const character::MotionTargetRef& wanted) {
                return curve.target < wanted;
            });
        if (found == layer.curves.end() || !(found->target == target)) continue;
        const auto layer_sample = found->curve.sample(time);
        if (!layer_sample) return core::Result<LayerSample>::failure(layer_sample.error());
        if (layer.kind == LayerKind::override_layer) {
            value = value + layer.weight * (layer_sample.value() - value);
        } else {
            value = value +
                layer.weight * (layer_sample.value() - layer.additive_reference);
        }
    }
    return core::Result<LayerSample>::success(LayerSample{value});
}

} // namespace carto::motion
