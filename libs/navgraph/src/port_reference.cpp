#include <carto/navgraph/port_reference.hpp>

#include "detail.hpp"

#include <algorithm>

namespace carto::navgraph {

std::optional<AccessPrimitive> PortReference::traversal_primitive() const noexcept {
    for (const auto& tag : tags) {
        if (const auto primitive = classify_access_primitive(tag); primitive.has_value()) {
            return primitive;
        }
    }
    return std::nullopt;
}

core::Result<void> PortReference::validate() const {
    if (!detail::safe_text(port_identity, kMaxIdentityBytes) ||
        !detail::safe_text(owner_identity, kMaxOwnerBytes)) {
        return core::Result<void>::failure(detail::invalid(
            "port reference identity or owner is invalid"));
    }
    if (!local_frame.finite()) {
        return core::Result<void>::failure(detail::invalid(
            "port reference local frame is not finite"));
    }
    if (tags.size() > kMaxTags || !std::is_sorted(tags.begin(), tags.end()) ||
        std::adjacent_find(tags.begin(), tags.end()) != tags.end()) {
        return core::Result<void>::failure(detail::validation(
            "port reference tags must be bounded, sorted, and unique"));
    }
    for (const auto& tag : tags) {
        if (!detail::safe_text(tag, kMaxTagBytes)) {
            return core::Result<void>::failure(detail::invalid(
                "port reference tag is invalid"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::navgraph
