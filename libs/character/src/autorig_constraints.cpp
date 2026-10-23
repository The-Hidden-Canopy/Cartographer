#include <carto/character/autorig_constraints.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace carto::character {
namespace {

constexpr std::size_t kMaxTokenBytes = 128U;
constexpr std::size_t kMaxRationaleBytes = 512U;
constexpr std::size_t kMaxEvidenceReferenceBytes = 512U;

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

core::Diagnostic provenance_context(
    const AutorigLandmarkConstraint& landmark) {
    return core::Diagnostic(core::ErrorCode::validation_failed,
        "landmark assertion " + landmark.provenance.assertion_id +
        " by " + landmark.provenance.author_id +
        " conflicts on role " + landmark.semantic_role +
        " side " + std::to_string(static_cast<unsigned>(landmark.side)) +
        " at part " + landmark.part_id +
        " (anchor " + std::to_string(landmark.vertex.value) + ")");
}

bool valid_token(std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaxTokenBytes &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        });
}

bool valid_side(AutorigPartSide side) noexcept {
    return side >= AutorigPartSide::unspecified && side <= AutorigPartSide::right;
}

bool valid_origin(AutorigConstraintOrigin origin) noexcept {
    return origin == AutorigConstraintOrigin::human_authored ||
        origin == AutorigConstraintOrigin::imported_reference;
}

} // namespace

core::Result<void> AutorigConstraintProvenance::validate() const {
    if (!valid_token(assertion_id) || !valid_token(author_id) ||
        rationale.empty() || rationale.size() > kMaxRationaleBytes ||
        !valid_origin(origin) || evidence_reference.size() > kMaxEvidenceReferenceBytes) {
        return core::Result<void>::failure(invalid(
            "autorig constraint provenance requires bounded assertion, author, rationale, and origin"));
    }
    if (origin == AutorigConstraintOrigin::imported_reference &&
        !valid_token(evidence_reference)) {
        return core::Result<void>::failure(invalid(
            "imported landmark provenance requires a stable evidence reference"));
    }
    if (std::any_of(rationale.begin(), rationale.end(), [](unsigned char byte) {
            return byte < 0x20U && byte != '\t';
        })) {
        return core::Result<void>::failure(invalid(
            "autorig constraint rationale contains control characters"));
    }
    return core::Result<void>::success();
}

core::Result<void> AutorigPartonomyNode::validate() const {
    if (!valid_token(id) || !valid_token(semantic_role) || !valid_side(side) ||
        (parent_id.has_value() &&
            (!valid_token(*parent_id) || *parent_id == id))) {
        return core::Result<void>::failure(invalid(
            "autorig partonomy node identity, role, parent, or side is invalid"));
    }
    return core::Result<void>::success();
}

core::Result<void> AutorigLandmarkConstraint::validate() const {
    if (!valid_token(id) || !valid_token(part_id) || !valid_token(semantic_role) ||
        !valid_side(side) || !vertex) {
        return core::Result<void>::failure(invalid(
            "autorig landmark identity, role, side, part, or vertex is invalid"));
    }
    return provenance.validate();
}

core::Result<void> AutorigAllowedRegionConstraint::validate() const {
    if (!valid_token(id) || !valid_token(part_id) || vertices.empty() ||
        vertices.size() > AutorigConstraintSet::kMaxTotalAllowedRegionVertices ||
        !std::is_sorted(vertices.begin(), vertices.end()) ||
        std::adjacent_find(vertices.begin(), vertices.end()) != vertices.end() ||
        std::any_of(vertices.begin(), vertices.end(), [](geometry::VertexId vertex) {
            return !vertex;
        })) {
        return core::Result<void>::failure(invalid(
            "autorig allowed region requires a bounded, sorted set of unique mesh vertices"));
    }
    return provenance.validate();
}

core::Result<void> AutorigMechanicalPortHint::validate() const {
    const bool fixed = kind == kinematic::JointKind::fixed;
    const bool actuated = kind == kinematic::JointKind::hinge ||
        kind == kinematic::JointKind::slider;
    const double axis_length = std::hypot(std::hypot(axis.x, axis.y), axis.z);
    const bool fixed_frame_valid = fixed && axis.x == 0.0 && axis.y == 0.0 &&
        axis.z == 0.0 && lower_limit == 0.0 && upper_limit == 0.0;
    const bool actuated_frame_valid = actuated && axis.finite() &&
        std::isfinite(axis_length) && std::abs(axis_length - 1.0) <= 1.0e-9 &&
        std::isfinite(lower_limit) && std::isfinite(upper_limit) &&
        lower_limit <= upper_limit;
    if (!valid_token(id) || !valid_token(parent_part_id) ||
        !valid_token(child_part_id) || !valid_token(parent_landmark_id) ||
        !valid_token(child_landmark_id) || parent_part_id == child_part_id ||
        (!fixed_frame_valid && !actuated_frame_valid)) {
        return core::Result<void>::failure(invalid(
            "mechanical port hint requires valid identities and a supported finite joint frame"));
    }
    return provenance.validate();
}

core::Result<void> AutorigConstraintSet::validate_against(
    const AutorigFeatureAnalysis& analysis) const {
    if (auto checked = analysis.validate(); !checked) return checked;
    if (auto checked = source.validate(); !checked) return checked;
    if (source != analysis.source || mesh_revision != analysis.mesh_revision ||
        analysis_digest != analysis.analysis_digest || analysis_digest.is_zero()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "autorig constraints do not match the exact analyzed source revision and digest"));
    }
    if (parts.empty() || parts.size() > kMaxParts || landmarks.empty() ||
        landmarks.size() > kMaxLandmarks || allowed_regions.size() > kMaxAllowedRegions ||
        mechanical_ports.size() > kMaxMechanicalPortHints) {
        return core::Result<void>::failure(invalid(
            "autorig constraints require bounded partonomy, landmark, and allowed-region records"));
    }

    std::map<std::string, const AutorigPartonomyNode*, std::less<>> parts_by_id;
    std::size_t root_count = 0U;
    for (const auto& part : parts) {
        if (auto checked = part.validate(); !checked) return checked;
        if (!parts_by_id.emplace(part.id, &part).second) {
            return core::Result<void>::failure(validation(
                "autorig partonomy contains a duplicate part ID"));
        }
        if (!part.parent_id.has_value()) ++root_count;
    }
    if (root_count != 1U) {
        return core::Result<void>::failure(validation(
            "autorig partonomy must contain exactly one root"));
    }
    for (const auto& part : parts) {
        if (part.parent_id.has_value() && !parts_by_id.contains(*part.parent_id)) {
            return core::Result<void>::failure(validation(
                "autorig partonomy references a missing parent"));
        }
    }

    // Each node has at most one parent; walking parent chains detects cycles
    // without imposing an order on caller records.
    std::map<std::string, std::uint8_t, std::less<>> color;
    for (const auto& part : parts) {
        std::vector<std::string> path;
        auto cursor = part.id;
        while (true) {
            const auto state = color[cursor];
            if (state == 2U) break;
            if (state == 1U) {
                return core::Result<void>::failure(validation(
                    "autorig partonomy contains a parent cycle"));
            }
            color[cursor] = 1U;
            path.push_back(cursor);
            const auto& current = *parts_by_id.at(cursor);
            if (!current.parent_id.has_value()) break;
            cursor = *current.parent_id;
        }
        for (const auto& id : path) color[id] = 2U;
    }

    std::set<std::uint64_t> analyzed_vertices;
    std::map<std::uint64_t, std::uint32_t> analyzed_components;
    for (const auto& feature : analysis.vertices) {
        analyzed_vertices.insert(feature.vertex.value);
        analyzed_components.emplace(feature.vertex.value, feature.component_index);
    }
    std::map<std::string, const AutorigAllowedRegionConstraint*, std::less<>> regions_by_part;
    std::set<std::string> region_ids;
    std::set<std::string> provenance_ids;
    std::size_t total_region_vertices = 0U;
    for (const auto& region : allowed_regions) {
        if (auto checked = region.validate(); !checked) return checked;
        if (!region_ids.insert(region.id).second ||
            !regions_by_part.emplace(region.part_id, &region).second) {
            return core::Result<void>::failure(validation(
                "autorig allowed regions require unique IDs and at most one region per part"));
        }
        if (!parts_by_id.contains(region.part_id)) {
            return core::Result<void>::failure(validation(
                "autorig allowed region references a missing partonomy node"));
        }
        if (region.vertices.size() > kMaxTotalAllowedRegionVertices - total_region_vertices) {
            return core::Result<void>::failure(invalid(
                "autorig allowed-region vertex records exceed their aggregate budget"));
        }
        total_region_vertices += region.vertices.size();
        for (const auto vertex : region.vertices) {
            if (!analyzed_vertices.contains(vertex.value)) {
                return core::Result<void>::failure(validation(
                    "autorig allowed region contains a vertex absent from the analyzed mesh"));
            }
        }
        if (!provenance_ids.insert(region.provenance.assertion_id).second) {
            return core::Result<void>::failure(validation(
                "autorig constraint assertion IDs must be unique"));
        }
    }
    std::set<std::string> landmark_ids;
    std::map<std::string, const AutorigLandmarkConstraint*, std::less<>> landmarks_by_id;
    using RoleKey = std::tuple<std::string, std::string, AutorigPartSide>;
    std::map<RoleKey, const AutorigLandmarkConstraint*> role_assignments;
    for (const auto& landmark : landmarks) {
        if (auto checked = landmark.validate(); !checked) return checked;
        if (!landmark_ids.insert(landmark.id).second) {
            return core::Result<void>::failure(validation(
                "autorig landmark set contains a duplicate record ID"));
        }
        landmarks_by_id.emplace(landmark.id, &landmark);
        const auto part = parts_by_id.find(landmark.part_id);
        if (part == parts_by_id.end()) {
            return core::Result<void>::failure(validation(
                "autorig landmark references a missing partonomy node"));
        }
        if (landmark.side != AutorigPartSide::unspecified &&
            part->second->side != AutorigPartSide::unspecified &&
            landmark.side != part->second->side) {
            return core::Result<void>::failure(validation(
                "autorig landmark side conflicts with its partonomy node"));
        }
        if (!analyzed_vertices.contains(landmark.vertex.value)) {
            return core::Result<void>::failure(validation(
                "autorig landmark anchor is absent from the analyzed mesh"));
        }
        const auto allowed_region = regions_by_part.find(landmark.part_id);
        if (allowed_region != regions_by_part.end() &&
            !std::binary_search(allowed_region->second->vertices.begin(),
                allowed_region->second->vertices.end(), landmark.vertex)) {
            auto diagnostic = validation(
                "autorig landmark anchor lies outside its authored allowed region");
            diagnostic.context.push_back(
                "allowed region assertion " + allowed_region->second->provenance.assertion_id +
                " by " + allowed_region->second->provenance.author_id);
            diagnostic.context.push_back(provenance_context(landmark).message);
            return core::Result<void>::failure(std::move(diagnostic));
        }
        if (!provenance_ids.insert(landmark.provenance.assertion_id).second) {
            return core::Result<void>::failure(validation(
                "autorig landmark assertion IDs must be unique"));
        }

        const RoleKey key{landmark.part_id, landmark.semantic_role, landmark.side};
        const auto [existing, inserted] = role_assignments.emplace(key, &landmark);
        if (!inserted) {
            auto diagnostic = validation(
                "conflicting landmark assignments share one part, role, and side");
            diagnostic.context.push_back(provenance_context(*existing->second).message);
            diagnostic.context.push_back(provenance_context(landmark).message);
            return core::Result<void>::failure(std::move(diagnostic));
        }
    }
    std::set<std::string> mechanical_hint_ids;
    std::set<std::string> mechanical_children;
    for (const auto& hint : mechanical_ports) {
        if (auto checked = hint.validate(); !checked) return checked;
        if (!mechanical_hint_ids.insert(hint.id).second ||
            !mechanical_children.insert(hint.child_part_id).second) {
            return core::Result<void>::failure(validation(
                "mechanical port hints require unique IDs and at most one link per child part"));
        }
        const auto parent = parts_by_id.find(hint.parent_part_id);
        const auto child = parts_by_id.find(hint.child_part_id);
        if (parent == parts_by_id.end() || child == parts_by_id.end() ||
            !child->second->parent_id || *child->second->parent_id != hint.parent_part_id) {
            return core::Result<void>::failure(validation(
                "mechanical port hint must bind a direct authored parent-child part edge"));
        }
        const auto parent_landmark = landmarks_by_id.find(hint.parent_landmark_id);
        const auto child_landmark = landmarks_by_id.find(hint.child_landmark_id);
        if (parent_landmark == landmarks_by_id.end() || child_landmark == landmarks_by_id.end() ||
            parent_landmark->second->part_id != hint.parent_part_id ||
            child_landmark->second->part_id != hint.child_part_id) {
            return core::Result<void>::failure(validation(
                "mechanical port hint landmarks must belong to their declared parts"));
        }
        const auto component_for = [&analyzed_components](geometry::VertexId vertex)
            -> std::optional<std::uint32_t> {
            const auto feature = analyzed_components.find(vertex.value);
            if (feature == analyzed_components.end()) return std::nullopt;
            return feature->second;
        };
        const auto parent_component = component_for(parent_landmark->second->vertex);
        const auto child_component = component_for(child_landmark->second->vertex);
        if (!parent_component || !child_component || *parent_component == *child_component) {
            return core::Result<void>::failure(validation(
                "mechanical port landmarks must lie on distinct analyzed rigid mesh components"));
        }
        if (!provenance_ids.insert(hint.provenance.assertion_id).second) {
            return core::Result<void>::failure(validation(
                "autorig constraint assertion IDs must be unique"));
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
