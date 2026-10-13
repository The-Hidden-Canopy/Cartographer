#include <carto/editor/viewport_interaction.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

class TestFailure final : public std::runtime_error {
public:
    explicit TestFailure(const char* message) : std::runtime_error(message) {}
};

#define REQUIRE(condition) \
    do { \
        if (!(condition)) throw TestFailure(#condition); \
    } while (false)

carto::editor::ViewportHit object_hit(
    std::uint64_t object,
    double x,
    double y,
    double depth,
    carto::core::Revision revision) {
    carto::editor::ViewportHit hit;
    hit.object = carto::scene::ObjectId{object};
    hit.screen_position = {x, y};
    hit.world_position = {x, y, depth};
    hit.depth = depth;
    hit.source_revision = revision;
    return hit;
}

carto::editor::ViewportHit vertex_hit(
    std::uint64_t object,
    std::uint64_t vertex,
    double x,
    double y,
    double depth,
    carto::core::Revision revision) {
    auto hit = object_hit(object, x, y, depth, revision);
    hit.vertex = carto::geometry::VertexId{vertex};
    return hit;
}

void hit_validation_rejects_ambiguous_or_unbounded_identity() {
    using namespace carto;
    auto hit = object_hit(1U, 10.0, 10.0, 1.0, core::Revision{4U});
    REQUIRE(hit.validate());

    hit.vertex = geometry::VertexId{2U};
    hit.edge = geometry::EdgeId{3U};
    REQUIRE(!hit.validate());

    hit.edge = {};
    hit.depth = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(!hit.validate());

    hit.depth = 1.0;
    hit.world_normal = {};
    REQUIRE(!hit.validate());
}

void cpu_pick_is_revision_bound_and_deterministic() {
    using namespace carto;
    const core::Revision revision{4U};
    auto near = object_hit(2U, 20.0, 10.0, 0.25, revision);
    auto far = object_hit(1U, 20.0, 10.0, 0.75, revision);
    auto stale = object_hit(3U, 20.0, 10.0, 0.01, core::Revision{3U});
    stale.front_facing = false;
    const std::vector<editor::ViewportHit> hits{far, stale, near};

    const auto picked = editor::pick_viewport_hit(hits, revision);
    REQUIRE(picked && picked.value().has_value());
    REQUIRE(picked.value()->object == scene::ObjectId{2U});

    editor::ViewportPickSettings include_backfaces;
    include_backfaces.include_backfaces = true;
    const auto backface_only = editor::pick_viewport_hit(
        {stale}, revision, include_backfaces);
    REQUIRE(backface_only && !backface_only.value().has_value());

    const auto stale_only = editor::pick_viewport_hit(
        {stale}, core::Revision{3U}, include_backfaces);
    REQUIRE(stale_only && stale_only.value().has_value());

    near.occluded = true;
    const auto visible_only = editor::pick_viewport_hit({near}, revision);
    REQUIRE(visible_only && !visible_only.value().has_value());
    include_backfaces.select_through = true;
    const auto through = editor::pick_viewport_hit({near}, revision, include_backfaces);
    REQUIRE(through && through.value().has_value());

    include_backfaces.minimum_depth = 2.0;
    const auto filtered = editor::pick_viewport_hit(hits, revision, include_backfaces);
    REQUIRE(filtered && !filtered.value().has_value());
}

void box_selection_deduplicates_objects_and_preserves_components() {
    using namespace carto;
    const core::Revision revision{8U};
    const std::vector<editor::ViewportHit> hits{
        object_hit(2U, 20.0, 20.0, 0.5, revision),
        object_hit(1U, 10.0, 10.0, 0.4, revision),
        object_hit(2U, 21.0, 21.0, 0.3, revision),
        vertex_hit(1U, 12U, 12.0, 12.0, 0.2, revision),
        vertex_hit(1U, 13U, 18.0, 18.0, 0.1, revision),
    };
    const editor::ViewportSelectionBox box{{5.0, 5.0}, {25.0, 25.0}, false};

    editor::ViewportPickSettings objects;
    objects.target = editor::ViewportPickTarget::object;
    const auto selected_objects = editor::select_viewport_box(hits, revision, box, objects);
    REQUIRE(selected_objects && selected_objects.value().size() == 2U);
    REQUIRE(selected_objects.value()[0].object == scene::ObjectId{1U});
    REQUIRE(selected_objects.value()[1].object == scene::ObjectId{2U});
    REQUIRE(selected_objects.value()[1].depth == 0.3);

    const auto reversed_objects = editor::select_viewport_box(
        {hits[4], hits[3], hits[2], hits[1], hits[0]}, revision, box, objects);
    REQUIRE(reversed_objects && reversed_objects.value().size() == 2U);
    REQUIRE(reversed_objects.value()[0].object == scene::ObjectId{1U});
    REQUIRE(reversed_objects.value()[1].object == scene::ObjectId{2U});
    REQUIRE(reversed_objects.value()[1].depth == 0.3);

    editor::ViewportPickSettings components;
    components.target = editor::ViewportPickTarget::component;
    const auto selected_components = editor::select_viewport_box(
        hits, revision, box, components);
    REQUIRE(selected_components && selected_components.value().size() == 2U);
    REQUIRE(selected_components.value()[0].vertex == geometry::VertexId{12U});
    REQUIRE(selected_components.value()[1].vertex == geometry::VertexId{13U});

    const auto reversed_components = editor::select_viewport_box(
        {hits[4], hits[3], hits[2], hits[1], hits[0]}, revision, box, components);
    REQUIRE(reversed_components && reversed_components.value().size() == 2U);
    REQUIRE(reversed_components.value()[0].vertex == geometry::VertexId{12U});
    REQUIRE(reversed_components.value()[1].vertex == geometry::VertexId{13U});

    auto invalid_box = box;
    invalid_box.first.x = std::numeric_limits<double>::infinity();
    REQUIRE(!editor::select_viewport_box(hits, revision, invalid_box, objects));
}

void interaction_session_captures_cancels_and_rejects_revision_changes() {
    using namespace carto;
    editor::ViewportInteractionSession session;
    const editor::ViewportPointerSample start{{10.0, 20.0}, 0U, true, core::Revision{5U}};
    const editor::ViewportPointerSample moved{{22.0, 30.0}, 1U, true, core::Revision{5U}};
    REQUIRE(session.begin(start));
    REQUIRE(session.phase() == editor::ViewportInteractionPhase::captured);
    REQUIRE(session.update(moved));
    REQUIRE(session.phase() == editor::ViewportInteractionPhase::dragging);
    REQUIRE(session.snapshot().start.screen_position.x == 10.0);

    const auto stale = session.update(
        {{25.0, 35.0}, 1U, true, core::Revision{6U}});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    REQUIRE(session.active());

    const auto completed = session.complete(moved);
    REQUIRE(completed && completed.value().phase == editor::ViewportInteractionPhase::completed);
    REQUIRE(!session.active());
    REQUIRE(!session.cancel());

    editor::ViewportInteractionSession cancelled;
    REQUIRE(cancelled.begin(start));
    REQUIRE(cancelled.cancel());
    REQUIRE(cancelled.phase() == editor::ViewportInteractionPhase::cancelled);
}

void transform_preview_is_axis_constrained_and_revision_bound() {
    using namespace carto;
    editor::ViewportTransformSession session;
    core::Transform initial;
    initial.translation = {2.0, 3.0, 4.0};
    const core::Revision revision{17U};
    REQUIRE(session.begin(scene::ObjectId{5U}, initial, revision,
        editor::ViewportTranslateAxis::world_x, {100.0, 80.0}, 50.0));
    REQUIRE(session.object() == scene::ObjectId{5U});
    const auto preview = session.preview({125.0, 160.0}, revision);
    REQUIRE(preview && std::abs(preview.value().translation.x - 2.5) < 1e-12);
    REQUIRE(preview.value().translation.y == 3.0 && preview.value().translation.z == 4.0);

    const auto stale = session.preview({150.0, 80.0}, core::Revision{18U});
    REQUIRE(!stale && stale.error().code == core::ErrorCode::stale_data);
    REQUIRE(session.active());
    const auto completed = session.complete({150.0, 80.0}, revision);
    REQUIRE(completed && std::abs(completed.value().translation.x - 3.0) < 1e-12);
    REQUIRE(!session.active());

    REQUIRE(session.begin(scene::ObjectId{5U}, initial, revision,
        editor::ViewportTranslateAxis::world_z, {100.0, 80.0}, 50.0));
    const auto z_preview = session.preview({100.0, 55.0}, revision);
    REQUIRE(z_preview && std::abs(z_preview.value().translation.z - 4.5) < 1e-12);
    REQUIRE(z_preview.value().translation.x == 2.0 && z_preview.value().translation.y == 3.0);
    REQUIRE(session.cancel());
    REQUIRE(!session.active());
    REQUIRE(!session.begin({}, initial, revision,
        editor::ViewportTranslateAxis::world_x, {}, 0.0));
}

void work_surface_camera_fits_projects_and_preserves_zoom_anchor() {
    using namespace carto;
    editor::WorkSurfaceCamera camera;
    const core::Vec2d origin{100.0, 50.0};
    const core::Vec2d extent{800.0, 400.0};
    REQUIRE(camera.frame({12.0, -4.0}, {16.0, 8.0}, extent));
    REQUIRE(camera.valid());
    REQUIRE(camera.target().x == 12.0 && camera.target().y == -4.0);
    REQUIRE(std::abs(camera.view_height() - 8.96) < 1e-12);

    const core::Vec2d world{15.0, -2.0};
    const auto screen = camera.world_to_screen(world, origin, extent);
    REQUIRE(screen);
    const auto round_trip = camera.screen_to_world(screen.value(), origin, extent);
    REQUIRE(round_trip);
    REQUIRE(std::abs(round_trip.value().x - world.x) < 1e-12);
    REQUIRE(std::abs(round_trip.value().y - world.y) < 1e-12);

    const core::Vec2d pointer{720.0, 150.0};
    const auto anchor_before = camera.screen_to_world(pointer, origin, extent);
    REQUIRE(anchor_before);
    REQUIRE(camera.zoom_at(3.0, pointer, origin, extent));
    const auto anchor_after = camera.screen_to_world(pointer, origin, extent);
    REQUIRE(anchor_after);
    REQUIRE(std::abs(anchor_after.value().x - anchor_before.value().x) < 1e-12);
    REQUIRE(std::abs(anchor_after.value().y - anchor_before.value().y) < 1e-12);
    REQUIRE(camera.view_height() < 8.96);

    const auto target_before_pan = camera.target();
    REQUIRE(camera.pan_pixels({20.0, -10.0}, extent.y));
    REQUIRE(camera.target().x < target_before_pan.x);
    REQUIRE(camera.target().y < target_before_pan.y);
}

void work_surface_camera_rejects_invalid_fit_and_pointer_inputs() {
    using namespace carto;
    editor::WorkSurfaceCamera camera;
    const auto original_target = camera.target();
    const double original_height = camera.view_height();
    REQUIRE(!camera.frame({}, {1.0, 1.0}, {0.0, 200.0}));
    REQUIRE(!camera.frame({}, {1.0, 1.0}, {200.0, 100.0}, 0.5));
    REQUIRE(!camera.frame({}, {1.0e12, 1.0e12}, {200.0, 100.0}));
    REQUIRE(camera.target().x == original_target.x &&
        camera.target().y == original_target.y &&
        camera.view_height() == original_height);
    REQUIRE(!camera.pan_pixels({std::numeric_limits<double>::infinity(), 0.0}, 100.0));
    REQUIRE(!camera.zoom_at(1.0, {500.0, 500.0}, {0.0, 0.0}, {100.0, 100.0}));
    REQUIRE(!camera.world_to_screen({}, {}, {}));
    REQUIRE(!camera.screen_to_world({}, {}, {}));
}

} // namespace

int main() {
    try {
        hit_validation_rejects_ambiguous_or_unbounded_identity();
        cpu_pick_is_revision_bound_and_deterministic();
        box_selection_deduplicates_objects_and_preserves_components();
        interaction_session_captures_cancels_and_rejects_revision_changes();
        transform_preview_is_axis_constrained_and_revision_bound();
        work_surface_camera_fits_projects_and_preserves_zoom_anchor();
        work_surface_camera_rejects_invalid_fit_and_pointer_inputs();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "viewport interaction tests passed\n";
    return 0;
}
