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

} // namespace

int main() {
    try {
        hit_validation_rejects_ambiguous_or_unbounded_identity();
        cpu_pick_is_revision_bound_and_deterministic();
        box_selection_deduplicates_objects_and_preserves_components();
        interaction_session_captures_cancels_and_rejects_revision_changes();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "viewport interaction tests passed\n";
    return 0;
}
