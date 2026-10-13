#include <carto/editor/viewport_interaction.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace carto::editor {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

bool valid_target(ViewportPickTarget target) noexcept {
    return target == ViewportPickTarget::nearest_visible ||
        target == ViewportPickTarget::object || target == ViewportPickTarget::component;
}

bool finite_depth_window(const ViewportPickSettings& settings) noexcept {
    if (settings.minimum_depth.has_value() &&
        (!std::isfinite(*settings.minimum_depth) || *settings.minimum_depth < 0.0)) {
        return false;
    }
    if (settings.maximum_depth.has_value() &&
        (!std::isfinite(*settings.maximum_depth) || *settings.maximum_depth < 0.0)) {
        return false;
    }
    return !settings.minimum_depth.has_value() || !settings.maximum_depth.has_value() ||
        *settings.minimum_depth <= *settings.maximum_depth;
}

bool has_component(const ViewportHit& hit) noexcept {
    return hit.vertex.has_value() || hit.edge.has_value() || hit.face.has_value();
}

bool in_depth_window(const ViewportHit& hit, const ViewportPickSettings& settings) noexcept {
    return (!settings.minimum_depth.has_value() || hit.depth >= *settings.minimum_depth) &&
        (!settings.maximum_depth.has_value() || hit.depth <= *settings.maximum_depth);
}

bool eligible(
    const ViewportHit& hit,
    core::Revision source_revision,
    const ViewportPickSettings& settings) noexcept {
    return hit.source_revision == source_revision &&
        (settings.include_backfaces || hit.front_facing) &&
        (settings.select_through || !hit.occluded) &&
        in_depth_window(hit, settings);
}

bool inside(const core::Vec2d point, const ViewportSelectionBox& box) noexcept {
    const double minimum_x = std::min(box.first.x, box.second.x);
    const double maximum_x = std::max(box.first.x, box.second.x);
    const double minimum_y = std::min(box.first.y, box.second.y);
    const double maximum_y = std::max(box.first.y, box.second.y);
    return point.x >= minimum_x && point.x <= maximum_x &&
        point.y >= minimum_y && point.y <= maximum_y;
}

bool segment_intersects_box(
    const core::Vec2d first,
    const core::Vec2d second,
    const ViewportSelectionBox& box) noexcept {
    if (inside(first, box) || inside(second, box)) return true;
    const double minimum_x = std::min(box.first.x, box.second.x);
    const double maximum_x = std::max(box.first.x, box.second.x);
    const double minimum_y = std::min(box.first.y, box.second.y);
    const double maximum_y = std::max(box.first.y, box.second.y);
    const double left = std::min(first.x, second.x);
    const double right = std::max(first.x, second.x);
    const double top = std::min(first.y, second.y);
    const double bottom = std::max(first.y, second.y);
    return right >= minimum_x && left <= maximum_x &&
        bottom >= minimum_y && top <= maximum_y;
}

bool sample_precedes(const ViewportHit& left, const ViewportHit& right) noexcept {
    return std::tie(
               left.depth,
               left.screen_position.y,
               left.screen_position.x,
               left.world_position.x,
               left.world_position.y,
               left.world_position.z,
               left.world_normal.x,
               left.world_normal.y,
               left.world_normal.z) <
        std::tie(
            right.depth,
            right.screen_position.y,
            right.screen_position.x,
            right.world_position.x,
            right.world_position.y,
            right.world_position.z,
            right.world_normal.x,
            right.world_normal.y,
            right.world_normal.z);
}

} // namespace

core::Result<void> ViewportHit::validate() const {
    const int component_count = static_cast<int>(vertex.has_value()) +
        static_cast<int>(edge.has_value()) + static_cast<int>(face.has_value());
    if (!object || component_count > 1 || !screen_position.finite() ||
        !world_position.finite() || !world_normal.finite() ||
        !std::isfinite(depth) || depth < 0.0 ||
        world_normal.length_squared() <= 1.0e-18) {
        return core::Result<void>::failure(invalid("viewport hit identity or geometry is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ViewportPickSettings::validate() const {
    if (!valid_target(target) || !finite_depth_window(*this)) {
        return core::Result<void>::failure(invalid("viewport pick settings are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<std::optional<ViewportHit>> pick_viewport_hit(
    const std::vector<ViewportHit>& hits,
    core::Revision source_revision,
    const ViewportPickSettings& settings) {
    if (auto result = settings.validate(); !result) {
        return core::Result<std::optional<ViewportHit>>::failure(result.error());
    }
    std::optional<ViewportHit> best;
    for (const auto& hit : hits) {
        if (auto result = hit.validate(); !result) {
            return core::Result<std::optional<ViewportHit>>::failure(result.error());
        }
        if (!eligible(hit, source_revision, settings)) continue;
        if (settings.target == ViewportPickTarget::component && !has_component(hit)) continue;
        if (settings.target == ViewportPickTarget::object && has_component(hit)) continue;
        if (!best.has_value() || hit.depth < best->depth ||
            (hit.depth == best->depth && hit.object < best->object)) {
            best = hit;
        }
    }
    return core::Result<std::optional<ViewportHit>>::success(std::move(best));
}

core::Result<void> ViewportSelectionBox::validate() const {
    if (!first.finite() || !second.finite()) {
        return core::Result<void>::failure(invalid("viewport selection box coordinates are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<std::vector<ViewportHit>> select_viewport_box(
    const std::vector<ViewportHit>& hits,
    core::Revision source_revision,
    const ViewportSelectionBox& box,
    const ViewportPickSettings& settings) {
    if (auto result = settings.validate(); !result) {
        return core::Result<std::vector<ViewportHit>>::failure(result.error());
    }
    if (auto result = box.validate(); !result) {
        return core::Result<std::vector<ViewportHit>>::failure(result.error());
    }
    std::vector<ViewportHit> selected;
    std::map<scene::ObjectId, ViewportHit> objects;
    using ComponentKey = std::tuple<std::uint64_t, std::uint8_t, std::uint64_t>;
    std::map<ComponentKey, ViewportHit> components;
    for (const auto& hit : hits) {
        if (auto result = hit.validate(); !result) {
            return core::Result<std::vector<ViewportHit>>::failure(result.error());
        }
        if (!eligible(hit, source_revision, settings)) continue;
        if (settings.target == ViewportPickTarget::component && !has_component(hit)) continue;
        if (settings.target == ViewportPickTarget::object && has_component(hit)) continue;
        const bool matches = box.crossing
            ? segment_intersects_box(hit.screen_position, hit.screen_position, box)
            : inside(hit.screen_position, box);
        if (!matches) continue;

        if (settings.target == ViewportPickTarget::nearest_visible ||
            settings.target == ViewportPickTarget::object || !has_component(hit)) {
            const auto existing = objects.find(hit.object);
            if (existing == objects.end() || sample_precedes(hit, existing->second)) {
                objects.insert_or_assign(hit.object, hit);
            }
            continue;
        }
        const std::uint64_t component_key = hit.vertex.has_value()
            ? hit.vertex->value
            : hit.edge.has_value() ? hit.edge->value : hit.face->value;
        const std::uint8_t kind_key = hit.vertex.has_value()
            ? 1U : hit.edge.has_value() ? 2U : 3U;
        const ComponentKey key{hit.object.value, kind_key, component_key};
        const auto existing = components.find(key);
        if (existing == components.end() || sample_precedes(hit, existing->second)) {
            components.insert_or_assign(key, hit);
        }
    }

    selected.reserve(objects.size() + components.size());
    for (const auto& [object, hit] : objects) {
        (void)object;
        selected.push_back(hit);
    }
    for (const auto& [component, hit] : components) {
        (void)component;
        selected.push_back(hit);
    }
    std::sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
        if (left.screen_position.y != right.screen_position.y) {
            return left.screen_position.y < right.screen_position.y;
        }
        if (left.screen_position.x != right.screen_position.x) {
            return left.screen_position.x < right.screen_position.x;
        }
        if (left.depth != right.depth) return left.depth < right.depth;
        if (left.object != right.object) return left.object < right.object;
        const auto component_key = [](const auto& hit) {
            const std::uint8_t kind = hit.vertex.has_value()
                ? 1U : hit.edge.has_value() ? 2U : hit.face.has_value() ? 3U : 0U;
            const std::uint64_t value = hit.vertex.has_value()
                ? hit.vertex->value
                : hit.edge.has_value() ? hit.edge->value
                : hit.face.has_value() ? hit.face->value : 0U;
            return std::tuple<std::uint8_t, std::uint64_t>{kind, value};
        };
        return component_key(left) < component_key(right);
    });
    return core::Result<std::vector<ViewportHit>>::success(std::move(selected));
}

core::Result<void> ViewportPointerSample::validate() const {
    if (!screen_position.finite()) {
        return core::Result<void>::failure(invalid("viewport pointer coordinates are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ViewportInteractionSnapshot::validate() const {
    if (!start.validate() || !current.validate() || start.source_revision != source_revision ||
        current.source_revision != source_revision) {
        return core::Result<void>::failure(invalid("viewport interaction snapshot is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> ViewportInteractionSession::begin(
    const ViewportPointerSample& sample) {
    if (auto result = sample.validate(); !result) return result;
    if (sample.source_revision != state_.source_revision && active()) {
        return core::Result<void>::failure(
            stale("viewport interaction cannot be replaced during an active capture"));
    }
    if (active()) {
        return core::Result<void>::failure(
            invalid_state("viewport interaction already has pointer capture"));
    }
    state_ = ViewportInteractionSnapshot{
        ViewportInteractionPhase::captured, sample.source_revision, sample, sample};
    return core::Result<void>::success();
}

core::Result<void> ViewportInteractionSession::require_revision(
    const ViewportPointerSample& sample) const {
    if (sample.source_revision != state_.source_revision) {
        return core::Result<void>::failure(
            stale("viewport interaction source revision changed during capture"));
    }
    return core::Result<void>::success();
}

core::Result<void> ViewportInteractionSession::require_active(
    const ViewportPointerSample& sample) const {
    if (auto result = sample.validate(); !result) return result;
    if (!active()) {
        return core::Result<void>::failure(
            invalid_state("viewport interaction has no active pointer capture"));
    }
    return require_revision(sample);
}

core::Result<void> ViewportInteractionSession::update(
    const ViewportPointerSample& sample) {
    if (auto result = require_active(sample); !result) return result;
    state_.current = sample;
    state_.phase = state_.current.screen_position.x == state_.start.screen_position.x &&
            state_.current.screen_position.y == state_.start.screen_position.y
        ? ViewportInteractionPhase::captured
        : ViewportInteractionPhase::dragging;
    return core::Result<void>::success();
}

core::Result<ViewportInteractionSnapshot> ViewportInteractionSession::complete(
    const ViewportPointerSample& sample) {
    if (auto result = require_active(sample); !result) {
        return core::Result<ViewportInteractionSnapshot>::failure(result.error());
    }
    state_.current = sample;
    state_.phase = ViewportInteractionPhase::completed;
    return core::Result<ViewportInteractionSnapshot>::success(state_);
}

core::Result<void> ViewportInteractionSession::cancel() {
    if (!active()) {
        return core::Result<void>::failure(
            invalid_state("viewport interaction has no active pointer capture"));
    }
    state_.phase = ViewportInteractionPhase::cancelled;
    return core::Result<void>::success();
}

} // namespace carto::editor
