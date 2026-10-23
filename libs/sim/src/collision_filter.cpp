#include <carto/sim/collision_filter.hpp>

#include <carto/sim/caps.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace carto::sim {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

bool joint_less(kinematic::JointId left, kinematic::JointId right) noexcept {
    return left.value < right.value;
}

assets::Sha256Digest digest_of(const std::string& canonical) {
    return assets::sha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()), canonical.size()});
}

} // namespace

core::Result<void> CollisionFilter::validate() const {
    if (excluded_joints.empty() || excluded_joints.size() > kMaxExcludedJoints) {
        return core::Result<void>::failure(validation(
            "collision filter requires a bounded non-empty excluded-joint set"));
    }
    for (const kinematic::JointId joint : excluded_joints) {
        if (joint.value == 0U) {
            return core::Result<void>::failure(invalid(
                "collision filter cannot exclude the invalid joint identity"));
        }
    }
    for (std::size_t index = 1U; index < excluded_joints.size(); ++index) {
        if (!(excluded_joints[index - 1U] < excluded_joints[index])) {
            return core::Result<void>::failure(validation(
                "collision filter excluded joints must be sorted and unique"));
        }
    }
    return core::Result<void>::success();
}

bool CollisionFilter::excludes(kinematic::JointId joint) const noexcept {
    return std::binary_search(
        excluded_joints.begin(), excluded_joints.end(), joint, joint_less);
}

std::string CollisionFilter::serialize() const {
    std::ostringstream output;
    output << "CARTO_SIM_COLLISION_FILTER " << kSchemaVersion << '\n';
    output << "EXCLUDED " << excluded_joints.size() << '\n';
    for (const kinematic::JointId joint : excluded_joints) {
        output << joint.value << '\n';
    }
    output << "END\n";
    return output.str();
}

core::Result<assets::Sha256Digest> CollisionFilter::canonical_digest() const {
    if (auto result = validate(); !result) {
        return core::Result<assets::Sha256Digest>::failure(result.error());
    }
    return core::Result<assets::Sha256Digest>::success(digest_of(serialize()));
}

core::Result<CollisionFilter> merge_collision_filters(
    std::span<const CollisionFilter> filters) {
    if (filters.empty() || filters.size() > SimCaps::kMaxCollisionFilters) {
        return core::Result<CollisionFilter>::failure(unsupported(
            "collision filter application exceeds its declared bounded capacity"));
    }
    CollisionFilter merged;
    for (const CollisionFilter& filter : filters) {
        if (auto result = filter.validate(); !result) {
            return core::Result<CollisionFilter>::failure(
                result.error().with_context("merging collision filters"));
        }
        merged.excluded_joints.insert(
            merged.excluded_joints.end(),
            filter.excluded_joints.begin(),
            filter.excluded_joints.end());
    }
    std::sort(merged.excluded_joints.begin(), merged.excluded_joints.end(), joint_less);
    const auto duplicate = std::adjacent_find(
        merged.excluded_joints.begin(), merged.excluded_joints.end());
    if (duplicate != merged.excluded_joints.end()) {
        return core::Result<CollisionFilter>::failure(validation(
            "collision filter merge produced a duplicate joint entry"));
    }
    if (merged.excluded_joints.size() > CollisionFilter::kMaxExcludedJoints) {
        return core::Result<CollisionFilter>::failure(unsupported(
            "merged collision filter exceeds its declared bounded capacity"));
    }
    return core::Result<CollisionFilter>::success(std::move(merged));
}

} // namespace carto::sim
