#include <carto/semantic/query.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace carto::semantic {
namespace {

auto binding_key(const world::AuthoringBinding& binding) {
    return std::make_tuple(
        static_cast<std::uint8_t>(binding.kind), binding.identity);
}

} // namespace

std::vector<world::AreaId> areas_with_type(
    const world::WorldModel& model, std::string_view semantic_type) {
    std::vector<world::AreaId> result;
    for (const auto& [identity, area] : model.areas()) {
        if (area.semantic_type == semantic_type) {
            result.push_back(identity);
        }
    }
    return result;
}

std::vector<world::AreaId> areas_with_tag(
    const world::WorldModel& model, std::string_view tag) {
    std::vector<world::AreaId> result;
    for (const auto& [identity, area] : model.areas()) {
        if (std::binary_search(area.tags.begin(), area.tags.end(), tag)) {
            result.push_back(identity);
        }
    }
    return result;
}

std::vector<const SemanticExtensionRecord*> bindings_for(
    const SemanticSidecar& sidecar, world::AuthoringBinding bound) {
    std::vector<const SemanticExtensionRecord*> result;
    const auto wanted_key = std::make_tuple(
        static_cast<std::uint8_t>(bound.kind), bound.identity);
    for (const auto& record : sidecar.records()) {
        const auto position = std::lower_bound(
            record.bound_identities.begin(), record.bound_identities.end(), wanted_key,
            [](const world::AuthoringBinding& binding, const auto& wanted) {
                return binding_key(binding) < wanted;
            });
        if (position != record.bound_identities.end() &&
            binding_key(*position) == wanted_key) {
            result.push_back(&record);
        }
    }
    std::sort(result.begin(), result.end(),
        [](const SemanticExtensionRecord* left, const SemanticExtensionRecord* right) {
            const auto left_key = std::make_tuple(
                left->schema.name_space, left->schema.name, left->payload_digest);
            const auto right_key = std::make_tuple(
                right->schema.name_space, right->schema.name, right->payload_digest);
            return left_key < right_key;
        });
    return result;
}

std::vector<const SemanticExtensionRecord*> bindings_for_object(
    const SemanticSidecar& sidecar, std::uint64_t scene_object_identity) {
    return bindings_for(
        sidecar,
        world::AuthoringBinding{
            world::AuthoringBindingKind::scene_object, scene_object_identity});
}

std::vector<world::PathId> paths_between(
    const world::WorldModel& model, world::AreaId start, world::AreaId end) {
    std::vector<world::PathId> result;
    for (const auto& [identity, path] : model.paths()) {
        if (path.start_area.has_value() && path.start_area.value() == start &&
            path.end_area.has_value() && path.end_area.value() == end) {
            result.push_back(identity);
        }
    }
    return result;
}

} // namespace carto::semantic
