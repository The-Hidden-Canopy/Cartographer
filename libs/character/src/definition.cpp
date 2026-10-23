#include <carto/character/definition.hpp>

#include <algorithm>
#include <utility>

namespace carto::character {
namespace {

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool safe_text(std::string_view value, std::size_t maximum, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > maximum) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return byte >= 0x20U && byte != 0x7fU && byte != '\t' &&
            byte != '\r' && byte != '\n';
    });
}

} // namespace

core::Result<void> MeshAssetRef::validate() const {
    if (asset_id == 0U || content_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "mesh asset reference requires an asset id and content digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> RigRef::validate() const {
    if (!rig || graph_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "rig reference requires a rig id and kinematic graph digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> JointLimitSetRef::validate() const {
    if (digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "joint limit set reference requires a content digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> ControlGraphRef::validate() const {
    if (!owner || digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "control graph reference requires an owner rig id and digest"));
    }
    return core::Result<void>::success();
}

core::Result<void> CharacterDefinition::validate() const {
    if (!id || !safe_text(project_namespace, kMaxNamespaceBytes) ||
        source_revision.value() == 0U || source_digest.is_zero() || !root ||
        mesh_refs.size() > kMaxMeshRefs ||
        locale_neutral_labels.size() > kMaxLabels ||
        coordinate_policy_digest.is_zero()) {
        return core::Result<void>::failure(invalid(
            "character definition identity, provenance, root, bounds, or policy digest is invalid"));
    }
    if (auto result = rig.validate(); !result) return result;
    if (auto result = control_graph.validate(); !result) return result;
    // Distinct identities: mesh references sorted and unique by asset id, so
    // an ID collision is a validation failure, never a silent merge.
    for (const MeshAssetRef& ref : mesh_refs) {
        if (auto result = ref.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < mesh_refs.size(); ++index) {
        if (!(mesh_refs[index - 1U] < mesh_refs[index])) {
            return core::Result<void>::failure(validation(
                "character definition mesh references must be sorted and unique"));
        }
    }
    for (const auto& [key, value] : locale_neutral_labels) {
        if (!safe_text(key, kMaxLabelKeyBytes) ||
            !safe_text(value, kMaxLabelValueBytes, true)) {
            return core::Result<void>::failure(invalid(
                "character definition label keys and values must be bounded safe text"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> SymmetryPair::validate() const {
    if (!left_joint || !right_joint || left_joint == right_joint) {
        return core::Result<void>::failure(invalid(
            "symmetry pair requires two distinct joint identities"));
    }
    return core::Result<void>::success();
}

core::Result<void> ControlSymmetryPair::validate() const {
    if (!left_control || !right_control || left_control == right_control) {
        return core::Result<void>::failure(invalid(
            "control symmetry pair requires two distinct control identities"));
    }
    return core::Result<void>::success();
}

core::Result<void> RigDefinition::validate() const {
    if (!id || kinematic_graph_digest.is_zero() || semantic_tags.size() > kMaxSemanticTags ||
        symmetry.size() > kMaxSymmetryPairs) {
        return core::Result<void>::failure(invalid(
            "rig definition identity, digests, or bounded size is invalid"));
    }
    if (auto result = joint_limits.validate(); !result) return result;
    if (auto result = rest_basis.validate(); !result) return result;
    if (auto result = control_graph.validate(); !result) return result;
    if (!std::is_sorted(semantic_tags.begin(), semantic_tags.end()) ||
        std::adjacent_find(semantic_tags.begin(), semantic_tags.end()) !=
            semantic_tags.end() ||
        std::any_of(semantic_tags.begin(), semantic_tags.end(),
            [](const std::string& tag) { return !safe_text(tag, kMaxSemanticTagBytes); })) {
        return core::Result<void>::failure(validation(
            "rig semantic tags must be bounded, sorted, unique, safe text"));
    }
    for (const SymmetryPair& pair : symmetry) {
        if (auto result = pair.validate(); !result) return result;
    }
    for (std::size_t index = 1U; index < symmetry.size(); ++index) {
        if (!(symmetry[index - 1U].left_joint < symmetry[index].left_joint)) {
            return core::Result<void>::failure(validation(
                "rig symmetry pairs must be sorted by left joint"));
        }
    }
    for (std::size_t left = 0U; left < symmetry.size(); ++left) {
        for (std::size_t right = left + 1U; right < symmetry.size(); ++right) {
            if (symmetry[left].right_joint == symmetry[right].right_joint) {
                return core::Result<void>::failure(validation(
                    "rig symmetry right joints must be unique"));
            }
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::character
