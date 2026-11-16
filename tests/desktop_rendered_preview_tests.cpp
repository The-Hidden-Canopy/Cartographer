#include "../apps/desktop/studio_rendered_preview.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void return_to_editing_requires_a_presented_current_canvas() {
    using namespace carto::studio;
    PreviewReturnGate gate;
    const PreviewReturnFrame frame{0, 7, 10, 20, 800, 500, 0, 0, 1280, 720};
    check(frame.valid(), "initial project generation zero is valid");
    check(!gate.may_resume(false, false, frame), "unpresented canvas allowed editing");
    gate.observe(false, false, frame);
    check(!gate.may_resume(false, false, frame), "failed present allowed editing");
    gate.observe(true, true, frame);
    check(!gate.may_resume(false, false, frame), "preview frame allowed editing");
    gate.observe(true, false, frame);
    check(gate.may_resume(false, false, frame), "presented current canvas did not allow resume");
    check(!gate.may_resume(true, false, frame), "enabled preview allowed editing");
    check(!gate.may_resume(false, true, frame), "active gesture allowed editing");
    auto changed = frame;
    ++changed.revision;
    check(!gate.may_resume(false, false, changed), "stale revision allowed editing");
    changed = frame; ++changed.generation;
    check(!gate.may_resume(false, false, changed), "another project allowed editing");
    changed = frame; changed.width += 1;
    check(!gate.may_resume(false, false, changed), "resized canvas allowed editing");
    changed = frame; changed.window_x += 1;
    check(!gate.may_resume(false, false, changed), "moved window allowed editing");
    changed = frame; changed.height = 0;
    check(!gate.may_resume(false, false, changed), "empty canvas allowed editing");
    changed = frame; changed.x = std::numeric_limits<float>::quiet_NaN();
    check(!gate.may_resume(false, false, changed), "nonfinite canvas allowed editing");
    gate.observe(true, false, std::nullopt);
    check(!gate.may_resume(false, false, frame), "hidden canvas retained receipt");
    gate.observe(true, false, frame);
    gate.observe(false, false, frame);
    check(!gate.may_resume(false, false, frame), "failed next present retained receipt");
    gate.observe(true, false, frame);
    gate.clear();
    check(!gate.may_resume(false, false, frame), "cleared receipt allowed editing");

    gate.observe(true, false, frame);
    check(gate.request_resume(false, false, frame), "resume request rejected current canvas");
    check(gate.pending(), "resume request did not remain pending");
    check(!gate.finish_resume(true, false, false, frame), "resume skipped final canvas presentation");
    gate.observe(true, false, frame);
    check(!gate.finish_resume(false, false, false, frame), "old button layout completed return");
    gate.observe(false, false, frame);
    check(gate.pending(), "deferred presentation discarded the requested return");
    check(!gate.finish_resume(true, false, false, frame), "failed final presentation completed return");
    gate.observe(true, false, frame);
    check(!gate.finish_resume(true, false, true, frame), "active gesture completed return");
    check(gate.finish_resume(true, false, false, frame), "presented final layout did not complete return");
    check(!gate.pending() && !gate.may_resume(false, false, frame), "completed return retained its receipt");
    gate.observe(true, false, frame);
    check(gate.request_resume(false, false, frame), "second resume request failed");
    changed = frame; ++changed.revision;
    gate.observe(true, false, changed);
    check(!gate.pending(), "project mutation retained prior resume request");
    check(!gate.finish_resume(true, false, false, changed), "new revision completed old resume request");
    check(gate.may_resume(false, false, changed), "current canvas could not be acknowledged anew");
    gate.cancel();
    gate.request_mode_exit(frame.generation, frame.revision);
    check(gate.pending() && !gate.finish_resume(true, false, false, frame),
        "mode exit unlocked before a live canvas was presented");
    gate.observe(false, false, frame);
    check(gate.pending(), "deferred mode exit lost its request");
    gate.observe(true, false, frame);
    check(gate.finish_resume(true, false, false, frame),
        "presented mode exit did not automatically return to editing");
}

void status_row_does_not_change_canvas_height() {
    ImGuiContext* context = ImGui::CreateContext();
    check(context != nullptr, "failed to create ImGui context");
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0F / 60.0F;
    io.DisplaySize = {800, 600};
    check(io.Fonts->Build(), "failed to build font atlas");
    float expected = -1;
    for (const std::string& status : {std::string{}, std::string{"Preparing rendered viewport."},
             std::string{"Rendering viewport."}, std::string{"GPU error\nsecond line"},
             std::string(5000, 'x')}) {
        for (const bool displayed : {false, true}) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({300, 300}, ImGuiCond_Always);
            ImGui::Begin("status-row-test", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const float before = ImGui::GetCursorPosY();
            carto::studio::draw_rendered_preview_status(status, displayed);
            const float height = ImGui::GetCursorPosY() - before;
            if (expected < 0) expected = height;
            check(std::abs(height - expected) < 0.01F, "status changed viewport layout height");
            ImGui::End();
            ImGui::Render();
        }
    }
    ImGui::DestroyContext(context);
}
} // namespace

int main() {
    try {
        return_to_editing_requires_a_presented_current_canvas();
        status_row_does_not_change_canvas_height();
        std::cout << "rendered preview layout and return gate passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
