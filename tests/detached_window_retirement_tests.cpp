#include "../apps/desktop/carto_detached_window_retirement.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Bundle {
    explicit Bundle(int& releases) : releases(&releases) {}
    ~Bundle() { ++*releases; }
    int* releases;
};
using Window = carto::studio::DetachedWindowRetirement<Bundle>;

void deferred_close() {
    int released = 0;
    Window window(std::make_unique<Bundle>(released));
    check(window.begin_frame(), "frame begins");
    check(window.request_close(), "close requested");
    check(!window.needs_signal() && !window.begin_frame(), "in-flight frame blocks retirement and new frames");
    check(window.finish_frame(Window::FrameResult::presented), "frame present recorded");
    check(window.needs_signal() && window.signal_queued(7), "queue signal follows final draw");
    for (unsigned i = 0; i < 1000; ++i) {
        window.observe_fence(6);
        window.observe_present(Window::PresentObservation::pending);
        check(!window.ready() && !window.take_retired() && released == 0, "timeouts retain resources");
    }
    window.observe_fence(7);
    check(!window.ready() && !window.take_retired(), "GPU fence alone does not prove presentation retirement");
    window.observe_present(Window::PresentObservation::retired);
    check(window.ready(), "independent present retirement completes close");
    auto retired = window.take_retired();
    check(retired && !window.resources() && released == 0, "resource ownership transfers intact");
    retired.reset();
    check(released == 1, "explicit owner releases only after retirement");
}
void resize_sequence() {
    int released = 0;
    Window window(std::make_unique<Bundle>(released));
    check(window.begin_frame() && window.finish_frame(Window::FrameResult::submitted_without_present), "submitted frame");
    check(window.request_resize(640, 480) && window.request_resize(800, 600), "resize coalesces latest size");
    check(window.signal_queued(1), "resize signal");
    Window::Extent size;
    check(!window.begin_resize(size) && !window.begin_frame(), "pending resize gates release and drawing");
    window.observe_fence(1);
    check(window.begin_resize(size) && size.width == 800 && size.height == 600, "resize receives latest extent after fence");
    check(!window.request_close() && !window.begin_frame(), "resize mutation is exclusive");
    check(window.finish_resize(true) && window.begin_frame(), "successful resize reopens frame gate");
    check(window.finish_frame(Window::FrameResult::not_submitted), "empty frame completes");
    check(window.request_close() && !window.signal_queued(1) && window.signal_queued(2), "fence values strictly increase");
    window.observe_fence(2);
    auto retired = window.take_retired();
    check(retired && released == 0, "resized bundle remains owned");
    retired.reset();
    check(released == 1, "resized bundle released after close retirement");
}
void uncertain_paths() {
    int released = 0;
    {
        Window window(std::make_unique<Bundle>(released));
        check(window.request_close() && window.needs_signal(), "uncertain signal setup");
        window.signal_uncertain();
        check(window.quarantined() && !window.take_retired(), "signal failure quarantines ownership");
    }
    check(released == 0, "destructor retains uncertain resources");
    {
        Window window(std::make_unique<Bundle>(released));
        check(window.request_close() && window.signal_queued(3), "device removal setup");
        window.observe_fence(std::numeric_limits<std::uint64_t>::max());
        check(window.quarantined() && !window.ready(), "device-removed sentinel is not completion");
    }
    check(released == 0, "device removal does not release resources");
    {
        Window window(std::make_unique<Bundle>(released));
        check(window.request_close() && window.signal_queued(9), "regressing fence setup");
        window.observe_fence(7);
        window.observe_fence(6);
        check(window.quarantined() && !window.take_retired(), "regressing fence observation retains ownership");
    }
    check(released == 0, "regressing fence cannot release resources");
    {
        Window window(std::make_unique<Bundle>(released));
        check(window.begin_frame() && window.finish_frame(Window::FrameResult::presented), "present uncertainty setup");
        check(window.request_close() && window.signal_queued(4), "present retirement signal");
        window.observe_fence(4);
        window.observe_present(Window::PresentObservation::uncertain);
        check(window.quarantined() && !window.take_retired(), "uncertain presentation cannot prove retirement");
    }
    check(released == 0, "uncertain presentation retains resources");
    {
        Window window(std::make_unique<Bundle>(released));
        check(window.begin_frame(), "uncertain frame starts");
        check(!window.finish_frame(Window::FrameResult::uncertain) && window.quarantined(), "uncertain submission quarantined");
    }
    check(released == 0, "uncertain frame does not release resources");
    {
        Window window(std::make_unique<Bundle>(released));
        check(window.request_resize(20, 20) && window.signal_queued(4), "failed resize setup");
        window.observe_fence(4);
        Window::Extent size;
        check(window.begin_resize(size) && !window.finish_resize(false) && window.quarantined(), "failed resize quarantines bundle");
    }
    check(released == 0, "failed resize cannot drop bundle");
}
} // namespace

int main() {
    try {
        deferred_close();
        resize_sequence();
        uncertain_paths();
        std::cout << "detached window retirement policy passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
