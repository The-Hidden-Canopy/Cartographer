#include <carto/character/motion_document.hpp>

#include <algorithm>
#include <tuple>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

template <typename Ref>
core::Result<void> validate_ref_sequence(
    const std::vector<Ref>& refs,
    std::size_t maximum,
    const char* label) {
    if (refs.size() > maximum) {
        return core::Result<void>::failure(validation(
            std::string(label) + " collection exceeds its bounded capacity"));
    }
    for (const Ref& ref : refs) {
        if (auto result = ref.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < refs.size(); ++index) {
        if (!(refs[index - 1U] < refs[index])) {
            return core::Result<void>::failure(validation(
                std::string(label) + " references must be sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<void> ClipRef::validate() const {
    if (!clip || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "motion clip reference requires a clip id and content digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> ChannelRef::validate() const {
    if (!channel || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "motion channel reference requires a channel id and content digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> SequenceRef::validate() const {
    if (!sequence || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "motion sequence reference requires a sequence id and content digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> EventRef::validate() const {
    if (!event || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "motion event reference requires an event id and content digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> MotionTargetRef::validate() const {
    if ((joint.value != 0U) == (control.value != 0U)) {
        return core::Result<void>::failure(invalid(
            "motion target must set exactly one joint or control"));
    }
    return core::Result<void>::success();
}

bool MotionTargetRef::operator<(const MotionTargetRef& other) const noexcept {
    return std::tuple{joint.value, control.value} <
        std::tuple{other.joint.value, other.control.value};
}

std::string_view root_motion_policy_name(RootMotionPolicy policy) noexcept {
    switch (policy) {
    case RootMotionPolicy::authored: return "authored";
    case RootMotionPolicy::proposal_only: return "proposal-only";
    case RootMotionPolicy::none: return "none";
    }
    return "unknown";
}

std::string_view missing_target_handling_name(MissingTargetHandling handling) noexcept {
    switch (handling) {
    case MissingTargetHandling::fail: return "fail";
    case MissingTargetHandling::skip_with_receipt: return "skip-with-receipt";
    }
    return "unknown";
}

core::Result<void> MotionDocument::validate() const {
    if (!id || timebase_ticks_per_second == 0U ||
        timebase_ticks_per_second > kMaxTicksPerSecond) {
        return core::Result<void>::failure(invalid(
            "motion document identity or rational timebase is invalid"));
    }
    if (auto result = validate_ref_sequence(clips, kMaxTemporalRefs, "motion clip");
        !result) {
        return result;
    }
    if (auto result = validate_ref_sequence(channels, kMaxTemporalRefs, "motion channel");
        !result) {
        return result;
    }
    if (auto result = validate_ref_sequence(sequences, kMaxTemporalRefs, "motion sequence");
        !result) {
        return result;
    }
    if (auto result = validate_ref_sequence(events, kMaxTemporalRefs, "motion event");
        !result) {
        return result;
    }
    if (targets.size() > kMaxTargets) {
        return core::Result<void>::failure(validation(
            "motion target collection exceeds its bounded capacity"));
    }
    for (const MotionTargetRef& target : targets) {
        if (auto result = target.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < targets.size(); ++index) {
        if (!(targets[index - 1U] < targets[index])) {
            return core::Result<void>::failure(validation(
                "motion targets must be sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
