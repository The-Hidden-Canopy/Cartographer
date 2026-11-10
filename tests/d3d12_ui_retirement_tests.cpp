#include "../apps/desktop/d3d12_ui_retirement.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {
struct Release {
    int* count = nullptr;
    void operator()(int* value) const noexcept { ++*count; delete value; }
};
using Resource = std::unique_ptr<int, Release>;
using Timeline = carto::desktop::D3D12UiRetirement<Resource, 2>;

std::optional<Resource> make_resource(int& count, int value) {
    return std::optional<Resource>(std::in_place, new int(value), Release{&count});
}
bool expect(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}
} // namespace

int main() {
    bool ok = true;
    int released = 0;
    Timeline timeline;
    auto first = make_resource(released, 1);
    auto second = make_resource(released, 2);
    auto third = make_resource(released, 3);

    ok &= expect(timeline.begin_frame(), "frame must open");
    auto first_slot = timeline.reserve();
    ok &= expect(bool(first_slot) && first_slot.commit(first) && !first,
        "reserved retirement must transfer ownership");
    auto second_slot = timeline.reserve();
    ok &= expect(bool(second_slot) && second_slot.commit(second) && !second,
        "second retirement must fit");
    auto full = timeline.reserve();
    ok &= expect(!full && third && **third == 3 && timeline.outstanding() == 2,
        "capacity failure must preserve current ownership");
    ok &= expect(!timeline.seal_after_platform_draws(1),
        "cannot seal while draw data may remain unsubmitted");
    ok &= expect(timeline.observe_completion(0) && released == 0,
        "unsealed records must survive polling");
    ok &= expect(!timeline.begin_shutdown(), "open frame blocks shutdown");

    // A skipped or abandoned frame discards unsubmitted draw data, but does
    // not establish a completion fence. An uncertain signal is not sealed.
    ok &= expect(timeline.close_frame() && timeline.begin_shutdown(),
        "closed frame may begin shutdown");
    ok &= expect(!timeline.shutdown_ready() && released == 0,
        "abandonment and uncertain signal retain ownership");
    ok &= expect(!timeline.reserve(), "shutdown blocks new reservations");
    ok &= expect(timeline.seal_after_platform_draws(7),
        "a later confirmed post-draw marker may seal retained records");
    ok &= expect(!timeline.seal_after_platform_draws(7) &&
        !timeline.seal_after_platform_draws(std::numeric_limits<std::uint64_t>::max()),
        "marker must increase and exclude device-removed sentinel");
    ok &= expect(timeline.observe_completion(6) && released == 0,
        "incomplete marker must not release resources");
    ok &= expect(!timeline.observe_completion(5) &&
        !timeline.observe_completion(8) &&
        !timeline.observe_completion(std::numeric_limits<std::uint64_t>::max()) && released == 0,
        "invalid or regressing polling must not release resources");
    ok &= expect(timeline.observe_completion(7) && released == 2 && timeline.shutdown_ready(),
        "only observed completion releases sealed records");
    third.reset();

    int later_released = 0;
    Timeline later;
    auto old = make_resource(later_released, 4);
    auto reserved = later.reserve();
    ok &= expect(bool(reserved) && later.begin_frame() && later.close_frame() &&
        later.seal_after_platform_draws(1), "reservation can outlive a frame");
    ok &= expect(reserved.commit(old) && later.observe_completion(1) && later_released == 0,
        "a retirement committed after a signal must not inherit that signal");
    ok &= expect(later.begin_frame() && later.close_frame() &&
        later.seal_after_platform_draws(2) && later.observe_completion(2) && later_released == 1,
        "the next confirmed marker retires the later record");
    auto held = later.reserve();
    ok &= expect(bool(held) && later.begin_shutdown() && !later.shutdown_ready(),
        "an outstanding reservation blocks shutdown");
    held.cancel();
    ok &= expect(later.shutdown_ready(), "cancelling an empty reservation releases capacity");
    return ok ? 0 : 1;
}
