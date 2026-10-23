#pragma once

#include <carto/character/ids.hpp>

namespace carto::motion {

// Strong identities for the motion authoring layer, allocated from the same
// monotonic non-zero idiom as the character foundation. Zero is the invalid
// sentinel and is never assigned.
struct MotionCurveTag;
struct AnimationLayerTag;
struct PerformanceTakeTag;

using MotionCurveId = character::Id<MotionCurveTag>;
using AnimationLayerId = character::Id<AnimationLayerTag>;
using PerformanceTakeId = character::Id<PerformanceTakeTag>;

} // namespace carto::motion
