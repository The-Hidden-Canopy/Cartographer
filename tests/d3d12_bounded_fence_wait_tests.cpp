#include "../libs/d3d12/include/carto/d3d12/bounded_fence_wait.hpp"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace carto::d3d12::detail;
using Clock = std::chrono::steady_clock;

int main() {
    bool ok = true;
    const auto check = [&](bool value, const char* message) {
        if (!value) { std::cerr << message << '\n'; ok = false; }
    };
    auto time = Clock::time_point{};
    std::uint64_t completed = 3;
    unsigned registrations = 0, waits = 0;
    auto observe = [&] { return completed; };
    auto registration = [&](std::uint64_t value) { ++registrations; return value == 7; };
    auto now = [&] { return time; };
    auto stale_then_complete = [&](std::uint32_t milliseconds) {
        check(milliseconds <= 100, "wait never exceeds remaining budget");
        ++waits;
        time += std::chrono::milliseconds(10);
        if (waits == 2) completed = 7;
        return FenceEventOutcome::signaled;
    };
    const auto done = bounded_fence_wait(7, time + std::chrono::milliseconds(100),
        observe, registration, stale_then_complete, now);
    check(done == FenceWaitOutcome::complete && waits == 2 && registrations == 1,
        "stale event does not prove completion or trigger duplicate registration");

    completed = 3; waits = 0; registrations = 0; time = Clock::time_point{};
    auto pending = [&](std::uint32_t milliseconds) {
        ++waits;
        time += std::chrono::milliseconds(milliseconds);
        return FenceEventOutcome::timeout;
    };
    const auto timed_out = bounded_fence_wait(7, time + std::chrono::milliseconds(25),
        observe, registration, pending, now);
    check(timed_out == FenceWaitOutcome::timeout && completed == 3 && waits == 1 && registrations == 1,
        "timeout preserves pending fence ownership");

    completed = std::numeric_limits<std::uint64_t>::max();
    const auto removed = bounded_fence_wait(7, time + std::chrono::milliseconds(25),
        observe, registration, pending, now);
    check(removed == FenceWaitOutcome::invalid_completion && registrations == 1,
        "device-removed sentinel never proves completion");
    completed = 3;
    const auto failure = bounded_fence_wait(7, time + std::chrono::milliseconds(25),
        observe, [](std::uint64_t) { return false; }, pending, now);
    check(failure == FenceWaitOutcome::registration_failed, "failed event registration fails closed");
    const auto wait_failure = bounded_fence_wait(7, time + std::chrono::milliseconds(25),
        observe, registration, [](std::uint32_t) { return FenceEventOutcome::failed; }, now);
    check(wait_failure == FenceWaitOutcome::wait_failed, "failed event wait fails closed");
#ifdef _WIN32
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event != nullptr, "native fence-event fixture is available");
    if (event) {
        std::atomic<std::uint64_t> native_completed{3};
        SetEvent(event);
        std::thread completion([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            native_completed.store(7);
            SetEvent(event);
        });
        const auto native_result = bounded_fence_wait(7, Clock::now() + std::chrono::seconds(1),
            [&] { return native_completed.load(); }, [](std::uint64_t) { return true; },
            [&](std::uint32_t milliseconds) {
                const auto status = WaitForSingleObject(event, milliseconds);
                return status == WAIT_OBJECT_0 ? FenceEventOutcome::signaled :
                    status == WAIT_TIMEOUT ? FenceEventOutcome::timeout : FenceEventOutcome::failed;
            }, [] { return Clock::now(); });
        completion.join();
        CloseHandle(event);
        check(native_result == FenceWaitOutcome::complete,
            "native auto-reset event ignores an old signal until the fence advances");
    }
#endif
    return ok ? 0 : 1;
}
