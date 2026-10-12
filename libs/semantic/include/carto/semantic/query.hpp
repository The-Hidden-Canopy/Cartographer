#pragma once

#include <carto/semantic/sidecar.hpp>
#include <carto/world/model.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace carto::semantic {

// Headless, read-only query helpers over the authoritative world model and a
// semantic sidecar. Results are sorted by stable identity and never mutate
// either input.

// Semantic areas whose type equals `semantic_type`, sorted by area id.
[[nodiscard]] std::vector<world::AreaId> areas_with_type(
    const world::WorldModel& model, std::string_view semantic_type);

// Semantic areas carrying `tag`, sorted by area id.
[[nodiscard]] std::vector<world::AreaId> areas_with_tag(
    const world::WorldModel& model, std::string_view tag);

// Extension records bound to the exact authored identity, sorted by schema
// identity then payload digest.
[[nodiscard]] std::vector<const SemanticExtensionRecord*> bindings_for(
    const SemanticSidecar& sidecar, world::AuthoringBinding bound);

// Convenience form of bindings_for for scene-object identities.
[[nodiscard]] std::vector<const SemanticExtensionRecord*> bindings_for_object(
    const SemanticSidecar& sidecar, std::uint64_t scene_object_identity);

// Semantic paths that start at `start` and end at `end` exactly (authored
// direction), sorted by path id.
[[nodiscard]] std::vector<world::PathId> paths_between(
    const world::WorldModel& model, world::AreaId start, world::AreaId end);

} // namespace carto::semantic
