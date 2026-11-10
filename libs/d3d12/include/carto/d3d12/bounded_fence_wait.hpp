#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace carto::d3d12::detail {

enum class FenceWaitOutcome { complete, timeout, invalid_completion, registration_failed, wait_failed };
enum class FenceEventOutcome { signaled, timeout, failed };

// Event notification is only a wake-up: an earlier timed-out registration can
// signal the same auto-reset event later. The fence value is the completion proof.
template<class Observe, class Register, class Wait, class Now>
[[nodiscard]] FenceWaitOutcome bounded_fence_wait(
    std::uint64_t target, std::chrono::steady_clock::time_point deadline,
    Observe&& observe, Register&& register_event, Wait&& wait_event, Now&& now) {
    bool registered = false;
    for (;;) {
        const auto completed = observe();
        if (completed == std::numeric_limits<std::uint64_t>::max())
            return FenceWaitOutcome::invalid_completion;
        if (completed >= target) return FenceWaitOutcome::complete;
        const auto current = now();
        if (current >= deadline) return FenceWaitOutcome::timeout;
        if (!registered) {
            if (!register_event(target)) return FenceWaitOutcome::registration_failed;
            registered = true;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - current);
        const auto millis = std::clamp<std::int64_t>(remaining.count(), 1, 0xfffffffeLL);
        if (wait_event(static_cast<std::uint32_t>(millis)) == FenceEventOutcome::failed)
            return FenceWaitOutcome::wait_failed;
    }
}

} // namespace carto::d3d12::detail
