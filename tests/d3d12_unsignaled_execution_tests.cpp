#include <carto/d3d12/device.hpp>

#include <iostream>
#include <limits>

int main() {
    using carto::d3d12::detail::UnsignaledExecution;
    using carto::gpu::QueueType;
    using carto::gpu::SubmissionSerial;
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; ok = false; }
    };
    UnsignaledExecution state;
    check(!state.active() && !state.arm(0, QueueType::graphics, 0), "invalid arm");
    check(state.arm(9, QueueType::graphics, 2), "execution must arm");
    check(state.active() && state.serial() == 9 && state.context() == 2 &&
        state.queue() == QueueType::graphics && !state.marker_confirmed(), "execution receipt lost");
    check(!state.arm(10, QueueType::graphics, 1), "second execution must not replace receipt");
    check(!state.recovered(10, QueueType::copy) && !state.recovered(9, QueueType::graphics) &&
        !state.recovered(std::numeric_limits<SubmissionSerial>::max(), QueueType::graphics),
        "recovery requires a later non-sentinel marker on the same queue");
    check(!state.observe_completion(100) && state.active(),
        "completion alone cannot retire an unconfirmed signal");
    check(state.recovered(11, QueueType::graphics) && state.active() && state.marker_confirmed(),
        "confirmed marker must retain quarantine until completion");
    check(!state.recovered(12, QueueType::graphics) && !state.observe_completion(10) &&
        !state.observe_completion(std::numeric_limits<SubmissionSerial>::max()) && state.active(),
        "early or device-removed polling must retain quarantine");
    check(state.observe_completion(11) && !state.active(), "completed recovery must clear quarantine");
    check(state.arm(12, QueueType::copy, 0), "normal execution must arm");
    state.original_signal_confirmed();
    check(!state.active(), "original successful signal clears provisional guard");
    return ok ? 0 : 1;
}
