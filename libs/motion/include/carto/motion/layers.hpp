#pragma once

#include <carto/character/motion_document.hpp>
#include <carto/character/time.hpp>
#include <carto/motion/curve.hpp>
#include <carto/motion/ids.hpp>

#include <carto/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carto::motion {

enum class LayerKind : std::uint8_t {
    // The single foundation clip of the stack.
    base,
    // Replaces the running value toward the layer value by its weight.
    override_layer,
    // Adds weight * (layer value - additive reference) to the running value.
    additive,
};

[[nodiscard]] std::string_view layer_kind_name(LayerKind kind) noexcept;

// One curve a layer addresses at one motion target. Curves are sorted by
// target within a layer so compositing order never depends on input order.
struct LayerCurve {
    character::MotionTargetRef target;
    KeyedCurve curve;

    [[nodiscard]] core::Result<void> validate() const;
    [[nodiscard]] bool operator<(const LayerCurve& other) const noexcept {
        return target < other.target;
    }
};

// One animation layer of the stack. Compositing order is the ascending layer
// identity order and is content: reordering layers changes the result and is
// a revisioned edit, never an implicit sort. The weight is clamped to [0, 1];
// the window is an inclusive rational-time range on the stack timebase and a
// layer outside its window contributes nothing but still occupies its order
// slot. Muted layers are skipped.
struct AnimationLayer {
    static constexpr std::size_t kMaxCurvesPerLayer = 1'024U;

    AnimationLayerId id;
    LayerKind kind = LayerKind::override_layer;
    double weight = 1.0;
    bool muted = false;
    character::RationalTime window_start{0, 0};
    character::RationalTime window_end{0, 0};
    // Pose value subtracted before additive contribution; zero means "the
    // additive layer stores deltas outright".
    double additive_reference = 0.0;
    std::vector<LayerCurve> curves;

    [[nodiscard]] core::Result<void> validate() const;
};

// Receipt from checking every layer curve against the motion document's
// allowed target set. With MissingTargetHandling::fail the check itself is a
// hard error; with skip_with_receipt the offenders are reported here, sorted
// and unique, and are the only curves the caller must exclude downstream.
struct TargetCheckReceipt {
    static constexpr std::size_t kMaxSkipped = 4'096U;

    std::vector<character::MotionTargetRef> skipped_targets;

    [[nodiscard]] core::Result<void> validate() const;
};

// The evaluated scalar for one target at one rational time. Only targets
// actually addressed by the base layer or at least one layer are evaluable;
// anything else is an honest not_found, never a silent zero.
struct LayerSample {
    double value = 0.0;
};

class AnimationLayerStack final {
public:
    static constexpr std::uint32_t kSchemaVersion = 1U;
    static constexpr std::size_t kMaxSerializedBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaxLayers = 256U;

    AnimationLayerId base_layer;
    std::uint32_t timebase_ticks_per_second = 0U;
    // Sorted and unique by layer id; exactly one layer is the base layer.
    std::vector<AnimationLayer> layers;

    [[nodiscard]] core::Result<void> validate() const;

    // Fails closed against the document's allowed target set per its
    // MissingTargetHandling policy; on skip_with_receipt the offenders are
    // returned in the receipt.
    [[nodiscard]] core::Result<TargetCheckReceipt> check_targets(
        const character::MotionDocument& document) const;

    // Deterministic compositing: base sample first, then every active layer
    // in ascending id order. The time must be on the stack timebase.
    [[nodiscard]] core::Result<LayerSample> evaluate(
        const character::MotionTargetRef& target,
        character::RationalTime time) const;
};

} // namespace carto::motion
