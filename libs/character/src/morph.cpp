#include <carto/character/morph.hpp>

#include <cmath>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

bool valid_tangent_policy(TangentPolicy policy) {
    switch (policy) {
    case TangentPolicy::none:
    case TangentPolicy::source_preserved:
    case TangentPolicy::recompute:
        return true;
    }
    return false;
}

} // namespace

std::string_view tangent_policy_name(TangentPolicy policy) noexcept {
    switch (policy) {
    case TangentPolicy::none: return "none";
    case TangentPolicy::source_preserved: return "source-preserved";
    case TangentPolicy::recompute: return "recompute";
    }
    return "unknown";
}

core::Result<void> MorphTarget::validate() const {
    if (!id || source_mesh_asset == 0U || source_mesh_digest.is_zero() ||
        displaced_point_count == 0U || displaced_point_count > kMaxDisplacedPoints ||
        delta_blob_digest.is_zero() || delta_blob_bytes == 0U ||
        delta_blob_bytes > kMaxDeltaBlobBytes ||
        topology_correspondence_digest.is_zero() ||
        !valid_tangent_policy(tangent_in) || !valid_tangent_policy(tangent_out) ||
        !std::isfinite(precision_bound) || precision_bound <= 0.0) {
        return core::Result<void>::failure(invalid(
            "morph target identity, digests, bounds, tangent policy, or precision is invalid"));
    }
    return core::Result<void>::success();
}

} // namespace carto::character
