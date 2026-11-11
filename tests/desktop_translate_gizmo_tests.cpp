#include "studio_translate_gizmo.hpp"
#include "studio_view_settings.hpp"
#include <carto/ui/ui.hpp>

#include <limits>
#include <stdexcept>

namespace {
using namespace carto;
#define REQUIRE(expression) do { if (!(expression)) throw std::runtime_error(#expression); } while (false)

void near(core::Vec3d actual, core::Vec3d expected) {
    REQUIRE((actual - expected).length() < 1.0e-8);
}

template <typename Project>
std::vector<studio::ViewportTranslateHandle> handles_at(core::Vec3d world, Project project) {
    constexpr std::array<core::Vec3d, 3U> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    std::array<std::optional<core::Vec2d>, 3U> samples;
    for (std::size_t i = 0U; i < axes.size(); ++i) samples[i] = project(world + axes[i]);
    return studio::viewport_translate_handles(project(world), samples);
}

void named_views_admit_only_usable_axes_and_commit_y_through_history() {
    for (const auto kind : {studio::ViewportViewKind::front_xy, studio::ViewportViewKind::right_zy}) {
        application::ApplicationSession application;
        ui::UiController ui(application);
        REQUIRE(ui.dispatch(application::CreateBoxAction{"Move fixture", {1, 1, 1}}));
        auto snapshot = ui.snapshot();
        REQUIRE(snapshot.objects.size() == 1U);
        const auto object = snapshot.objects.front().object.id;
        const auto initial = snapshot.objects.front().world_transform;
        editor::EditorCamera camera;
        camera.set_projection(editor::EditorProjection::orthographic);
        REQUIRE(studio::apply_orthographic_view_orientation(kind, camera));
        REQUIRE(camera.frame(initial.translation, 5.0, 4.0 / 3.0));
        const auto project = [&camera](core::Vec3d world) {
            const auto result = camera.project_marker(world, {800, 600});
            REQUIRE(result && result.value().in_front);
            return result.value().screen_position;
        };
        const auto handles = handles_at(initial.translation, project);
        REQUIRE(handles.size() == 2U);
        const auto omitted = kind == studio::ViewportViewKind::front_xy
            ? editor::ViewportTranslateAxis::world_z : editor::ViewportTranslateAxis::world_x;
        for (const auto& handle : handles) REQUIRE(handle.axis != omitted);
        const auto y = std::find_if(handles.begin(), handles.end(), [](const auto& handle) {
            return handle.axis == editor::ViewportTranslateAxis::world_y;
        });
        REQUIRE(y != handles.end());
        const auto hit = studio::hit_viewport_translate_handle(handles, y->tip, 8.0);
        REQUIRE(hit && handles[*hit].axis == editor::ViewportTranslateAxis::world_y);
        REQUIRE(!studio::hit_viewport_translate_handle(handles, {10, 10}, 8.0));
        editor::ViewportTransformSession drag;
        REQUIRE(drag.begin_projected(object, initial, snapshot.project_revision,
            y->axis, y->tip, y->screen_axis_per_world));
        const auto pointer = y->tip + y->screen_axis_per_world * 1.25;
        const auto preview = drag.preview(pointer, snapshot.project_revision);
        REQUIRE(preview);
        near(preview.value().translation, initial.translation + core::Vec3d{0, 1.25, 0});
        near(ui.snapshot().objects.front().world_transform.translation, initial.translation);
        const auto completed = drag.complete(pointer, snapshot.project_revision);
        REQUIRE(completed && !drag.active());
        REQUIRE(ui.dispatch(application::SetObjectTransformAction{
            object, completed.value(), snapshot.project_revision}));
        near(ui.snapshot().objects.front().world_transform.translation, completed.value().translation);
        REQUIRE(!ui.dispatch(application::SetObjectTransformAction{
            object, initial, snapshot.project_revision}));
        REQUIRE(ui.dispatch(application::UndoAction{}));
        near(ui.snapshot().objects.front().world_transform.translation, initial.translation);
        REQUIRE(ui.dispatch(application::RedoAction{}));
        near(ui.snapshot().objects.front().world_transform.translation, completed.value().translation);
    }
}

void plan_and_perspective_handle_projection_remain_supported() {
    editor::WorkSurfaceCamera plan;
    REQUIRE(plan.frame({}, {10, 10}, {800, 600}));
    const auto project = [&plan](core::Vec3d world) {
        const auto result = plan.world_to_screen({world.x, world.z}, {}, {800, 600});
        REQUIRE(result);
        return result.value();
    };
    const auto handles = handles_at({2, 3, 1}, project);
    REQUIRE(handles.size() == 2U);
    REQUIRE(handles[0].axis == editor::ViewportTranslateAxis::world_x);
    REQUIRE(handles[1].axis == editor::ViewportTranslateAxis::world_z);
    editor::EditorCamera camera;
    REQUIRE(camera.set_orientation(0.6, -0.5));
    REQUIRE(camera.frame({}, 5.0, 4.0 / 3.0));
    const auto spatial = handles_at({}, [&camera](core::Vec3d world) {
        const auto result = camera.project_marker(world, {800, 600});
        REQUIRE(result && result.value().in_front);
        return result.value().screen_position;
    });
    REQUIRE(spatial.size() == 3U);
    for (const auto& handle : spatial) {
        const auto hit = studio::hit_viewport_translate_handle(spatial, handle.tip, 8.0);
        REQUIRE(hit && spatial[*hit].axis == handle.axis);
    }
}

void unavailable_axis_does_not_remove_other_handles() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::array<std::optional<core::Vec2d>, 3U> samples{
        core::Vec2d{50, 0}, std::nullopt, core::Vec2d{0, -50}};
    auto handles = studio::viewport_translate_handles({}, samples);
    REQUIRE(handles.size() == 2U);
    REQUIRE(studio::hit_viewport_translate_handle(handles, handles[1].tip, 8.0) == 1U);
    auto invalid = samples;
    invalid[0] = core::Vec2d{nan, 0};
    handles = studio::viewport_translate_handles({}, invalid);
    REQUIRE(handles.size() == 1U && handles.front().axis == editor::ViewportTranslateAxis::world_z);
    REQUIRE(studio::viewport_translate_handles({}, samples, 8.0).empty());
    REQUIRE(studio::viewport_translate_handles({nan, 0}, samples).empty());
    REQUIRE(!studio::hit_viewport_translate_handle(handles, {nan, 0}, 8.0));
    REQUIRE(!studio::hit_viewport_translate_handle(handles, {}, -1.0));
}
}

int main() {
    named_views_admit_only_usable_axes_and_commit_y_through_history();
    plan_and_perspective_handle_projection_remain_supported();
    unavailable_axis_does_not_remove_other_handles();
}
