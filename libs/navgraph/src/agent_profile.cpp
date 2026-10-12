#include <carto/navgraph/agent_profile.hpp>

#include "detail.hpp"

#include <algorithm>

namespace carto::navgraph {

core::Result<void> AgentProfile::validate() const {
    if (!detail::valid_locomotion(locomotion) ||
        !detail::safe_text(profile_identity, kMaxLabelBytes) ||
        !clearances.valid() ||
        clearances.minimum.x > clearances.maximum.x ||
        clearances.minimum.y > clearances.maximum.y ||
        clearances.minimum.z > clearances.maximum.z ||
        primitives.size() > kMaxPrimitives ||
        !detail::valid_traversal_primitive_set(primitives)) {
        return core::Result<void>::failure(detail::invalid(
            "agent profile identity, locomotion, clearances, or primitives are invalid"));
    }
    if (!std::is_sorted(primitives.begin(), primitives.end()) ||
        std::adjacent_find(primitives.begin(), primitives.end()) != primitives.end()) {
        return core::Result<void>::failure(detail::validation(
            "agent profile primitives must be sorted and unique"));
    }
    return core::Result<void>::success();
}

bool AgentProfile::supports(TraversalPrimitive primitive) const noexcept {
    return std::binary_search(primitives.begin(), primitives.end(), primitive);
}

core::Result<void> AgentAreaBinding::validate() const {
    if (!region || !detail::safe_text(profile_identity, AgentProfile::kMaxLabelBytes) ||
        required_primitives.size() > AgentProfile::kMaxPrimitives ||
        !detail::valid_traversal_primitive_set(required_primitives)) {
        return core::Result<void>::failure(detail::invalid(
            "agent area binding region, profile, or primitives are invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> AgentPathBinding::validate() const {
    if (!route || !detail::safe_text(profile_identity, AgentProfile::kMaxLabelBytes) ||
        required_primitives.size() > AgentProfile::kMaxPrimitives ||
        !detail::valid_traversal_primitive_set(required_primitives)) {
        return core::Result<void>::failure(detail::invalid(
            "agent path binding route, profile, or primitives are invalid"));
    }
    return core::Result<void>::success();
}

std::vector<TraversalPrimitive> classify_route_primitives(std::string_view semantic_type) {
    if (semantic_type == "water-route") {
        return {TraversalPrimitive::water};
    }
    if (semantic_type == "vessel-deck-route") {
        return {TraversalPrimitive::ground, TraversalPrimitive::vessel_deck};
    }
    if (semantic_type == "ladder-route") {
        return {TraversalPrimitive::ladder};
    }
    return {TraversalPrimitive::ground};
}

bool agent_supports_region(
    const AgentProfile& profile,
    const AgentAreaBinding& binding) noexcept {
    return std::all_of(
        binding.required_primitives.begin(), binding.required_primitives.end(),
        [&profile](TraversalPrimitive primitive) { return profile.supports(primitive); });
}

bool agent_supports_route(
    const AgentProfile& profile,
    const AgentPathBinding& binding) noexcept {
    return std::all_of(
        binding.required_primitives.begin(), binding.required_primitives.end(),
        [&profile](TraversalPrimitive primitive) { return profile.supports(primitive); });
}

bool agent_supports_access(
    const AgentProfile& profile,
    AccessPrimitive primitive) noexcept {
    return profile.supports(traversal_primitive_for_access(primitive));
}

} // namespace carto::navgraph
