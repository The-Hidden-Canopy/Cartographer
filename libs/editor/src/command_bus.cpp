#include <carto/editor/command_bus.hpp>
#include <carto/mesh_attributes/provenance.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace carto::editor {

namespace {

} // namespace

core::Result<void> CommandBus::execute(std::unique_ptr<EditorCommand> command) {
    if (!command) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_argument, "command bus cannot execute a null command"));
    }
    if (auto result = command->execute(); !result) {
        return result;
    }
    undo_stack_.push_back(std::move(command));
    redo_stack_.clear();
    return core::Result<void>::success();
}

core::Result<void> CommandBus::rollback_last_execute() {
    if (undo_stack_.empty()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state,
                             "command history has no accepted command to roll back"));
    }
    auto command = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    if (auto result = command->undo(); !result) {
        undo_stack_.push_back(std::move(command));
        return result;
    }
    // The failed admission consumed the redo branch when execute() accepted
    // the command. It must not become visible as a successful history state.
    redo_stack_.clear();
    return core::Result<void>::success();
}

core::Result<void> CommandBus::undo() {
    if (undo_stack_.empty()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "command history has no undo entry"));
    }
    auto& command = undo_stack_.back();
    if (auto result = command->undo(); !result) {
        return result;
    }
    redo_stack_.push_back(std::move(command));
    undo_stack_.pop_back();
    return core::Result<void>::success();
}

core::Result<void> CommandBus::redo() {
    if (redo_stack_.empty()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "command history has no redo entry"));
    }
    auto& command = redo_stack_.back();
    if (auto result = command->execute(); !result) {
        return result;
    }
    undo_stack_.push_back(std::move(command));
    redo_stack_.pop_back();
    return core::Result<void>::success();
}

void CommandBus::clear_history() noexcept {
    undo_stack_.clear();
    redo_stack_.clear();
}

SetObjectTransformCommand::SetObjectTransformCommand(
    scene::Scene& scene,
    scene::ObjectId object,
    core::Transform before,
    core::Transform after)
    : scene_(&scene), object_(object), before_(before), after_(after) {}

core::Result<void> SetObjectTransformCommand::execute() { return apply(after_); }

core::Result<void> SetObjectTransformCommand::undo() { return apply(before_); }

core::Result<void> SetObjectTransformCommand::apply(core::Transform transform) {
    if (!scene_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "transform command has no scene"));
    }
    return scene_->set_local_transform(object_, transform);
}

SetProjectObjectTransformCommand::SetProjectObjectTransformCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    scene::ObjectId object,
    core::Transform before,
    core::Transform after)
    : document_(&document),
      object_(object),
      before_(before),
      after_(after),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectObjectTransformCommand::execute() {
    return apply(after_);
}

core::Result<void> SetProjectObjectTransformCommand::undo() {
    return apply(before_);
}

core::Result<void> SetProjectObjectTransformCommand::apply(core::Transform transform) {
    if (!document_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project transform command has no document"));
    }
    const core::Revision expected =
        current_project_revision_.value_or(expected_project_revision_);
    auto result = document_->set_object_transform_if_revision(object_, expected, transform);
    if (!result) {
        return core::Result<void>::failure(result.error());
    }
    current_project_revision_ = result.value();
    return core::Result<void>::success();
}

SetProjectSculptStackCommand::SetProjectSculptStackCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    std::optional<project::SerializedSculptStack> before,
    project::SerializedSculptStack after)
    : document_(&document),
      mesh_asset_(mesh_asset),
      before_(std::move(before)),
      after_(std::move(after)),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectSculptStackCommand::execute() {
    if (!document_ || mesh_asset_ == 0U || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project SculptStack command has incomplete or already-applied state"));
    }
    if (document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project SculptStack command revision no longer matches the project"));
    }
    const auto current = document_->sculpt_stacks().find(mesh_asset_);
    if ((before_.has_value() &&
            (current == document_->sculpt_stacks().end() || current->second != *before_)) ||
        (!before_.has_value() && current != document_->sculpt_stacks().end())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project SculptStack changed before the command was applied"));
    }
    if (auto result = document_->set_sculpt_stack(
            mesh_asset_, after_.bytes, after_.digest); !result) {
        return result;
    }
    if (document_->revision() == expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project SculptStack command did not change the project"));
    }
    expected_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectSculptStackCommand::undo() {
    if (!document_ || !applied_ || mesh_asset_ == 0U ||
        document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project SculptStack command cannot be undone in the current revision"));
    }
    const auto current = document_->sculpt_stacks().find(mesh_asset_);
    if (current == document_->sculpt_stacks().end() || current->second != after_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project SculptStack no longer matches the command being undone"));
    }
    core::Result<void> result = before_.has_value()
        ? document_->set_sculpt_stack(mesh_asset_, before_->bytes, before_->digest)
        : document_->erase_sculpt_stack_if_revision(mesh_asset_, expected_project_revision_);
    if (!result) return result;
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectEngineeringAuthoringCommand::SetProjectEngineeringAuthoringCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::optional<project::SerializedEngineeringAuthoring> before,
    project::SerializedEngineeringAuthoring after)
    : document_(&document),
      before_(std::move(before)),
      after_(std::move(after)),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectEngineeringAuthoringCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring command has incomplete or already-applied state"));
    }
    if (document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering authoring command revision no longer matches the project"));
    }
    const auto& current = document_->engineering_authoring();
    if (current != before_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering authoring changed before the command was applied"));
    }
    if (auto result = document_->set_engineering_authoring(after_.bytes, after_.digest);
        !result) {
        return result;
    }
    if (document_->revision() == expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "engineering authoring command did not change the project"));
    }
    expected_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectEngineeringAuthoringCommand::undo() {
    if (!document_ || !applied_ || document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering authoring command cannot be undone in the current revision"));
    }
    const auto& current = document_->engineering_authoring();
    if (!current.has_value() || *current != after_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "engineering authoring no longer matches the command being undone"));
    }
    core::Result<void> result = before_.has_value()
        ? document_->set_engineering_authoring(before_->bytes, before_->digest)
        : document_->erase_engineering_authoring_if_revision(expected_project_revision_);
    if (!result) return result;
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

ReplaceProjectDocumentCommand::ReplaceProjectDocumentCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    project::ProjectDocument candidate,
    std::string label)
    : document_(&document),
      before_(document),
      after_(std::move(candidate)),
      label_(std::move(label)),
      expected_project_revision_(document.revision()) {}

bool ReplaceProjectDocumentCommand::matches_snapshot_ignoring_revision(
    const project::ProjectDocument& expected) const {
    if (!document_) return false;
    project::ProjectDocument normalized(*document_);
    normalized.revision_ = expected.revision();
    return normalized.serialize() == expected.serialize();
}

core::Result<void> ReplaceProjectDocumentCommand::execute() {
    if (!document_ || applied_ || expected_project_revision_.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "speculative project replacement is unavailable in the current state"));
    }
    if (!initialized_) {
        if (document_->revision() != expected_project_revision_) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project changed before speculative acceptance"));
        }
    } else if (!matches_snapshot_ignoring_revision(before_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project no longer matches the source snapshot for redo"));
    }
    if (auto result = after_.validate(); !result) return result;

    project::ProjectDocument replacement(after_);
    replacement.revision_ = document_->revision();
    replacement.bump_revision();
    document_->swap(replacement);
    expected_project_revision_ = document_->revision();
    initialized_ = true;
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> ReplaceProjectDocumentCommand::undo() {
    if (!document_ || !applied_ || expected_project_revision_.exhausted() ||
        !matches_snapshot_ignoring_revision(after_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "speculative result no longer matches the project and cannot be undone"));
    }
    project::ProjectDocument replacement(before_);
    replacement.revision_ = document_->revision();
    replacement.bump_revision();
    document_->swap(replacement);
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectConditionAuthoringCommand::SetProjectConditionAuthoringCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::optional<project::SerializedConditionAuthoring> before,
    project::SerializedConditionAuthoring after)
    : document_(&document),
      before_(std::move(before)),
      after_(std::move(after)),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectConditionAuthoringCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION authoring command has incomplete or already-applied state"));
    }
    if (!has_executed_ && document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION authoring command revision no longer matches the project"));
    }
    if (document_->condition_authoring() != before_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION authoring changed before the command was applied"));
    }
    if (auto result = document_->set_condition_authoring(after_.bytes, after_.digest);
        !result) {
        return result;
    }
    if (document_->revision() == expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION authoring command did not change the project"));
    }
    expected_project_revision_ = document_->revision();
    has_executed_ = true;
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectConditionAuthoringCommand::undo() {
    if (!document_ || !applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION authoring command is not in an applied state"));
    }
    if (document_->condition_authoring() != std::optional<project::SerializedConditionAuthoring>{after_}) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION authoring no longer matches the command being undone"));
    }
    core::Result<void> result = before_.has_value()
        ? document_->set_condition_authoring(before_->bytes, before_->digest)
        : document_->erase_condition_authoring_if_revision(document_->revision());
    if (!result) return result;
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectMaterialCatalogCommand::SetProjectMaterialCatalogCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    project::MaterialCatalog before,
    project::MaterialCatalog after)
    : document_(&document),
      before_(std::move(before)),
      after_(std::move(after)),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectMaterialCatalogCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "material catalog command has incomplete or already-applied state"));
    }
    // Initial admission uses the exact caller revision. Redo occurs after
    // later commands have been undone, so the project revision has advanced;
    // the full catalog snapshot below is the command-scoped freshness fence.
    if (!current_project_revision_.has_value() &&
        document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material catalog command revision no longer matches the project"));
    }
    const core::Revision revision_before = document_->revision();
    if (document_->material_catalog().serialize() != before_.serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material catalog changed before the command was applied"));
    }
    if (auto result = document_->set_material_catalog(after_); !result) return result;
    if (document_->revision() == revision_before) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "material catalog command did not change the project"));
    }
    current_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectMaterialCatalogCommand::undo() {
    // Undoing later project commands advances the document revision. The
    // exact catalog equality check protects this command's owned state while
    // allowing the history stack to unwind in sequence.
    if (!document_ || !applied_ || !current_project_revision_.has_value() ||
        document_->material_catalog().serialize() != after_.serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "material catalog no longer matches the command being undone"));
    }
    if (auto result = document_->set_material_catalog(before_); !result) return result;
    current_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectPresentationCatalogCommand::SetProjectPresentationCatalogCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    project::PresentationCatalog before,
    project::PresentationCatalog after)
    : document_(&document),
      before_(std::move(before)),
      after_(std::move(after)),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectPresentationCatalogCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "scene presentation command has incomplete or already-applied state"));
    }
    if (!current_project_revision_.has_value() &&
        document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "scene presentation command revision no longer matches the project"));
    }
    const core::Revision revision_before = document_->revision();
    if (document_->presentation_catalog().serialize() != before_.serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "scene presentation changed before the command was applied"));
    }
    if (auto result = document_->set_presentation_catalog(after_); !result) return result;
    if (document_->revision() == revision_before) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "scene presentation command did not change the project"));
    }
    current_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectPresentationCatalogCommand::undo() {
    if (!document_ || !applied_ || !current_project_revision_.has_value() ||
        document_->presentation_catalog().serialize() != after_.serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "scene presentation no longer matches the command being undone"));
    }
    if (auto result = document_->set_presentation_catalog(before_); !result) return result;
    current_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectAuthoringStandardsCommand::SetProjectAuthoringStandardsCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    project::ProjectAuthoringStandards before,
    project::ProjectAuthoringStandards after)
    : document_(&document),
      before_(std::move(before)),
      after_(std::move(after)),
      expected_project_revision_(document.revision()) {}

core::Result<void> SetProjectAuthoringStandardsCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project authoring standards command has incomplete or already-applied state"));
    }
    const core::Revision expected =
        current_project_revision_.value_or(expected_project_revision_);
    if (document_->revision() != expected ||
        document_->authoring_standards().serialize() != before_.serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project authoring standards changed before the command was applied"));
    }
    if (auto result = document_->set_authoring_standards(after_); !result) return result;
    if (document_->revision() == expected) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project authoring standards command did not change the project"));
    }
    current_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectAuthoringStandardsCommand::undo() {
    if (!document_ || !applied_ || !current_project_revision_.has_value() ||
        document_->revision() != *current_project_revision_ ||
        document_->authoring_standards().serialize() != after_.serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project authoring standards no longer match the command being undone"));
    }
    if (auto result = document_->set_authoring_standards(before_); !result) return result;
    current_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

ApplySourceReimportPlanCommand::ApplySourceReimportPlanCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    assets::SourceReimportPlan reviewed_plan)
    : document_(&document),
      reviewed_plan_(std::move(reviewed_plan)),
      expected_project_revision_(document.revision()) {}

core::Result<assets::SourceReimportPlan>
ApplySourceReimportPlanCommand::plan_for_records(
    const std::vector<assets::SourceImportRecord>& records) const {
    if (!document_) {
        return core::Result<assets::SourceReimportPlan>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source reimport command has no project document"));
    }
    std::vector<assets::SourceImportRecord> incoming = records;
    const auto& registry = document_->source_asset_registry();
    for (auto& record : incoming) {
        const auto active = registry.records().find(record.identity);
        const auto lineage = registry.lineage().find(record.identity);
        if (active != registry.records().end()) {
            const auto original_generation = record.source_generation;
            record.source_generation = active->second.source_generation;
            if (record.serialize() == active->second.serialize()) continue;
            record.source_generation = original_generation;
        }
        if (lineage == registry.lineage().end()) {
            record.source_generation = core::Revision{1U};
            continue;
        }
        if (lineage->second.exhausted()) {
            return core::Result<assets::SourceReimportPlan>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "source asset generation space is exhausted during command replay"));
        }
        record.source_generation = lineage->second.next();
    }
    return assets::plan_source_reimport(registry, std::move(incoming));
}

core::Result<void> ApplySourceReimportPlanCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source reimport command has incomplete or already-applied state"));
    }
    if (document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source reimport command revision no longer matches the project"));
    }
    if (!initialized_) {
        const auto updated = assets::apply_source_reimport_plan(
            document_->source_asset_registry(), reviewed_plan_);
        if (!updated) return core::Result<void>::failure(updated.error());
        if (updated.value().serialize() == document_->source_asset_registry().serialize()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "unchanged source reimport plan cannot enter command history"));
        }
        for (const auto& [identity, record] : document_->source_asset_registry().records()) {
            static_cast<void>(identity);
            before_records_.push_back(record);
        }
        for (const auto& entry : reviewed_plan_.entries) {
            if (entry.incoming.has_value()) after_records_.push_back(*entry.incoming);
        }
        if (auto result = document_->apply_source_reimport_plan(reviewed_plan_); !result) {
            return result;
        }
        initialized_ = true;
    } else {
        const auto redo_plan = plan_for_records(after_records_);
        if (!redo_plan) return core::Result<void>::failure(redo_plan.error());
        if (auto result = document_->apply_source_reimport_plan(redo_plan.value()); !result) {
            return result;
        }
    }
    expected_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> ApplySourceReimportPlanCommand::undo() {
    if (!document_ || !initialized_ || !applied_ ||
        document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source reimport command is no longer current and cannot be undone"));
    }
    const auto undo_plan = plan_for_records(before_records_);
    if (!undo_plan) return core::Result<void>::failure(undo_plan.error());
    if (auto result = document_->apply_source_reimport_plan(undo_plan.value()); !result) {
        return result;
    }
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectSpatialFieldCommand::SetProjectSpatialFieldCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::optional<spatial_field::ScalarField2D> before,
    spatial_field::ScalarField2D after)
    : document_(&document), before_(std::move(before)), after_(std::move(after)) {}

core::Result<void> SetProjectSpatialFieldCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "spatial field command has incomplete or already-applied state"));
    }
    const auto current = document_->spatial_fields().find(after_.identity());
    if (before_.has_value()) {
        if (current == document_->spatial_fields().end() ||
            !current->second.same_content(*before_)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "spatial field changed before the command was applied"));
        }
    } else if (current != document_->spatial_fields().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "spatial field identity is already active"));
    }

    spatial_field::ScalarField2D target = after_;
    if (current != document_->spatial_fields().end()) {
        auto revised = after_.at_next_revision(current->second.source_revision());
        if (!revised) return core::Result<void>::failure(revised.error());
        target = std::move(revised.value());
    } else {
        const auto lineage = document_->spatial_field_lineage_.find(after_.identity());
        if (lineage != document_->spatial_field_lineage_.end()) {
            auto revised = after_.at_next_revision(lineage->second);
            if (!revised) return core::Result<void>::failure(revised.error());
            target = std::move(revised.value());
        } else if (after_.source_revision() != core::Revision{1U}) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "new spatial field does not begin at source revision one"));
        }
    }
    if (auto result = document_->set_spatial_field(target); !result) return result;
    after_ = std::move(target);
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectSpatialFieldCommand::undo() {
    if (!document_ || !applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "spatial field command cannot be undone in the current project revision"));
    }
    const auto current = document_->spatial_fields().find(after_.identity());
    if (current == document_->spatial_fields().end() ||
        (before_.has_value()
            ? current->second.serialize() != after_.serialize()
            : !current->second.same_content(after_))) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "spatial field no longer matches the command being undone"));
    }
    if (before_.has_value()) {
        auto restored = before_->at_next_revision(current->second.source_revision());
        if (!restored) return core::Result<void>::failure(restored.error());
        if (auto result = document_->set_spatial_field(restored.value()); !result) return result;
        before_ = std::move(restored.value());
    } else {
        if (auto result = document_->erase_spatial_field_if_revision(
                after_.identity(), current->second.source_revision()); !result) {
            return result;
        }
    }
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectRoadAuthoringCommand::SetProjectRoadAuthoringCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    geometry::CurvePath centerline,
    project::RoadSource road)
    : document_(&document),
      authored_centerline_(std::move(centerline)),
      authored_road_(std::move(road)),
      expected_project_revision_(document.revision()) {
    const auto path = document.curve_paths().find(authored_centerline_.source_identity);
    if (path != document.curve_paths().end()) expected_centerline_ = path->second;
    const auto source = document.road_sources().find(authored_road_.source_identity);
    if (source != document.road_sources().end()) expected_road_ = source->second;
}

core::Result<void> SetProjectRoadAuthoringCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "road authoring command has incomplete or already-applied state"));
    }
    if (document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road authoring command project revision is stale"));
    }
    const auto same_path = [](const auto& left, const auto& right) {
        return left.has_value() == right.has_value() &&
            (!left.has_value() || left->serialize() == right->serialize());
    };
    const auto same_road = [](const auto& left, const auto& right) {
        return left.has_value() == right.has_value() &&
            (!left.has_value() || left->serialize() == right->serialize());
    };
    const auto current_path = document_->curve_paths().find(authored_centerline_.source_identity);
    const auto current_road = document_->road_sources().find(authored_road_.source_identity);
    const std::optional<geometry::CurvePath> observed_path =
        current_path == document_->curve_paths().end()
            ? std::nullopt : std::optional<geometry::CurvePath>{current_path->second};
    const std::optional<project::RoadSource> observed_road =
        current_road == document_->road_sources().end()
            ? std::nullopt : std::optional<project::RoadSource>{current_road->second};
    if (!same_path(observed_path, expected_centerline_) ||
        !same_road(observed_road, expected_road_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road or centerline source changed before the command was applied"));
    }
    if (authored_road_.centerline_identity != authored_centerline_.source_identity ||
        (authored_road_.centerline_revision != authored_centerline_.source_revision &&
            !initialized_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "road authoring must bind the supplied centerline identity and revision"));
    }
    if (expected_road_.has_value() &&
        expected_road_->centerline_identity != authored_centerline_.source_identity) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "editing a road centerline association is not supported by this authoring action"));
    }
    for (const auto& [identity, road] : document_->road_sources()) {
        if (road.centerline_identity == authored_centerline_.source_identity &&
            identity != authored_road_.source_identity) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "centerline editing requires one road source per centerline in this authoring slice"));
        }
    }

    project::ProjectDocument candidate(*document_);
    auto next_revision = [](const auto& lineage, const std::string& identity)
        -> core::Result<core::Revision> {
        const auto found = lineage.find(identity);
        if (found == lineage.end()) {
            return core::Result<core::Revision>::success(core::Revision{1U});
        }
        if (found->second.exhausted()) {
            return core::Result<core::Revision>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state, "source revision space is exhausted"));
        }
        return core::Result<core::Revision>::success(found->second.next());
    };
    auto centerline_revision = next_revision(
        candidate.curve_path_lineage_, authored_centerline_.source_identity);
    auto road_revision = next_revision(
        candidate.road_source_lineage_, authored_road_.source_identity);
    if (!centerline_revision) return core::Result<void>::failure(centerline_revision.error());
    if (!road_revision) return core::Result<void>::failure(road_revision.error());
    if (!initialized_ &&
        (authored_centerline_.source_revision != centerline_revision.value() ||
         authored_road_.source_revision != road_revision.value())) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "authored curve or road source revision does not match retained lineage"));
    }

    previous_centerline_ = expected_centerline_;
    previous_road_ = expected_road_;
    geometry::CurvePath centerline = authored_centerline_;
    centerline.source_revision = centerline_revision.value();
    project::RoadSource road = authored_road_;
    road.source_revision = road_revision.value();
    road.centerline_revision = centerline_revision.value();
    if (auto result = candidate.set_curve_path(centerline); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (auto result = candidate.set_road_source(road); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (document_->revision().exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "project revision space is exhausted"));
    }
    candidate.revision_ = document_->revision();
    candidate.bump_revision();
    if (auto result = candidate.validate(); !result) return result;

    expected_project_revision_ = candidate.revision();
    expected_centerline_ = centerline;
    expected_road_ = road;
    document_->swap(candidate);
    initialized_ = true;
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectRoadAuthoringCommand::undo() {
    if (!document_ || !applied_ ||
        document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road authoring command cannot be undone in the current project revision"));
    }
    if (!expected_centerline_.has_value() || !expected_road_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "applied road authoring is missing its current source pair"));
    }
    const auto current_path = document_->curve_paths().find(authored_centerline_.source_identity);
    const auto current_road = document_->road_sources().find(authored_road_.source_identity);
    if (current_path == document_->curve_paths().end() ||
        current_road == document_->road_sources().end() ||
        current_path->second.serialize() != expected_centerline_->serialize() ||
        current_road->second.serialize() != expected_road_->serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "road authoring sources no longer match the command being undone"));
    }
    project::ProjectDocument candidate(*document_);
    const core::Revision current_centerline_revision = current_path->second.source_revision;
    const core::Revision current_road_revision = current_road->second.source_revision;
    if (previous_road_.has_value()) {
        if (previous_centerline_.has_value()) {
            const auto curve_lineage = candidate.curve_path_lineage_.find(
                previous_centerline_->source_identity);
            if (curve_lineage == candidate.curve_path_lineage_.end() ||
                curve_lineage->second.exhausted()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state, "curve source revision space is exhausted"));
            }
            geometry::CurvePath restored_centerline = *previous_centerline_;
            restored_centerline.source_revision = curve_lineage->second.next();
            if (auto result = candidate.set_curve_path(restored_centerline); !result) {
                return result;
            }
            const auto road_lineage = candidate.road_source_lineage_.find(
                previous_road_->source_identity);
            if (road_lineage == candidate.road_source_lineage_.end() ||
                road_lineage->second.exhausted()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state, "road source revision space is exhausted"));
            }
            project::RoadSource restored_road = *previous_road_;
            restored_road.source_revision = road_lineage->second.next();
            restored_road.centerline_revision = restored_centerline.source_revision;
            if (auto result = candidate.set_road_source(restored_road); !result) return result;
            previous_centerline_ = restored_centerline;
            previous_road_ = restored_road;
            expected_centerline_ = restored_centerline;
            expected_road_ = restored_road;
        } else {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "a road source cannot predate its bound centerline"));
        }
    } else {
        if (auto result = candidate.erase_road_source_if_revision(
                authored_road_.source_identity, current_road_revision); !result) return result;
        expected_road_.reset();
        if (previous_centerline_.has_value()) {
            const auto curve_lineage = candidate.curve_path_lineage_.find(
                previous_centerline_->source_identity);
            if (curve_lineage == candidate.curve_path_lineage_.end() ||
                curve_lineage->second.exhausted()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state, "curve source revision space is exhausted"));
            }
            geometry::CurvePath restored_centerline = *previous_centerline_;
            restored_centerline.source_revision = curve_lineage->second.next();
            if (auto result = candidate.set_curve_path(restored_centerline); !result) return result;
            previous_centerline_ = restored_centerline;
            expected_centerline_ = restored_centerline;
        } else {
            if (auto result = candidate.erase_curve_path_if_revision(
                    authored_centerline_.source_identity, current_centerline_revision); !result) {
                return result;
            }
            expected_centerline_.reset();
        }
    }
    if (document_->revision().exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "project revision space is exhausted"));
    }
    candidate.revision_ = document_->revision();
    candidate.bump_revision();
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectSweepSourceCommand::SetProjectSweepSourceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    geometry::SweepSource source)
    : document_(&document),
      authored_source_(std::move(source)),
      expected_project_revision_(document.revision()) {
    const auto existing = document.sweep_sources().find(authored_source_.source_identity);
    if (existing != document.sweep_sources().end()) {
        previous_source_ = existing->second;
        expected_current_source_ = existing->second;
    }
}

core::Result<void> SetProjectSweepSourceCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "sweep source command has incomplete or already-applied state"));
    }
    if (document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep source command project revision is stale"));
    }
    const auto current = document_->sweep_sources().find(authored_source_.source_identity);
    const std::optional<geometry::SweepSource> observed =
        current == document_->sweep_sources().end()
            ? std::nullopt : std::optional<geometry::SweepSource>{current->second};
    const auto same_source = [](const auto& left, const auto& right) {
        return left.has_value() == right.has_value() &&
            (!left.has_value() || left->serialize() == right->serialize());
    };
    if (!same_source(observed, expected_current_source_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep source changed before the command was applied"));
    }

    project::ProjectDocument candidate(*document_);
    const auto lineage = candidate.sweep_source_lineage_.find(
        authored_source_.source_identity);
    if (lineage != candidate.sweep_source_lineage_.end() && lineage->second.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "sweep source revision space is exhausted"));
    }
    const core::Revision next_revision = lineage == candidate.sweep_source_lineage_.end()
        ? core::Revision{1U} : lineage->second.next();
    if (!initialized_ && authored_source_.source_revision != next_revision) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep source revision does not match retained project lineage"));
    }
    geometry::SweepSource authored = authored_source_;
    authored.source_revision = next_revision;
    if (auto result = candidate.set_sweep_source(authored); !result) return result;
    if (auto result = candidate.validate(); !result) return result;

    document_->swap(candidate);
    expected_project_revision_ = document_->revision();
    expected_current_source_ = std::move(authored);
    initialized_ = true;
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectSweepSourceCommand::undo() {
    if (!document_ || !applied_ ||
        document_->revision() != expected_project_revision_ ||
        !expected_current_source_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep source command cannot be undone in the current project revision"));
    }
    const auto current = document_->sweep_sources().find(
        expected_current_source_->source_identity);
    if (current == document_->sweep_sources().end() ||
        current->second.serialize() != expected_current_source_->serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sweep source no longer matches the command being undone"));
    }

    project::ProjectDocument candidate(*document_);
    if (previous_source_.has_value()) {
        const auto lineage = candidate.sweep_source_lineage_.find(
            previous_source_->source_identity);
        if (lineage == candidate.sweep_source_lineage_.end() || lineage->second.exhausted()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state, "sweep source revision space is exhausted"));
        }
        geometry::SweepSource restored = *previous_source_;
        restored.source_revision = lineage->second.next();
        if (auto result = candidate.set_sweep_source(restored); !result) return result;
        expected_current_source_ = std::move(restored);
    } else {
        if (auto result = candidate.erase_sweep_source_if_revision(
                expected_current_source_->source_identity,
                expected_current_source_->source_revision); !result) {
            return result;
        }
        expected_current_source_.reset();
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectBiologicalAuthoringCommand::SetProjectBiologicalAuthoringCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    biological::BiologicalAuthoringDocument source)
    : document_(&document),
      authored_source_(std::move(source)),
      key_{authored_source_.source.source_namespace, authored_source_.source.asset_id},
      expected_project_revision_(document.revision()) {
    const auto existing = document.biological_authoring_sources().find(key_);
    if (existing != document.biological_authoring_sources().end()) {
        previous_source_ = existing->second;
        expected_current_source_ = existing->second;
    }
}

core::Result<void> SetProjectBiologicalAuthoringCommand::execute() {
    if (!document_ || applied_ || document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological authoring command is stale or already applied"));
    }
    if (auto result = authored_source_.validate(); !result) return result;
    const auto current = document_->biological_authoring_sources().find(key_);
    const std::optional<project::SerializedBiologicalAuthoring> observed =
        current == document_->biological_authoring_sources().end()
            ? std::nullopt
            : std::optional<project::SerializedBiologicalAuthoring>{current->second};
    if (observed != expected_current_source_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological source changed before the command was applied"));
    }

    project::ProjectDocument candidate(*document_);
    biological::BiologicalAuthoringDocument authored = authored_source_;
    if (initialized_) {
        const auto lineage = candidate.biological_authoring_lineage_.find(key_);
        if (lineage == candidate.biological_authoring_lineage_.end() ||
            lineage->second.exhausted()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "biological source revision lineage cannot advance for redo"));
        }
        authored.source.revision = lineage->second.next();
    }
    if (auto result = candidate.set_biological_authoring(authored); !result) return result;
    if (auto result = candidate.validate(); !result) return result;

    document_->swap(candidate);
    const auto& stored = document_->biological_authoring_sources().at(key_);
    expected_current_source_ = stored;
    expected_project_revision_ = document_->revision();
    initialized_ = true;
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectBiologicalAuthoringCommand::undo() {
    if (!document_ || !applied_ ||
        document_->revision() != expected_project_revision_ ||
        !expected_current_source_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological authoring command cannot be undone in the current project state"));
    }
    const auto current = document_->biological_authoring_sources().find(key_);
    if (current == document_->biological_authoring_sources().end() ||
        current->second != *expected_current_source_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "biological source no longer matches the command being undone"));
    }

    project::ProjectDocument candidate(*document_);
    if (previous_source_.has_value()) {
        auto restored = biological::BiologicalAuthoringDocument::deserialize(
            previous_source_->bytes);
        if (!restored) return core::Result<void>::failure(restored.error());
        const auto lineage = candidate.biological_authoring_lineage_.find(key_);
        if (lineage == candidate.biological_authoring_lineage_.end() ||
            lineage->second.exhausted()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "biological source revision lineage cannot advance for undo"));
        }
        restored.value().source.revision = lineage->second.next();
        if (auto result = candidate.set_biological_authoring(restored.value()); !result) {
            return result;
        }
        expected_current_source_ = candidate.biological_authoring_sources().at(key_);
    } else {
        auto current_source = biological::BiologicalAuthoringDocument::deserialize(
            current->second.bytes);
        if (!current_source) return core::Result<void>::failure(current_source.error());
        if (auto result = candidate.erase_biological_authoring_if_revision(
                key_.first, key_.second, current_source.value().source.revision); !result) {
            return result;
        }
        expected_current_source_.reset();
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetProjectSignTextSourceCommand::SetProjectSignTextSourceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    geometry::SignTextSource source)
    : document_(&document),
      authored_source_(std::move(source)),
      expected_project_revision_(document.revision()) {
    const auto existing = document.sign_text_sources().find(authored_source_.source_identity);
    if (existing != document.sign_text_sources().end()) {
        previous_source_ = existing->second;
        expected_current_source_ = existing->second;
    }
}

core::Result<void> SetProjectSignTextSourceCommand::execute() {
    if (!document_ || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "sign-text source command has incomplete or already-applied state"));
    }
    if (document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign-text source command project revision is stale"));
    }
    const auto current = document_->sign_text_sources().find(authored_source_.source_identity);
    const std::optional<geometry::SignTextSource> observed =
        current == document_->sign_text_sources().end()
            ? std::nullopt : std::optional<geometry::SignTextSource>{current->second};
    const auto same_source = [](const auto& left, const auto& right) {
        return left.has_value() == right.has_value() &&
            (!left.has_value() || left->serialize() == right->serialize());
    };
    if (!same_source(observed, expected_current_source_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign-text source changed before the command was applied"));
    }
    project::ProjectDocument candidate(*document_);
    const auto lineage = candidate.sign_text_source_lineage_.find(
        authored_source_.source_identity);
    if (lineage != candidate.sign_text_source_lineage_.end() && lineage->second.exhausted()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "sign-text source revision space is exhausted"));
    }
    const core::Revision next_revision = lineage == candidate.sign_text_source_lineage_.end()
        ? core::Revision{1U} : lineage->second.next();
    if (!initialized_ && authored_source_.source_revision != next_revision) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign-text source revision does not match retained project lineage"));
    }
    geometry::SignTextSource authored = authored_source_;
    authored.source_revision = next_revision;
    if (auto result = candidate.set_sign_text_source(authored); !result) return result;
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    expected_project_revision_ = document_->revision();
    expected_current_source_ = std::move(authored);
    initialized_ = true;
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectSignTextSourceCommand::undo() {
    if (!document_ || !applied_ ||
        document_->revision() != expected_project_revision_ ||
        !expected_current_source_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign-text source command cannot be undone in the current project revision"));
    }
    const auto current = document_->sign_text_sources().find(
        expected_current_source_->source_identity);
    if (current == document_->sign_text_sources().end() ||
        current->second.serialize() != expected_current_source_->serialize()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "sign-text source no longer matches the command being undone"));
    }
    project::ProjectDocument candidate(*document_);
    if (previous_source_.has_value()) {
        const auto lineage = candidate.sign_text_source_lineage_.find(
            previous_source_->source_identity);
        if (lineage == candidate.sign_text_source_lineage_.end() || lineage->second.exhausted()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "sign-text source revision space is exhausted"));
        }
        geometry::SignTextSource restored = *previous_source_;
        restored.source_revision = lineage->second.next();
        if (auto result = candidate.set_sign_text_source(restored); !result) return result;
        expected_current_source_ = std::move(restored);
    } else {
        if (auto result = candidate.erase_sign_text_source_if_revision(
                expected_current_source_->source_identity,
                expected_current_source_->source_revision); !result) {
            return result;
        }
        expected_current_source_.reset();
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SetVertexPositionCommand::SetVertexPositionCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    core::Vec3d before,
    core::Vec3d after)
    : mesh_(&mesh), vertex_(vertex), before_(before), after_(after) {}

core::Result<void> SetVertexPositionCommand::execute() { return apply(after_); }

core::Result<void> SetVertexPositionCommand::undo() { return apply(before_); }

core::Result<void> SetVertexPositionCommand::apply(core::Vec3d position) {
    if (!mesh_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "vertex command has no mesh"));
    }
    const auto* current = mesh_->find_vertex(vertex_);
    if (!current) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "vertex command references a missing mesh vertex"));
    }
    return mesh_->apply_patch(geometry::MeshPatch{
        mesh_->revision(),
        {geometry::VertexChange{vertex_, current->position, position}},
    });
}

SetProjectSelectedVertexPositionCommand::SetProjectSelectedVertexPositionCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    core::Vec3d position)
    : document_(&document), selection_(&selection), position_(position) {}

core::Result<void> SetProjectSelectedVertexPositionCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto current = candidate.meshes().find(edit.mesh_asset);
            if (current == candidate.meshes().end() ||
                current->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project vertex patch redo no longer matches its source revision"));
            }
            geometry::MeshPatch redo_patch = edit.patch;
            redo_patch.expected_revision = current->second.revision();
            auto result = candidate.apply_mesh_patch(edit.mesh_asset, redo_patch);
            if (!result) return core::Result<void>::failure(result.error());
            edit.patch = std::move(redo_patch);
            edit.current_mesh_revision = result.value();
            edit.after = candidate.meshes().at(edit.mesh_asset);
        }
        if (auto validation = candidate.validate(); !validation) return validation;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex editing requires vertex selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex editing requires an object-bound vertex selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.vertices.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                group.vertices.empty() ? core::ErrorCode::invalid_state
                                       : core::ErrorCode::unsupported,
                "project vertex editing requires exactly one selected vertex per object"));
        }
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected vertex owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object vertex editing refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    if (groups.size() > 1U) {
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object vertex editing refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        const auto* vertex = before.find_vertex(group.vertices.front());
        if (!vertex) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project selected vertex disappeared before patch admission"));
        }
        geometry::MeshPatch patch{
            before.revision(),
            {geometry::VertexChange{group.vertices.front(), vertex->position, position_}},
        };
        auto replaced = candidate.apply_mesh_patch(mesh_asset, patch);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            std::move(before),
            candidate.meshes().at(mesh_asset),
            std::move(patch),
            replaced.value(),
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> SetProjectSelectedVertexPositionCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    for (auto& edit : staged_edits) {
        const auto current = candidate.meshes().find(edit.mesh_asset);
        if (current == candidate.meshes().end() ||
            current->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project vertex patch undo no longer matches its source revision"));
        }
        auto result = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!result) return core::Result<void>::failure(result.error());
        edit.current_mesh_revision = result.value();
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

SetProjectMeshCommand::SetProjectMeshCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::uint64_t mesh_asset,
    core::Revision expected_project_revision,
    core::Revision expected_mesh_revision,
    geometry::EditableMesh after)
    : document_(&document),
      mesh_asset_(mesh_asset),
      expected_project_revision_(expected_project_revision),
      expected_mesh_revision_(expected_mesh_revision),
      after_(std::move(after)) {}

core::Result<void> SetProjectMeshCommand::execute() {
    if (!document_ || mesh_asset_ == 0U || applied_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project mesh command has incomplete or already-applied state"));
    }
    const core::Revision expected_project =
        current_project_revision_.value_or(expected_project_revision_);
    const core::Revision expected_mesh =
        current_mesh_revision_.value_or(expected_mesh_revision_);
    if (document_->revision() != expected_project) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project mesh command revision no longer matches the project"));
    }
    const auto current = document_->meshes().find(mesh_asset_);
    if (current == document_->meshes().end() || current->second.revision() != expected_mesh) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project mesh command no longer matches its source mesh revision"));
    }
    if (!before_.has_value()) before_ = current->second;
    const auto replaced = document_->replace_mesh_if_revision(
        mesh_asset_, expected_mesh, after_);
    if (!replaced) return core::Result<void>::failure(replaced.error());
    current_mesh_revision_ = replaced.value();
    current_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> SetProjectMeshCommand::undo() {
    if (!document_ || !applied_ || !before_.has_value() ||
        !current_project_revision_.has_value() || !current_mesh_revision_.has_value() ||
        document_->revision() != *current_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project mesh no longer matches the command being undone"));
    }
    const auto replaced = document_->replace_mesh_if_revision(
        mesh_asset_, *current_mesh_revision_, *before_);
    if (!replaced) return core::Result<void>::failure(replaced.error());
    current_mesh_revision_ = replaced.value();
    current_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

SlideVertexCommand::SlideVertexCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    geometry::EdgeId support_edge,
    double factor)
    : mesh_(&mesh), vertex_(vertex), support_edge_(support_edge), factor_(factor) {}

core::Result<void> SlideVertexCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "vertex slide command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "vertex slide command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }
    geometry::EditableMesh before = *mesh_;
    if (auto result = mesh_->slide_vertex(vertex_, support_edge_, factor_); !result) {
        return result;
    }
    before_ = std::move(before);
    after_ = *mesh_;
    return core::Result<void>::success();
}

core::Result<void> SlideVertexCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "vertex slide command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

SlideProjectSelectedVertexCommand::SlideProjectSelectedVertexCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    std::optional<geometry::EdgeId> support_edge,
    double factor)
    : document_(&document),
      selection_(&selection),
      requested_support_edge_(support_edge),
      factor_(factor) {}

core::Result<void> SlideProjectSelectedVertexCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex slide command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project vertex slide redo no longer matches its source revision"));
            }
            auto patch = edit.patch;
            patch.expected_revision = edit.current_mesh_revision;
            auto replaced = candidate.apply_mesh_patch(edit.mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            edit.current_mesh_revision = replaced.value();
            edit.after = candidate.meshes().at(edit.mesh_asset);
        }
        if (auto validation = candidate.validate(); !validation) return validation;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex sliding requires vertex selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex sliding requires an object-bound vertex selection"));
    }
    std::size_t selected_vertex_count = 0U;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.vertices.size() > 64U - selected_vertex_count) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "project vertex sliding is limited to 64 selected vertices"));
        }
        selected_vertex_count += group.vertices.size();
    }
    if (selected_vertex_count == 0U) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex sliding has no selected vertices"));
    }
    if (selected_vertex_count > 1U && requested_support_edge_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "multi-vertex sliding requires automatic owner-local support-edge resolution"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex slide owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected vertex slide owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex slide owner references a missing mesh"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object vertex sliding refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    std::map<std::uint64_t, std::size_t> mesh_owner_counts;
    for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
        if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
    }
    for (const auto& [mesh_asset, selected_owner] : asset_owners) {
        static_cast<void>(selected_owner);
        const auto uses = mesh_owner_counts.find(mesh_asset);
        if (uses == mesh_owner_counts.end() || uses->second != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project vertex sliding refuses mesh assets shared with other objects"));
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex slide owner references a missing mesh"));
        }
        geometry::EditableMesh before = mesh->second;
        geometry::EditableMesh after = before;
        auto selected_vertices = group.vertices;
        std::sort(selected_vertices.begin(), selected_vertices.end());
        const std::set<geometry::VertexId> selected_in_owner(
            selected_vertices.begin(), selected_vertices.end());
        const auto topology = before.topology();
        if (!topology) return core::Result<void>::failure(topology.error());
        for (const geometry::VertexId vertex : selected_vertices) {
            std::optional<geometry::EdgeId> preferred_support;
            std::optional<geometry::EdgeId> fallback_support;
            for (const auto& edge : topology.value().edges) {
                if (edge.first != vertex && edge.second != vertex) continue;
                if (!fallback_support.has_value() || edge.id < *fallback_support) {
                    fallback_support = edge.id;
                }
                const geometry::VertexId other = edge.first == vertex ? edge.second : edge.first;
                if (!selected_in_owner.contains(other) &&
                    (!preferred_support.has_value() || edge.id < *preferred_support)) {
                    preferred_support = edge.id;
                }
            }
            const auto support_edge = requested_support_edge_.has_value()
                ? requested_support_edge_
                : (preferred_support.has_value() ? preferred_support : fallback_support);
            if (!support_edge.has_value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::not_found,
                    "project vertex sliding requires an incident support edge for every selected vertex"));
            }
            if (auto result = after.slide_vertex(vertex, *support_edge, factor_); !result) {
                return result;
            }
        }
        geometry::MeshPatch patch{before.revision(), {}};
        patch.vertex_changes.reserve(selected_vertices.size());
        for (const geometry::VertexId vertex : selected_vertices) {
            const auto* source = before.find_vertex(vertex);
            const auto* target = after.find_vertex(vertex);
            if (!source || !target) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project vertex slide lost a selected vertex while staging its patch"));
            }
            if (source->position.x == target->position.x &&
                source->position.y == target->position.y &&
                source->position.z == target->position.z) {
                continue;
            }
            patch.vertex_changes.push_back(geometry::VertexChange{
                vertex, source->position, target->position});
        }
        if (auto result = patch.validate(); !result) {
            return core::Result<void>::failure(result.error());
        }
        auto replaced = candidate.apply_mesh_patch(mesh_asset, patch);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            std::move(before),
            candidate.meshes().at(mesh_asset),
            std::move(patch),
            replaced.value(),
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> SlideProjectSelectedVertexCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex slide command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    for (auto& edit : staged_edits) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project vertex slide undo no longer matches its source revision"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        edit.current_mesh_revision = replaced.value();
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

ExtrudeFaceCommand::ExtrudeFaceCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    double distance)
    : mesh_(&mesh), face_(face), distance_(distance) {}

core::Result<void> ExtrudeFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "extrude command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "extrude command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    const geometry::EditableMesh before = *mesh_;
    if (auto result = mesh_->extrude_face(face_, distance_); !result) {
        return result;
    }
    before_ = before;
    after_ = *mesh_;
    return core::Result<void>::success();
}

core::Result<void> ExtrudeFaceCommand::undo() {
    if (!mesh_ || !before_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "extrude command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

ExtrudeSelectedFaceCommand::ExtrudeSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double distance)
    : selection_(&selection), mesh_(&mesh), distance_(distance) {}

core::Result<void> ExtrudeSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face extrusion command has incomplete context"));
    }
    if (!before_.has_value()) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face extrusion requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face extrusion requires at least one selected face"));
        }
        geometry::EditableMesh candidate = *mesh_;
        for (const geometry::FaceId face : faces) {
            if (auto result = candidate.extrude_face(face, distance_); !result) {
                return result;
            }
        }
        if (auto result = candidate.validate(); !result) return result;
        const geometry::EditableMesh before = *mesh_;
        if (auto result = mesh_->restore_from(candidate); !result) return result;
        before_ = before;
        after_ = *mesh_;
        return core::Result<void>::success();
    }
    if (!after_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face extrusion has incomplete history state"));
    }
    return mesh_->restore_from(*after_);
}

core::Result<void> ExtrudeSelectedFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face extrusion has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

ExtrudeProjectSelectedFaceCommand::ExtrudeProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double distance)
    : document_(&document), selection_(&selection), distance_(distance) {}

core::Result<void> ExtrudeProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project extrusion command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project extrusion redo no longer matches its source mesh"));
            }
            if (edit.patches.empty()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "project extrusion redo has no saved face patches"));
            }
            for (auto& patch : edit.patches) {
                const core::Revision revision_before = edit.current_mesh_revision;
                patch.expected_revision = revision_before;
                auto result = candidate.apply_face_extrusion_patch(edit.mesh_asset, patch);
                if (!result) return core::Result<void>::failure(result.error());
                const auto& receipts = candidate.topology_receipts();
                if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                    receipts.back().receipt.revision_before != revision_before ||
                    receipts.back().receipt.revision_after != result.value()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "persistent face extrusion redo did not retain its topology receipt"));
                }
                edit.current_mesh_revision = result.value();
                edit.after = candidate.meshes().at(edit.mesh_asset);
                edit.receipt = receipts.back().receipt;
            }
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project extrusion requires face selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project extrusion requires an object-bound face selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project extrusion requires at least one selected face per object"));
        }
        const auto* object = document_->scene().find(group.object);
        if (object == nullptr) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected face owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object face extrusion refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    if (groups.size() > 1U) {
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object face extrusion refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        std::vector<geometry::FaceExtrusionPatch> patches;
        patches.reserve(group.faces.size());
        core::Revision current_mesh_revision = before.revision();
        geometry::TopologyEditReceipt last_receipt;
        for (const geometry::FaceId face : group.faces) {
            const geometry::FaceExtrusionPatch patch{
                current_mesh_revision, face, distance_};
            auto replaced = candidate.apply_face_extrusion_patch(mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != current_mesh_revision ||
                receipts.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent face extrusion did not retain its topology receipt"));
            }
            patches.push_back(patch);
            current_mesh_revision = replaced.value();
            last_receipt = receipts.back().receipt;
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset, std::move(patches), std::move(before),
            candidate.meshes().at(mesh_asset), last_receipt, current_mesh_revision});
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> ExtrudeProjectSelectedFaceCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project extrusion command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    std::vector<core::Revision> revisions(edits_.size());
    for (std::size_t reverse = edits_.size(); reverse > 0U; --reverse) {
        const std::size_t index = reverse - 1U;
        const AppliedMeshEdit& edit = edits_[index];
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project extrusion undo no longer matches its source mesh"));
        }
        auto result = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!result) return core::Result<void>::failure(result.error());
        revisions[index] = result.value();
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    for (std::size_t index = 0U; index < edits_.size(); ++index) {
        edits_[index].current_mesh_revision = revisions[index];
    }
    return core::Result<void>::success();
}

InsetFaceCommand::InsetFaceCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    double distance)
    : mesh_(&mesh), face_(face), distance_(distance) {}

core::Result<void> InsetFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "inset command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "inset command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto inset = mesh_->inset_face(face_, distance_);
    if (!inset) {
        return core::Result<void>::failure(inset.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(inset.value());
    return core::Result<void>::success();
}

core::Result<void> InsetFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "inset command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

InsetSelectedFaceCommand::InsetSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double distance)
    : selection_(&selection), mesh_(&mesh), distance_(distance) {}

core::Result<void> InsetSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face inset command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face inset requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face inset requires one selected face"));
        }
        if (faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-face inset is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<InsetFaceCommand>(*mesh_, faces.front(), distance_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> InsetSelectedFaceCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face inset has no completed operation to undo"));
    }
    return delegate_->undo();
}

InsetProjectSelectedFaceCommand::InsetProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double distance)
    : document_(&document), selection_(&selection), distance_(distance) {}

core::Result<void> InsetProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face inset command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        std::map<std::uint64_t, core::Revision> current_revisions;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project face inset redo no longer matches its source mesh"));
            }
            auto [revision, inserted] = current_revisions.emplace(
                edit.mesh_asset, edit.current_mesh_revision);
            if (inserted && mesh->second.revision() != revision->second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project face inset redo no longer matches its source mesh"));
            }
            if (mesh->second.revision() != revision->second) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project face inset redo patch chain is out of order"));
            }
            auto patch = edit.patch;
            const core::Revision revision_before = revision->second;
            patch.expected_revision = revision_before;
            auto replaced = candidate.apply_face_inset_patch(edit.mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                receipts.back().receipt.revision_before != revision_before ||
                receipts.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent face inset redo did not retain its topology receipt"));
            }
            edit.patch = patch;
            edit.current_mesh_revision = replaced.value();
            edit.after = candidate.meshes().at(edit.mesh_asset);
            edit.receipt = receipts.back().receipt;
            revision->second = replaced.value();
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face inset requires face selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face inset requires an object-bound face selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project face inset requires at least one selected face per object"));
        }
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected face owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object face inset refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    if (groups.size() > 1U) {
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object face inset refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    std::size_t selected_face_count = 0U;
    for (const ComponentSelectionGroup& group : groups) {
        selected_face_count += group.faces.size();
    }
    staged_edits.reserve(selected_face_count);
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        auto faces = group.faces;
        std::sort(faces.begin(), faces.end());
        core::Revision current_mesh_revision = before.revision();
        for (const geometry::FaceId face : faces) {
            const geometry::FaceInsetPatch patch{current_mesh_revision, face, distance_};
            auto replaced = candidate.apply_face_inset_patch(mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != current_mesh_revision ||
                receipts.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent face inset did not retain its topology receipt"));
            }
            current_mesh_revision = replaced.value();
            staged_edits.push_back(AppliedMeshEdit{
                mesh_asset,
                patch,
                before,
                candidate.meshes().at(mesh_asset),
                receipts.back().receipt,
                current_mesh_revision,
            });
        }
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> InsetProjectSelectedFaceCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face inset command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    std::set<std::uint64_t> restored_assets;
    for (auto edit = staged_edits.rbegin(); edit != staged_edits.rend(); ++edit) {
        if (!restored_assets.insert(edit->mesh_asset).second) continue;
        const auto mesh = candidate.meshes().find(edit->mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit->current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project face inset undo no longer matches its source mesh"));
        }
        const auto first_edit = std::find_if(
            staged_edits.begin(), staged_edits.end(), [edit](const AppliedMeshEdit& candidate_edit) {
                return candidate_edit.mesh_asset == edit->mesh_asset;
            });
        if (first_edit == staged_edits.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project face inset undo lost its source snapshot"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            edit->mesh_asset, edit->current_mesh_revision, first_edit->before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        for (auto& asset_edit : staged_edits) {
            if (asset_edit.mesh_asset == edit->mesh_asset) {
                asset_edit.current_mesh_revision = replaced.value();
            }
        }
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

PokeFaceCommand::PokeFaceCommand(geometry::EditableMesh& mesh, geometry::FaceId face)
    : mesh_(&mesh), face_(face) {}

core::Result<void> PokeFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "poke command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "poke command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto poked = mesh_->poke_face(face_);
    if (!poked) {
        return core::Result<void>::failure(poked.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(poked.value());
    return core::Result<void>::success();
}

core::Result<void> PokeFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "poke command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

PokeSelectedFaceCommand::PokeSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> PokeSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face poke command has incomplete context"));
    }
    if (!before_.has_value()) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face poke requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face poke requires at least one selected face"));
        }
        geometry::EditableMesh candidate = *mesh_;
        for (const geometry::FaceId face : faces) {
            auto poked = candidate.poke_face(face);
            if (!poked) return core::Result<void>::failure(poked.error());
        }
        if (auto result = candidate.validate(); !result) return result;
        const geometry::EditableMesh before = *mesh_;
        if (auto result = mesh_->restore_from(candidate); !result) return result;
        before_ = before;
        after_ = *mesh_;
        return core::Result<void>::success();
    }
    if (!after_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face poke has incomplete history state"));
    }
    return mesh_->restore_from(*after_);
}

core::Result<void> PokeSelectedFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face poke has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

PokeProjectSelectedFaceCommand::PokeProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> PokeProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face poke command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project face poke redo no longer matches its source mesh"));
            }
            if (edit.patches.empty()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "project face poke redo has no saved face patches"));
            }
            for (auto& patch : edit.patches) {
                const core::Revision revision_before = edit.current_mesh_revision;
                patch.expected_revision = revision_before;
                auto result = candidate.apply_face_poke_patch(edit.mesh_asset, patch);
                if (!result) return core::Result<void>::failure(result.error());
                const auto& receipts = candidate.topology_receipts();
                if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                    receipts.back().receipt.revision_before != revision_before ||
                    receipts.back().receipt.revision_after != result.value()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "persistent face poke redo did not retain its topology receipt"));
                }
                edit.current_mesh_revision = result.value();
                edit.after = candidate.meshes().at(edit.mesh_asset);
                edit.receipt = receipts.back().receipt;
            }
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face poke requires face selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face poke requires an object-bound face selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project face poke requires at least one selected face per object"));
        }
        const auto* object = document_->scene().find(group.object);
        if (object == nullptr) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected face owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object face poke refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    if (groups.size() > 1U) {
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object face poke refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        std::vector<geometry::FacePokePatch> patches;
        patches.reserve(group.faces.size());
        core::Revision current_mesh_revision = before.revision();
        geometry::TopologyEditReceipt last_receipt;
        for (const geometry::FaceId face : group.faces) {
            const geometry::FacePokePatch patch{current_mesh_revision, face};
            auto replaced = candidate.apply_face_poke_patch(mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != current_mesh_revision ||
                receipts.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent face poke did not retain its topology receipt"));
            }
            patches.push_back(patch);
            current_mesh_revision = replaced.value();
            last_receipt = receipts.back().receipt;
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset, std::move(patches), std::move(before),
            candidate.meshes().at(mesh_asset), last_receipt, current_mesh_revision});
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> PokeProjectSelectedFaceCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face poke command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    std::vector<core::Revision> revisions(edits_.size());
    for (std::size_t reverse = edits_.size(); reverse > 0U; --reverse) {
        const std::size_t index = reverse - 1U;
        const AppliedMeshEdit& edit = edits_[index];
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project face poke undo no longer matches its source mesh"));
        }
        auto result = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!result) return core::Result<void>::failure(result.error());
        revisions[index] = result.value();
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    for (std::size_t index = 0U; index < edits_.size(); ++index) {
        edits_[index].current_mesh_revision = revisions[index];
    }
    return core::Result<void>::success();
}

DeleteFaceCommand::DeleteFaceCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId face,
    bool remove_orphaned_vertices)
    : mesh_(&mesh), face_(face), remove_orphaned_vertices_(remove_orphaned_vertices) {}

core::Result<void> DeleteFaceCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "delete face command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "delete face command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto deleted = mesh_->delete_face(face_, remove_orphaned_vertices_);
    if (!deleted) {
        return core::Result<void>::failure(deleted.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(deleted.value());
    return core::Result<void>::success();
}

core::Result<void> DeleteFaceCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "delete face command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

DeleteSelectedFaceCommand::DeleteSelectedFaceCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    bool remove_orphaned_vertices)
    : selection_(&selection),
      mesh_(&mesh),
      remove_orphaned_vertices_(remove_orphaned_vertices) {}

core::Result<void> DeleteSelectedFaceCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face deletion command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-face deletion requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-face deletion requires one selected face"));
        }
        if (faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-face deletion is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<DeleteFaceCommand>(
            *mesh_, faces.front(), remove_orphaned_vertices_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> DeleteSelectedFaceCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-face deletion has no completed operation to undo"));
    }
    return delegate_->undo();
}

DeleteProjectSelectedFaceCommand::DeleteProjectSelectedFaceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    bool remove_orphaned_vertices)
    : document_(&document),
      selection_(&selection),
      remove_orphaned_vertices_(remove_orphaned_vertices) {}

core::Result<void> DeleteProjectSelectedFaceCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face deletion command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        std::vector<core::Revision> revisions;
        revisions.reserve(edits_.size());
        for (const auto& edit : edits_) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project face deletion redo no longer matches its source mesh"));
            }
            auto patch = edit.patch;
            patch.expected_revision = edit.current_mesh_revision;
            auto result = candidate.apply_face_deletion_patch(edit.mesh_asset, patch);
            if (!result) {
                return core::Result<void>::failure(result.error());
            }
            revisions.push_back(result.value());
        }
        if (auto result = candidate.validate(); !result) {
            return result;
        }
        document_->swap(candidate);
        for (std::size_t index = 0U; index < edits_.size(); ++index) {
            edits_[index].current_mesh_revision = revisions[index];
            edits_[index].patch.expected_revision = revisions[index];
            edits_[index].after = document_->meshes().at(edits_[index].mesh_asset);
        }
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face deletion requires face selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project face deletion requires an object-bound face selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const auto& group : groups) {
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected face owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        if (group.faces.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                group.faces.empty() ? core::ErrorCode::invalid_state
                                    : core::ErrorCode::unsupported,
                group.faces.empty()
                    ? "project face deletion requires one selected face per object"
                    : "multi-face project deletion is not implemented"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object face deletion refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }

    if (groups.size() > 1U) {
        const auto scene_objects = document_->scene().objects_sorted();
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const auto& object : scene_objects) {
            if (object.mesh_asset.has_value()) {
                ++mesh_owner_counts[*object.mesh_asset];
            }
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object face deletion refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const auto& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        const geometry::FaceDeletionPatch patch{
            before.revision(), group.faces.front(), remove_orphaned_vertices_};
        auto replaced = candidate.apply_face_deletion_patch(mesh_asset, patch);
        if (!replaced) {
            return core::Result<void>::failure(replaced.error());
        }
        const auto applied_mesh = candidate.meshes().find(mesh_asset);
        const auto& receipts = candidate.topology_receipts();
        if (applied_mesh == candidate.meshes().end() || receipts.empty() ||
            receipts.back().mesh_asset != mesh_asset ||
            receipts.back().receipt.revision_before != before.revision() ||
            receipts.back().receipt.revision_after != replaced.value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "persistent face deletion did not retain its topology receipt"));
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            std::move(before),
            applied_mesh->second,
            receipts.back().receipt,
            patch,
            replaced.value(),
        });
    }
    if (auto result = candidate.validate(); !result) {
        return result;
    }
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> DeleteProjectSelectedFaceCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project face deletion command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    std::vector<core::Revision> revisions;
    revisions.reserve(edits_.size());
    for (const auto& edit : edits_) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project face deletion undo no longer matches its source mesh"));
        }
        auto result = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!result) {
            return core::Result<void>::failure(result.error());
        }
        revisions.push_back(result.value());
    }
    if (auto result = candidate.validate(); !result) {
        return result;
    }
    document_->swap(candidate);
    for (std::size_t index = 0U; index < edits_.size(); ++index) {
        edits_[index].current_mesh_revision = revisions[index];
    }
    return core::Result<void>::success();
}

SplitEdgeCommand::SplitEdgeCommand(
    geometry::EditableMesh& mesh,
    geometry::EdgeId edge,
    double factor)
    : mesh_(&mesh), edge_(edge), factor_(factor) {}

core::Result<void> SplitEdgeCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "split edge command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "split edge command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto split = mesh_->split_edge(edge_, factor_);
    if (!split) {
        return core::Result<void>::failure(split.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(split.value());
    return core::Result<void>::success();
}

core::Result<void> SplitEdgeCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "split edge command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

SplitSelectedEdgeCommand::SplitSelectedEdgeCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double factor)
    : selection_(&selection), mesh_(&mesh), factor_(factor) {}

core::Result<void> SplitSelectedEdgeCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge split command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::edge) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-edge splitting requires edge selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto edges = selection_->selected_edges();
        if (edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-edge splitting requires one selected edge"));
        }
        if (edges.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-edge splitting is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<SplitEdgeCommand>(*mesh_, edges.front(), factor_);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> SplitSelectedEdgeCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge splitting has no completed operation to undo"));
    }
    return delegate_->undo();
}

SplitProjectSelectedEdgeCommand::SplitProjectSelectedEdgeCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double factor)
    : document_(&document), selection_(&selection), factor_(factor) {}

core::Result<void> SplitProjectSelectedEdgeCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge split command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto source = candidate.meshes().find(edit.mesh_asset);
            if (source == candidate.meshes().end() ||
                source->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project edge split redo no longer matches its source mesh"));
            }
            const auto source_revision = source->second.revision();
            auto patch = edit.patch;
            patch.expected_revision = source_revision;
            auto result = candidate.apply_edge_split_patch(edit.mesh_asset, patch);
            if (!result) return core::Result<void>::failure(result.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                receipts.back().receipt.revision_before != source_revision ||
                receipts.back().receipt.revision_after != result.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent edge split did not retain its topology receipt"));
            }
            edit.patch = patch;
            edit.current_mesh_revision = result.value();
            edit.after = candidate.meshes().at(edit.mesh_asset);
            edit.receipt = receipts.back().receipt;
        }
        if (auto validation = candidate.validate(); !validation) {
            return validation;
        }
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::edge) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge splitting requires edge selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge splitting requires object-bound edge selections"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const auto& group : groups) {
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected edge owner has no mesh asset"));
        }
        const auto mesh = document_->meshes().find(*object->mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner references a missing mesh asset"));
        }
        if (group.edges.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                group.edges.empty() ? core::ErrorCode::invalid_state
                                    : core::ErrorCode::unsupported,
                group.edges.empty()
                    ? "project edge splitting requires one selected edge per object"
                    : "multi-edge project splitting is not implemented"));
        }
        if (!asset_owners.emplace(*object->mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object edge splitting refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }

    std::map<std::uint64_t, std::size_t> mesh_owner_counts;
    for (const auto& object : document_->scene().objects_sorted()) {
        if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
    }
    for (const auto& [mesh_asset, selected_owner] : asset_owners) {
        static_cast<void>(selected_owner);
        const auto uses = mesh_owner_counts.find(mesh_asset);
        if (uses == mesh_owner_counts.end() || uses->second != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project edge splitting refuses mesh assets shared by other objects"));
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const auto& group : groups) {
        const auto* object = candidate.scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        const geometry::EdgeSplitPatch patch{
            before.revision(), group.edges.front(), factor_};
        auto replaced = candidate.apply_edge_split_patch(mesh_asset, patch);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        const auto applied_mesh = candidate.meshes().find(mesh_asset);
        const auto& receipts = candidate.topology_receipts();
        if (applied_mesh == candidate.meshes().end() || receipts.empty() ||
            receipts.back().mesh_asset != mesh_asset ||
            receipts.back().receipt.revision_before != before.revision() ||
            receipts.back().receipt.revision_after != replaced.value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "persistent edge split did not retain its topology receipt"));
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            patch,
            std::move(before),
            applied_mesh->second,
            receipts.back().receipt,
            replaced.value(),
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> SplitProjectSelectedEdgeCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge split command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    std::vector<core::Revision> revisions;
    revisions.reserve(edits_.size());
    for (const auto& edit : edits_) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project edge split undo no longer matches its source mesh"));
        }
        auto result = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!result) return core::Result<void>::failure(result.error());
        revisions.push_back(result.value());
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    for (std::size_t index = 0U; index < edits_.size(); ++index) {
        edits_[index].current_mesh_revision = revisions[index];
    }
    return core::Result<void>::success();
}

BevelEdgeCommand::BevelEdgeCommand(
    geometry::EditableMesh& mesh,
    geometry::EdgeId edge,
    double width,
    std::uint32_t segments)
    : mesh_(&mesh), edge_(edge), width_(width), segments_(segments) {}

core::Result<void> BevelEdgeCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "bevel edge command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "bevel edge command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto beveled = mesh_->bevel_edge(edge_, width_, segments_);
    if (!beveled) {
        return core::Result<void>::failure(beveled.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(beveled.value());
    return core::Result<void>::success();
}

core::Result<void> BevelEdgeCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "bevel edge command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

BevelSelectedEdgeCommand::BevelSelectedEdgeCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double width,
    std::uint32_t segments)
    : selection_(&selection), mesh_(&mesh), width_(width), segments_(segments) {}

core::Result<void> BevelSelectedEdgeCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge bevel command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::edge) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-edge bevel requires edge selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto edges = selection_->selected_edges();
        if (edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-edge bevel requires one selected edge"));
        }
        if (edges.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-edge bevel is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<BevelEdgeCommand>(
            *mesh_, edges.front(), width_, segments_);
        auto result = delegate_->execute();
        if (!result) delegate_.reset();
        return result;
    }
    return delegate_->execute();
}

core::Result<void> BevelSelectedEdgeCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge bevel has no completed operation to undo"));
    }
    return delegate_->undo();
}

BevelProjectSelectedEdgeCommand::BevelProjectSelectedEdgeCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double width,
    std::uint32_t segments)
    : document_(&document), selection_(&selection), width_(width), segments_(segments) {}

core::Result<void> BevelProjectSelectedEdgeCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge bevel command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project edge bevel redo no longer matches its source mesh"));
            }
            if (edit.patches.empty()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "project edge bevel redo has no saved patches"));
            }
            for (auto& patch : edit.patches) {
                const core::Revision revision_before = edit.current_mesh_revision;
                patch.expected_revision = revision_before;
                auto replaced = candidate.apply_bevel_edge_patch(edit.mesh_asset, patch);
                if (!replaced) return core::Result<void>::failure(replaced.error());
                const auto& receipts = candidate.topology_receipts();
                if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                    receipts.back().receipt.revision_before != revision_before ||
                    receipts.back().receipt.revision_after != replaced.value()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "persistent edge bevel redo did not retain its topology receipt"));
                }
                edit.current_mesh_revision = replaced.value();
                edit.after = candidate.meshes().at(edit.mesh_asset);
                edit.receipt = receipts.back().receipt;
            }
        }
        if (auto validation = candidate.validate(); !validation) return validation;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::edge) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge bevel requires edge selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge bevel requires object-bound edge selections"));
    }
    constexpr std::size_t kMaxProjectEdgeBevelSelection = 64U;
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    std::size_t selected_edge_count = 0U;
    for (const auto& group : groups) {
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected edge owner has no mesh asset"));
        }
        const auto mesh = document_->meshes().find(*object->mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner references a missing mesh asset"));
        }
        if (group.edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project edge bevel requires at least one selected edge per object"));
        }
        if (group.edges.size() > kMaxProjectEdgeBevelSelection - selected_edge_count) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "project edge bevel selection exceeds the 64-edge command limit"));
        }
        selected_edge_count += group.edges.size();
        if (!asset_owners.emplace(*object->mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object edge bevel refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }

    std::map<std::uint64_t, std::size_t> mesh_owner_counts;
    for (const auto& object : document_->scene().objects_sorted()) {
        if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
    }
    for (const auto& [mesh_asset, selected_owner] : asset_owners) {
        static_cast<void>(selected_owner);
        const auto uses = mesh_owner_counts.find(mesh_asset);
        if (uses == mesh_owner_counts.end() || uses->second != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project edge bevel refuses mesh assets shared by other objects"));
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const auto& group : groups) {
        const auto* object = candidate.scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        auto edges = group.edges;
        std::sort(edges.begin(), edges.end());
        if (std::adjacent_find(edges.begin(), edges.end()) != edges.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "project edge bevel selection contains duplicate edge identities"));
        }
        std::vector<geometry::BevelEdgePatch> patches;
        patches.reserve(edges.size());
        core::Revision current_mesh_revision = before.revision();
        geometry::TopologyEditReceipt last_receipt;
        for (const geometry::EdgeId edge : edges) {
            const geometry::BevelEdgePatch patch{
                current_mesh_revision, edge, width_, segments_};
            auto replaced = candidate.apply_bevel_edge_patch(mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != current_mesh_revision ||
                receipts.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent edge bevel did not retain its topology receipt"));
            }
            patches.push_back(patch);
            current_mesh_revision = replaced.value();
            last_receipt = receipts.back().receipt;
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            std::move(patches),
            std::move(before),
            candidate.meshes().at(mesh_asset),
            last_receipt,
            current_mesh_revision,
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> BevelProjectSelectedEdgeCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge bevel command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    for (auto& edit : staged_edits) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project edge bevel undo no longer matches its source mesh"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        edit.current_mesh_revision = replaced.value();
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

BevelVertexCommand::BevelVertexCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId vertex,
    double width,
    std::uint32_t segments)
    : mesh_(&mesh), vertex_(vertex), width_(width), segments_(segments) {}

core::Result<void> BevelVertexCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "bevel vertex command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "bevel vertex command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto beveled = mesh_->bevel_vertex(vertex_, width_, segments_);
    if (!beveled) {
        return core::Result<void>::failure(beveled.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(beveled.value());
    return core::Result<void>::success();
}

core::Result<void> BevelVertexCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "bevel vertex command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

BevelSelectedVertexCommand::BevelSelectedVertexCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    double width,
    std::uint32_t segments)
    : selection_(&selection), mesh_(&mesh), width_(width), segments_(segments) {}

core::Result<void> BevelSelectedVertexCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-vertex bevel command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::vertex) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-vertex bevel requires vertex selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto vertices = selection_->selected_vertices();
        if (vertices.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-vertex bevel requires one selected vertex"));
        }
        if (vertices.size() != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-vertex bevel is not implemented in this tool boundary"));
        }
        delegate_ = std::make_unique<BevelVertexCommand>(
            *mesh_, vertices.front(), width_, segments_);
        auto result = delegate_->execute();
        if (!result) delegate_.reset();
        return result;
    }
    return delegate_->execute();
}

core::Result<void> BevelSelectedVertexCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-vertex bevel has no completed operation to undo"));
    }
    return delegate_->undo();
}

BevelProjectSelectedVertexCommand::BevelProjectSelectedVertexCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    double width,
    std::uint32_t segments)
    : document_(&document), selection_(&selection), width_(width), segments_(segments) {}

core::Result<void> BevelProjectSelectedVertexCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex bevel command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project vertex bevel redo no longer matches its source mesh"));
            }
            if (edit.patches.empty()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "project vertex bevel redo has no selected vertex patches"));
            }
            for (auto& patch : edit.patches) {
                patch.expected_revision = edit.current_mesh_revision;
                auto replaced = candidate.apply_bevel_vertex_patch(edit.mesh_asset, patch);
                if (!replaced) return core::Result<void>::failure(replaced.error());
                const auto& receipts = candidate.topology_receipts();
                if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                    receipts.back().receipt.revision_before != edit.current_mesh_revision ||
                    receipts.back().receipt.revision_after != replaced.value()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "persistent vertex bevel redo did not retain its topology receipt"));
                }
                edit.current_mesh_revision = replaced.value();
                edit.receipt = receipts.back().receipt;
            }
            edit.after = candidate.meshes().at(edit.mesh_asset);
        }
        if (auto validation = candidate.validate(); !validation) return validation;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex bevel requires vertex selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex bevel requires an object-bound vertex selection"));
    }
    constexpr std::size_t kMaxProjectVertexBevelSelection = 64U;
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    std::size_t selected_vertex_count = 0U;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.vertices.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project vertex bevel requires at least one selected vertex per object"));
        }
        if (group.vertices.size() >
            kMaxProjectVertexBevelSelection - selected_vertex_count) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "project vertex bevel selection exceeds the 64-vertex command limit"));
        }
        selected_vertex_count += group.vertices.size();
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected vertex owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object vertex bevel refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    std::map<std::uint64_t, std::size_t> mesh_owner_counts;
    for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
        if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
    }
    for (const auto& [mesh_asset, selected_owner] : asset_owners) {
        static_cast<void>(selected_owner);
        const auto uses = mesh_owner_counts.find(mesh_asset);
        if (uses == mesh_owner_counts.end() || uses->second != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project vertex bevel refuses mesh assets shared by other objects"));
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        auto vertices = group.vertices;
        std::sort(vertices.begin(), vertices.end());
        if (std::adjacent_find(vertices.begin(), vertices.end()) != vertices.end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "project vertex bevel selection contains duplicate vertex identities"));
        }
        std::vector<geometry::BevelVertexPatch> patches;
        patches.reserve(vertices.size());
        core::Revision current_mesh_revision = before.revision();
        geometry::TopologyEditReceipt last_receipt;
        for (const geometry::VertexId vertex : vertices) {
            const geometry::BevelVertexPatch patch{
                current_mesh_revision, vertex, width_, segments_};
            auto replaced = candidate.apply_bevel_vertex_patch(mesh_asset, patch);
            if (!replaced) return core::Result<void>::failure(replaced.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != current_mesh_revision ||
                receipts.back().receipt.revision_after != replaced.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent vertex bevel did not retain its topology receipt"));
            }
            patches.push_back(patch);
            current_mesh_revision = replaced.value();
            last_receipt = receipts.back().receipt;
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            std::move(patches),
            std::move(before),
            candidate.meshes().at(mesh_asset),
            last_receipt,
            current_mesh_revision,
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> BevelProjectSelectedVertexCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex bevel command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    for (auto& edit : staged_edits) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project vertex bevel undo no longer matches its source mesh"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        edit.current_mesh_revision = replaced.value();
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

DissolveEdgeCommand::DissolveEdgeCommand(
    geometry::EditableMesh& mesh,
    geometry::EdgeId edge)
    : mesh_(&mesh), edges_{edge} {}

DissolveEdgeCommand::DissolveEdgeCommand(
    geometry::EditableMesh& mesh,
    std::vector<geometry::EdgeId> edges)
    : mesh_(&mesh), edges_(std::move(edges)) {}

core::Result<void> DissolveEdgeCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "dissolve edge command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "dissolve edge command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto dissolved = mesh_->dissolve_edges(edges_);
    if (!dissolved) {
        return core::Result<void>::failure(dissolved.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(dissolved.value());
    return core::Result<void>::success();
}

core::Result<void> DissolveEdgeCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "dissolve edge command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

DissolveSelectedEdgeCommand::DissolveSelectedEdgeCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> DissolveSelectedEdgeCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge dissolve command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::edge) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-edge dissolve requires edge selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto edges = selection_->selected_edges();
        if (edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected-edge dissolve requires one selected edge"));
        }
        delegate_ = std::make_unique<DissolveEdgeCommand>(*mesh_, edges);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> DissolveSelectedEdgeCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-edge dissolve has no completed operation to undo"));
    }
    return delegate_->undo();
}

DissolveProjectSelectedEdgeCommand::DissolveProjectSelectedEdgeCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> DissolveProjectSelectedEdgeCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge dissolve command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project edge dissolve redo no longer matches its source mesh"));
            }
            auto patch = edit.patch;
            patch.expected_revision = edit.current_mesh_revision;
            auto applied = candidate.apply_edge_dissolve_patch(edit.mesh_asset, patch);
            if (!applied) return core::Result<void>::failure(applied.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                receipts.back().receipt.revision_before != edit.current_mesh_revision ||
                receipts.back().receipt.revision_after != applied.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent edge dissolve redo did not retain its topology receipt"));
            }
            edit.patch = std::move(patch);
            edit.current_mesh_revision = applied.value();
            edit.after = candidate.meshes().at(edit.mesh_asset);
            edit.receipt = receipts.back().receipt;
        }
        if (auto result = candidate.validate(); !result) return result;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::edge) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge dissolve requires edge selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project edge dissolve requires object-bound edge selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        if (group.edges.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project edge dissolve requires at least one selected edge per object"));
        }
        const auto* object = document_->scene().find(group.object);
        if (object == nullptr) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected edge owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object edge dissolve refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    if (groups.size() > 1U) {
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object edge dissolve refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected edge owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        const geometry::EdgeDissolvePatch patch{before.revision(), group.edges};
        auto applied = candidate.apply_edge_dissolve_patch(mesh_asset, patch);
        if (!applied) return core::Result<void>::failure(applied.error());
        const auto& receipts = candidate.topology_receipts();
        if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
            receipts.back().receipt.revision_before != before.revision() ||
            receipts.back().receipt.revision_after != applied.value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "persistent edge dissolve did not retain its topology receipt"));
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset, patch, std::move(before), candidate.meshes().at(mesh_asset),
            receipts.back().receipt, applied.value()});
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> DissolveProjectSelectedEdgeCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project edge dissolve command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    std::vector<core::Revision> revisions(edits_.size());
    for (std::size_t reverse = edits_.size(); reverse > 0U; --reverse) {
        const std::size_t index = reverse - 1U;
        const AppliedMeshEdit& edit = edits_[index];
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project edge dissolve undo no longer matches its source mesh"));
        }
        auto result = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!result) return core::Result<void>::failure(result.error());
        revisions[index] = result.value();
    }
    if (auto result = candidate.validate(); !result) return result;
    document_->swap(candidate);
    for (std::size_t index = 0U; index < edits_.size(); ++index) {
        edits_[index].current_mesh_revision = revisions[index];
    }
    return core::Result<void>::success();
}

TriToQuadCommand::TriToQuadCommand(
    geometry::EditableMesh& mesh,
    geometry::FaceId first,
    geometry::FaceId second)
    : mesh_(&mesh), first_(first), second_(second) {}

core::Result<void> TriToQuadCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "triangle-to-quad command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "triangle-to-quad command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto converted = mesh_->tri_to_quad(first_, second_);
    if (!converted) {
        return core::Result<void>::failure(converted.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(converted.value());
    return core::Result<void>::success();
}

core::Result<void> TriToQuadCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "triangle-to-quad command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

TriToQuadSelectedFacesCommand::TriToQuadSelectedFacesCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh)
    : selection_(&selection), mesh_(&mesh) {}

core::Result<void> TriToQuadSelectedFacesCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected triangle-to-quad command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::face) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "triangle-to-quad conversion requires face selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto faces = selection_->selected_faces();
        if (faces.size() != 2U) {
            return core::Result<void>::failure(core::Diagnostic(
                faces.empty() ? core::ErrorCode::invalid_state : core::ErrorCode::unsupported,
                "triangle-to-quad conversion requires exactly two selected faces"));
        }
        delegate_ = std::make_unique<TriToQuadCommand>(*mesh_, faces[0], faces[1]);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> TriToQuadSelectedFacesCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected triangle-to-quad conversion has no completed operation to undo"));
    }
    return delegate_->undo();
}

TriToQuadProjectSelectedFacesCommand::TriToQuadProjectSelectedFacesCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection)
    : document_(&document), selection_(&selection) {}

core::Result<void> TriToQuadProjectSelectedFacesCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project triangle-to-quad command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project triangle-to-quad redo no longer matches its source mesh"));
            }
            auto patch = edit.patch;
            patch.expected_revision = edit.current_mesh_revision;
            auto applied = candidate.apply_tri_to_quad_patch(edit.mesh_asset, patch);
            if (!applied) return core::Result<void>::failure(applied.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                receipts.back().receipt.revision_before != edit.current_mesh_revision ||
                receipts.back().receipt.revision_after != applied.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent triangle-to-quad redo did not retain its topology receipt"));
            }
            edit.patch = patch;
            edit.current_mesh_revision = applied.value();
            edit.after = candidate.meshes().at(edit.mesh_asset);
        }
        if (auto validation = candidate.validate(); !validation) return validation;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::face) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project triangle-to-quad conversion requires face selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project triangle-to-quad conversion requires object-bound face selections"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const auto& group : groups) {
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected face owner has no mesh asset"));
        }
        const auto mesh = document_->meshes().find(*object->mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        if (group.faces.size() != 2U) {
            return core::Result<void>::failure(core::Diagnostic(
                group.faces.empty() ? core::ErrorCode::invalid_state
                                    : core::ErrorCode::unsupported,
                group.faces.empty()
                    ? "project triangle-to-quad conversion requires two faces per object"
                    : "each project owner must select exactly two faces for triangle-to-quad"));
        }
        if (!asset_owners.emplace(*object->mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object triangle-to-quad refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }

    std::map<std::uint64_t, std::size_t> mesh_owner_counts;
    for (const auto& object : document_->scene().objects_sorted()) {
        if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
    }
    for (const auto& [mesh_asset, selected_owner] : asset_owners) {
        static_cast<void>(selected_owner);
        const auto uses = mesh_owner_counts.find(mesh_asset);
        if (uses == mesh_owner_counts.end() || uses->second != 1U) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "project triangle-to-quad refuses mesh assets shared by other objects"));
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const auto& group : groups) {
        const auto* object = candidate.scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected face owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        const geometry::TriToQuadPatch patch{
            before.revision(), group.faces[0], group.faces[1]};
        auto applied = candidate.apply_tri_to_quad_patch(mesh_asset, patch);
        if (!applied) return core::Result<void>::failure(applied.error());
        const auto& receipts = candidate.topology_receipts();
        if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
            receipts.back().receipt.revision_before != before.revision() ||
            receipts.back().receipt.revision_after != applied.value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "persistent triangle-to-quad conversion did not retain its topology receipt"));
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            patch,
            std::move(before),
            candidate.meshes().at(mesh_asset),
            applied.value(),
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> TriToQuadProjectSelectedFacesCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project triangle-to-quad command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    for (auto& edit : staged_edits) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project triangle-to-quad undo no longer matches its source mesh"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        edit.current_mesh_revision = replaced.value();
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

MergeVerticesCommand::MergeVerticesCommand(
    geometry::EditableMesh& mesh,
    geometry::VertexId target,
    geometry::VertexId source,
    geometry::VertexMergeMode mode)
    : mesh_(&mesh), target_(target), source_(source), mode_(mode) {}

core::Result<void> MergeVerticesCommand::execute() {
    if (!mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state, "merge vertices command has no mesh"));
    }
    if (before_.has_value()) {
        if (!after_.has_value() || !receipt_.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "merge vertices command has incomplete history state"));
        }
        return mesh_->restore_from(*after_);
    }

    geometry::EditableMesh before = *mesh_;
    auto merged = mesh_attributes::merge_vertices_with_attribute_transfer(
        *mesh_, target_, source_, mode_);
    if (!merged) {
        return core::Result<void>::failure(merged.error());
    }
    before_ = std::move(before);
    after_ = *mesh_;
    receipt_ = std::move(merged.value());
    return core::Result<void>::success();
}

core::Result<void> MergeVerticesCommand::undo() {
    if (!mesh_ || !before_.has_value() || !after_.has_value() || !receipt_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "merge vertices command has no completed operation to undo"));
    }
    return mesh_->restore_from(*before_);
}

MergeSelectedVerticesCommand::MergeSelectedVerticesCommand(
    const SelectionState& selection,
    geometry::EditableMesh& mesh,
    geometry::VertexMergeMode mode)
    : selection_(&selection), mesh_(&mesh), mode_(mode) {}

core::Result<void> MergeSelectedVerticesCommand::execute() {
    if (!selection_ || !mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-vertex merge command has incomplete context"));
    }
    if (!delegate_) {
        if (selection_->mode() != SelectionMode::vertex) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "selected-vertex merge requires vertex selection mode"));
        }
        if (auto result = selection_->validate(*mesh_); !result) {
            return result;
        }
        const auto vertices = selection_->selected_vertices();
        if (vertices.size() != 2U) {
            return core::Result<void>::failure(core::Diagnostic(
                vertices.empty() ? core::ErrorCode::invalid_state :
                    core::ErrorCode::unsupported,
                "selected-vertex merge requires exactly two selected vertices"));
        }
        geometry::VertexId target = vertices.front();
        geometry::VertexId source = vertices.back();
        geometry::VertexMergeMode resolved_mode = mode_;
        if (mode_ == geometry::VertexMergeMode::last_selected) {
            const auto order = selection_->selected_vertices_in_order();
            if (order.size() != 2U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "last-selected merge requires a complete vertex selection order"));
            }
            target = order.back();
            source = order.front();
            resolved_mode = geometry::VertexMergeMode::target;
        }
        delegate_ = std::make_unique<MergeVerticesCommand>(
            *mesh_, target, source, resolved_mode);
        auto result = delegate_->execute();
        if (!result) {
            delegate_.reset();
        }
        return result;
    }
    return delegate_->execute();
}

core::Result<void> MergeSelectedVerticesCommand::undo() {
    if (!delegate_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "selected-vertex merge has no completed operation to undo"));
    }
    return delegate_->undo();
}

MergeProjectSelectedVerticesCommand::MergeProjectSelectedVerticesCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    const SelectionState& selection,
    geometry::VertexMergeMode mode,
    std::optional<double> merge_distance)
    : document_(&document),
      selection_(&selection),
      mode_(mode),
      merge_distance_(merge_distance) {}

core::Result<void> MergeProjectSelectedVerticesCommand::execute() {
    if (!document_ || !selection_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex merge command has incomplete context"));
    }
    if (!edits_.empty()) {
        project::ProjectDocument candidate = *document_;
        auto staged_edits = edits_;
        for (auto& edit : staged_edits) {
            const auto mesh = candidate.meshes().find(edit.mesh_asset);
            if (mesh == candidate.meshes().end() ||
                mesh->second.revision() != edit.current_mesh_revision) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "project vertex merge redo no longer matches its source mesh"));
            }
            if (edit.merge_by_distance_patch.has_value()) {
                auto patch = *edit.merge_by_distance_patch;
                patch.expected_revision = edit.current_mesh_revision;
                auto applied = candidate.apply_merge_by_distance_patch(
                    edit.mesh_asset, patch);
                if (!applied) return core::Result<void>::failure(applied.error());
                const auto& receipts = candidate.topology_receipts();
                if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                    receipts.back().receipt.revision_before != edit.current_mesh_revision ||
                    receipts.back().receipt.revision_after != applied.value()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "persistent merge-by-distance redo did not retain its topology receipt"));
                }
                edit.merge_by_distance_patch = std::move(patch);
                edit.current_mesh_revision = applied.value();
                edit.after = candidate.meshes().at(edit.mesh_asset);
                edit.receipt = receipts.back().receipt;
            } else if (edit.vertex_merge_patch.has_value()) {
                auto patch = *edit.vertex_merge_patch;
                patch.expected_revision = edit.current_mesh_revision;
                auto applied = candidate.apply_vertex_merge_patch(edit.mesh_asset, patch);
                if (!applied) return core::Result<void>::failure(applied.error());
                const auto& receipts = candidate.topology_receipts();
                if (receipts.empty() || receipts.back().mesh_asset != edit.mesh_asset ||
                    receipts.back().receipt.revision_before != edit.current_mesh_revision ||
                    receipts.back().receipt.revision_after != applied.value()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "persistent vertex merge redo did not retain its topology receipt"));
                }
                edit.vertex_merge_patch = std::move(patch);
                edit.current_mesh_revision = applied.value();
                edit.after = candidate.meshes().at(edit.mesh_asset);
                edit.receipt = receipts.back().receipt;
            } else {
                auto replaced = candidate.replace_mesh_if_revision(
                    edit.mesh_asset, edit.current_mesh_revision, edit.after);
                if (!replaced) return core::Result<void>::failure(replaced.error());
                edit.current_mesh_revision = replaced.value();
                edit.after = candidate.meshes().at(edit.mesh_asset);
            }
        }
        if (auto validation = candidate.validate(); !validation) return validation;
        document_->swap(candidate);
        edits_ = std::move(staged_edits);
        return core::Result<void>::success();
    }
    if (selection_->mode() != SelectionMode::vertex) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex merge requires vertex selection mode"));
    }
    const auto groups = selection_->component_groups();
    if (groups.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "project vertex merge requires an object-bound vertex selection"));
    }
    std::map<scene::ObjectId, const geometry::EditableMesh*> owner_meshes;
    std::map<std::uint64_t, scene::ObjectId> asset_owners;
    for (const ComponentSelectionGroup& group : groups) {
        if (merge_distance_.has_value()) {
            if (group.vertices.size() < 2U) {
                return core::Result<void>::failure(core::Diagnostic(
                    group.vertices.empty() ? core::ErrorCode::invalid_state
                                           : core::ErrorCode::unsupported,
                    "project merge-by-distance requires at least two vertices per object"));
            }
        } else if (group.vertices.size() != 2U) {
            return core::Result<void>::failure(core::Diagnostic(
                group.vertices.empty() ? core::ErrorCode::invalid_state
                                       : core::ErrorCode::unsupported,
                "project vertex merge requires exactly two vertices per object"));
        }
        const auto* object = document_->scene().find(group.object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "selected vertex owner has no mesh asset"));
        }
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = document_->meshes().find(mesh_asset);
        if (mesh == document_->meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner references a missing mesh asset"));
        }
        if (!asset_owners.emplace(mesh_asset, group.object).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::unsupported,
                "multi-object vertex merge refuses shared mesh assets"));
        }
        owner_meshes.emplace(group.object, &mesh->second);
    }
    if (auto result = selection_->validate(document_->scene(), owner_meshes); !result) {
        return result;
    }
    if (groups.size() > 1U) {
        std::map<std::uint64_t, std::size_t> mesh_owner_counts;
        for (const scene::SceneObject& object : document_->scene().objects_sorted()) {
            if (object.mesh_asset.has_value()) ++mesh_owner_counts[*object.mesh_asset];
        }
        for (const auto& [mesh_asset, selected_owner] : asset_owners) {
            static_cast<void>(selected_owner);
            const auto uses = mesh_owner_counts.find(mesh_asset);
            if (uses == mesh_owner_counts.end() || uses->second != 1U) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::unsupported,
                    "multi-object vertex merge refuses mesh assets shared by other objects"));
            }
        }
    }

    project::ProjectDocument candidate = *document_;
    std::vector<AppliedMeshEdit> staged_edits;
    staged_edits.reserve(groups.size());
    for (const ComponentSelectionGroup& group : groups) {
        const auto* object = document_->scene().find(group.object);
        const std::uint64_t mesh_asset = *object->mesh_asset;
        const auto mesh = candidate.meshes().find(mesh_asset);
        if (mesh == candidate.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "selected vertex owner references a missing mesh asset"));
        }
        geometry::EditableMesh before = mesh->second;
        std::optional<geometry::MergeByDistancePatch> merge_by_distance_patch;
        std::optional<geometry::VertexMergePatch> vertex_merge_patch;
        geometry::TopologyEditReceipt receipt;
        core::Revision result_revision;
        if (merge_distance_.has_value()) {
            geometry::MergeByDistancePatch patch{
                before.revision(), group.vertices, *merge_distance_};
            auto applied = candidate.apply_merge_by_distance_patch(mesh_asset, patch);
            if (!applied) return core::Result<void>::failure(applied.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != before.revision() ||
                receipts.back().receipt.revision_after != applied.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent merge-by-distance did not retain its topology receipt"));
            }
            merge_by_distance_patch = std::move(patch);
            receipt = receipts.back().receipt;
            result_revision = applied.value();
        } else {
            geometry::VertexId target = group.vertices.front();
            geometry::VertexId source = group.vertices.back();
            geometry::VertexMergeMode resolved_mode = mode_;
            if (mode_ == geometry::VertexMergeMode::last_selected) {
                if (group.vertex_selection_order.size() != 2U) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "last-selected merge requires a complete per-object selection order"));
                }
                target = group.vertex_selection_order.back();
                source = group.vertex_selection_order.front();
                resolved_mode = geometry::VertexMergeMode::target;
            }
            geometry::VertexMergePatch patch{
                before.revision(), target, source, resolved_mode};
            auto applied = candidate.apply_vertex_merge_patch(mesh_asset, patch);
            if (!applied) return core::Result<void>::failure(applied.error());
            const auto& receipts = candidate.topology_receipts();
            if (receipts.empty() || receipts.back().mesh_asset != mesh_asset ||
                receipts.back().receipt.revision_before != before.revision() ||
                receipts.back().receipt.revision_after != applied.value()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "persistent vertex merge did not retain its topology receipt"));
            }
            vertex_merge_patch = std::move(patch);
            receipt = receipts.back().receipt;
            result_revision = applied.value();
        }
        staged_edits.push_back(AppliedMeshEdit{
            mesh_asset,
            std::move(merge_by_distance_patch),
            std::move(vertex_merge_patch),
            std::move(before),
            candidate.meshes().at(mesh_asset),
            receipt,
            result_revision,
        });
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

core::Result<void> MergeProjectSelectedVerticesCommand::undo() {
    if (!document_ || edits_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "project vertex merge command has no completed operation to undo"));
    }
    project::ProjectDocument candidate = *document_;
    auto staged_edits = edits_;
    for (auto& edit : staged_edits) {
        const auto mesh = candidate.meshes().find(edit.mesh_asset);
        if (mesh == candidate.meshes().end() ||
            mesh->second.revision() != edit.current_mesh_revision) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project vertex merge undo no longer matches its source mesh"));
        }
        auto replaced = candidate.replace_mesh_if_revision(
            edit.mesh_asset, edit.current_mesh_revision, edit.before);
        if (!replaced) return core::Result<void>::failure(replaced.error());
        edit.current_mesh_revision = replaced.value();
    }
    if (auto validation = candidate.validate(); !validation) return validation;
    document_->swap(candidate);
    edits_ = std::move(staged_edits);
    return core::Result<void>::success();
}

CreateMeshObjectCommand::CreateMeshObjectCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::string object_name,
    geometry::EditableMesh mesh,
    core::Transform object_transform,
    std::optional<std::uint64_t> existing_mesh_asset,
    std::optional<project::SignTextInstanceReceipt> sign_text_receipt,
    std::optional<project::RoadSurfaceInstanceReceipt> road_surface_receipt,
    std::optional<project::BiologicalProxyInstanceReceipt> biological_proxy_receipt)
    : document_(&document),
      object_name_(std::move(object_name)),
      mesh_(std::move(mesh)),
      sign_text_receipt_(std::move(sign_text_receipt)),
      road_surface_receipt_(std::move(road_surface_receipt)),
      biological_proxy_receipt_(std::move(biological_proxy_receipt)),
      object_transform_(object_transform),
      existing_mesh_asset_(existing_mesh_asset) {}

core::Result<void> CreateMeshObjectCommand::execute() {
    if (!document_) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "create mesh command has no project"));
    }
    if (mesh_asset_.has_value() || object_.has_value()) {
        return restore_instance();
    }
    return create_first_instance();
}

core::Result<void> CreateMeshObjectCommand::undo() {
    if (!document_ || !mesh_asset_.has_value() || !object_.has_value()) {
        return core::Result<void>::failure(
            core::Diagnostic(core::ErrorCode::invalid_state, "create mesh command has no instance to undo"));
    }
    return remove_instance();
}

core::Result<void> CreateMeshObjectCommand::create_first_instance() {
    const project::ProjectDocument before = *document_;
    const auto rollback = [this, &before](core::Diagnostic diagnostic) {
        *document_ = before;
        mesh_asset_.reset();
        object_.reset();
        biological_proxy_receipt_.reset();
        owns_mesh_asset_ = false;
        return core::Result<void>::failure(std::move(diagnostic));
    };

    if (existing_mesh_asset_.has_value()) {
        if (!document_->meshes().contains(*existing_mesh_asset_)) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "shared mesh asset disappeared before instance creation"));
        }
        mesh_asset_ = *existing_mesh_asset_;
        owns_mesh_asset_ = false;
    } else {
        auto mesh_asset = document_->add_mesh(mesh_);
        if (!mesh_asset) {
            return core::Result<void>::failure(mesh_asset.error());
        }
        mesh_asset_ = mesh_asset.value();
        owns_mesh_asset_ = true;
    }

    auto object = document_->create_object(object_name_, object_transform_);
    if (!object) {
        return rollback(object.error());
    }
    const scene::ObjectId object_id = object.value();
    if (auto attach = document_->attach_mesh(object_id, *mesh_asset_); !attach) {
        return rollback(attach.error());
    }
    const auto* inserted = document_->scene().find(object_id);
    if (!inserted) {
        return rollback(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "created mesh object disappeared before command capture"));
    }
    object_ = *inserted;
    if (sign_text_receipt_.has_value()) {
        sign_text_receipt_->object = object_id;
        sign_text_receipt_->mesh_asset = *mesh_asset_;
        if (auto result = sign_text_receipt_->validate(); !result) {
            return rollback(result.error().with_context("generated sign-text source receipt"));
        }
        if (!document_->sign_text_instance_receipts_.emplace(
                object_id, *sign_text_receipt_).second) {
            return rollback(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "generated sign-text receipt object identity is already present"));
        }
    }
    if (road_surface_receipt_.has_value()) {
        road_surface_receipt_->object = object_id;
        road_surface_receipt_->mesh_asset = *mesh_asset_;
        if (auto result = road_surface_receipt_->validate(); !result) {
            return rollback(result.error().with_context(
                "generated road-surface source receipt"));
        }
        if (!document_->road_surface_instance_receipts_.emplace(
                object_id, *road_surface_receipt_).second) {
            return rollback(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "generated road-surface receipt object identity is already present"));
        }
    }
    if (biological_proxy_receipt_.has_value()) {
        biological_proxy_receipt_->object = object_id;
        biological_proxy_receipt_->mesh_asset = *mesh_asset_;
        if (auto result = biological_proxy_receipt_->validate(); !result) {
            return rollback(result.error().with_context(
                "generated biological proxy source receipt"));
        }
        if (!document_->biological_proxy_instance_receipts_.emplace(
                object_id, *biological_proxy_receipt_).second) {
            return rollback(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "biological proxy receipt object identity is already present"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> CreateMeshObjectCommand::restore_instance() {
    if (!mesh_asset_.has_value() || !object_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "create mesh command has incomplete restoration state"));
    }
    const project::ProjectDocument before = *document_;
    if (owns_mesh_asset_) {
        if (auto result = document_->insert_mesh(*mesh_asset_, mesh_); !result) {
            return result;
        }
    } else if (!document_->meshes().contains(*mesh_asset_)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shared mesh asset is missing during instance restoration"));
    }
    if (auto result = document_->insert_object(*object_); !result) {
        *document_ = before;
        return result;
    }
    if (sign_text_receipt_.has_value() &&
        !document_->sign_text_instance_receipts_.emplace(
            object_->id, *sign_text_receipt_).second) {
        *document_ = before;
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "generated sign-text receipt could not be restored with its scene object"));
    }
    if (road_surface_receipt_.has_value() &&
        !document_->road_surface_instance_receipts_.emplace(
            object_->id, *road_surface_receipt_).second) {
        *document_ = before;
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "generated road-surface receipt could not be restored with its scene object"));
    }
    if (biological_proxy_receipt_.has_value() &&
        !document_->biological_proxy_instance_receipts_.emplace(
            object_->id, *biological_proxy_receipt_).second) {
        *document_ = before;
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "biological proxy receipt could not be restored with its scene object"));
    }
    return core::Result<void>::success();
}

core::Result<void> CreateMeshObjectCommand::remove_instance() {
    const project::ProjectDocument before = *document_;
    const scene::ObjectId object_id = object_->id;
    if (auto result = document_->remove_object(object_id); !result) {
        return result;
    }
    if (owns_mesh_asset_) {
        if (auto result = document_->remove_mesh(*mesh_asset_); !result) {
            *document_ = before;
            return result;
        }
    }
    return core::Result<void>::success();
}

ImportSourceMeshCommand::ImportSourceMeshCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    std::string object_name,
    geometry::EditableMesh mesh,
    assets::SourceImportRecord source_record)
    : document_(&document),
      object_name_(std::move(object_name)),
      imported_mesh_(std::move(mesh)),
      imported_record_(std::move(source_record)),
      expected_project_revision_(document.revision()) {}

core::Result<void> ImportSourceMeshCommand::apply_registry_record(
    std::optional<assets::SourceImportRecord> desired) {
    if (!document_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source mesh command has no project document"));
    }
    const auto& registry = document_->source_asset_registry();
    const std::string identity = desired.has_value()
        ? desired->identity
        : imported_record_.identity;
    std::vector<assets::SourceImportRecord> records;
    records.reserve(registry.records().size() + (desired.has_value() ? 1U : 0U));
    for (const auto& [active_identity, record] : registry.records()) {
        if (active_identity != identity) records.push_back(record);
    }
    if (desired.has_value()) {
        const auto active = registry.records().find(identity);
        const auto lineage = registry.lineage().find(identity);
        if (active != registry.records().end()) {
            if (active->second.kind != desired->kind ||
                active->second.artifact_identity != desired->artifact_identity) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "source reimport cannot change the bound artifact identity"));
            }
            desired->source_generation = active->second.source_generation;
            if (desired->serialize() != active->second.serialize()) {
                if (lineage->second.exhausted()) {
                    return core::Result<void>::failure(core::Diagnostic(
                        core::ErrorCode::invalid_state,
                        "source asset generation space is exhausted"));
                }
                desired->source_generation = lineage->second.next();
            }
        } else if (lineage == registry.lineage().end()) {
            desired->source_generation = core::Revision{1U};
        } else {
            if (lineage->second.exhausted()) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "source asset generation space is exhausted"));
            }
            desired->source_generation = lineage->second.next();
        }
        if (auto result = desired->validate(); !result) return result;
        records.push_back(std::move(*desired));
    }
    const auto plan = assets::plan_source_reimport(registry, std::move(records));
    if (!plan) return core::Result<void>::failure(plan.error());
    return document_->apply_source_reimport_plan(plan.value());
}

core::Result<void> ImportSourceMeshCommand::apply_imported_mesh() {
    if (!document_ || !mesh_asset_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source mesh command has no bound mesh asset"));
    }
    const auto current = document_->meshes().find(*mesh_asset_);
    if (current == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "source mesh artifact is no longer present in the project"));
    }
    const auto replaced = document_->replace_mesh_if_revision(
        *mesh_asset_, current->second.revision(), imported_mesh_);
    return replaced
        ? core::Result<void>::success()
        : core::Result<void>::failure(replaced.error());
}

core::Result<void> ImportSourceMeshCommand::apply_previous_mesh() {
    if (!document_ || !mesh_asset_ || !previous_mesh_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source mesh command has no prior mesh snapshot"));
    }
    const auto current = document_->meshes().find(*mesh_asset_);
    if (current == document_->meshes().end()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "source mesh artifact is no longer present in the project"));
    }
    const auto replaced = document_->replace_mesh_if_revision(
        *mesh_asset_, current->second.revision(), *previous_mesh_);
    return replaced
        ? core::Result<void>::success()
        : core::Result<void>::failure(replaced.error());
}

core::Result<void> ImportSourceMeshCommand::restore_created_instance() {
    if (!document_ || !mesh_asset_ || !object_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source mesh command has incomplete instance restoration state"));
    }
    if (auto result = document_->insert_mesh(*mesh_asset_, imported_mesh_); !result) {
        return result;
    }
    if (auto result = document_->insert_object(*object_); !result) return result;
    return core::Result<void>::success();
}

core::Result<void> ImportSourceMeshCommand::remove_created_instance() {
    if (!document_ || !mesh_asset_ || !object_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "source mesh command has incomplete instance removal state"));
    }
    if (auto result = document_->remove_object(object_->id); !result) return result;
    return document_->remove_mesh(*mesh_asset_);
}

core::Result<void> ImportSourceMeshCommand::execute() {
    if (!document_ || applied_ || document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source mesh command is stale, incomplete, or already applied"));
    }
    const project::ProjectDocument before = *document_;
    const auto rollback = [this, &before](core::Diagnostic diagnostic) {
        *document_ = before;
        return core::Result<void>::failure(std::move(diagnostic));
    };

    if (!initialized_) {
        const auto active = document_->source_asset_registry().records().find(
            imported_record_.identity);
        if (active != document_->source_asset_registry().records().end()) {
            if (active->second.kind != assets::SourceAssetKind::mesh ||
                active->second.artifact_identity != imported_record_.artifact_identity) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::validation_failed,
                    "source identity is bound to a different mesh artifact"));
            }
            std::optional<std::uint64_t> bound_asset;
            for (const auto& [asset, mesh] : document_->meshes()) {
                static_cast<void>(mesh);
                if (active->second.artifact_identity ==
                    "mesh.asset." + std::to_string(asset)) {
                    bound_asset = asset;
                    break;
                }
            }
            if (!bound_asset) {
                return core::Result<void>::failure(core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "source registry points to a missing mesh artifact"));
            }
            const auto current_mesh = document_->meshes().find(*bound_asset);
            mesh_asset_ = *bound_asset;
            previous_mesh_ = current_mesh->second;
            previous_record_ = active->second;
            imported_record_.artifact_identity = active->second.artifact_identity;
            mesh_changed_ = active->second.artifact_digest !=
                imported_record_.artifact_digest;
            if (mesh_changed_) {
                if (auto result = apply_imported_mesh(); !result) return rollback(result.error());
            }
            if (auto result = apply_registry_record(imported_record_); !result) {
                return rollback(result.error());
            }
            created_instance_ = false;
        } else {
            const auto mesh = document_->add_mesh(imported_mesh_);
            if (!mesh) return rollback(mesh.error());
            mesh_asset_ = mesh.value();
            auto object = document_->create_object(object_name_);
            if (!object) return rollback(object.error());
            if (auto result = document_->attach_mesh(object.value(), *mesh_asset_); !result) {
                return rollback(result.error());
            }
            const auto* inserted = document_->scene().find(object.value());
            if (!inserted) {
                return rollback(core::Diagnostic(
                    core::ErrorCode::invalid_state,
                    "source mesh object disappeared before command capture"));
            }
            object_ = *inserted;
            imported_record_.artifact_identity =
                "mesh.asset." + std::to_string(*mesh_asset_);
            if (auto result = apply_registry_record(imported_record_); !result) {
                return rollback(result.error());
            }
            created_instance_ = true;
        }
        initialized_ = true;
    } else {
        if (created_instance_) {
            if (auto result = restore_created_instance(); !result) return rollback(result.error());
        } else if (mesh_changed_) {
            if (auto result = apply_imported_mesh(); !result) return rollback(result.error());
        }
        if (auto result = apply_registry_record(imported_record_); !result) {
            return rollback(result.error());
        }
    }
    expected_project_revision_ = document_->revision();
    applied_ = true;
    return core::Result<void>::success();
}

core::Result<void> ImportSourceMeshCommand::undo() {
    if (!document_ || !initialized_ || !applied_ ||
        document_->revision() != expected_project_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "source mesh command is no longer current and cannot be undone"));
    }
    const project::ProjectDocument before = *document_;
    const auto rollback = [this, &before](core::Diagnostic diagnostic) {
        *document_ = before;
        return core::Result<void>::failure(std::move(diagnostic));
    };
    if (created_instance_) {
        if (auto result = remove_created_instance(); !result) return rollback(result.error());
        if (auto result = apply_registry_record(std::nullopt); !result) {
            return rollback(result.error());
        }
    } else {
        if (mesh_changed_) {
            if (auto result = apply_previous_mesh(); !result) return rollback(result.error());
        }
        if (auto result = apply_registry_record(previous_record_); !result) {
            return rollback(result.error());
        }
    }
    expected_project_revision_ = document_->revision();
    applied_ = false;
    return core::Result<void>::success();
}

DuplicateProjectMeshInstanceCommand::DuplicateProjectMeshInstanceCommand(
    ProjectCommandAdmission,
    project::ProjectDocument& document,
    scene::ObjectId source,
    std::string object_name,
    core::Vec3d local_translation_offset)
    : document_(&document),
      source_(source),
      object_name_(std::move(object_name)),
      local_translation_offset_(local_translation_offset) {}

core::Result<void> DuplicateProjectMeshInstanceCommand::execute() {
    if (!document_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance command has no project"));
    }
    if (object_.has_value()) {
        return restore_instance();
    }
    return create_first_instance();
}

core::Result<void> DuplicateProjectMeshInstanceCommand::undo() {
    if (!document_ || !object_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance command has no instance to undo"));
    }
    return remove_instance();
}

core::Result<void> DuplicateProjectMeshInstanceCommand::create_first_instance() {
    if (!source_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "duplicate mesh instance requires a non-zero source object id"));
    }
    if (!local_translation_offset_.finite()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "duplicate mesh instance offset must contain finite values"));
    }

    const auto* source = document_->scene().find(source_);
    if (!source) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "duplicate mesh instance source object does not exist"));
    }
    if (source->locked) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance source object is locked"));
    }
    if (!source->visible) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance source object is hidden"));
    }
    if (source->parent.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance only supports root-level source objects"));
    }
    if (!source->mesh_asset.has_value() || !document_->meshes().contains(*source->mesh_asset)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance source object has no available mesh asset"));
    }
    const std::uint64_t mesh_asset = *source->mesh_asset;

    core::Transform transform = source->local_transform;
    transform.translation = transform.translation + local_translation_offset_;
    if (!transform.finite()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "duplicate mesh instance transform must contain finite values"));
    }

    const project::ProjectDocument before = *document_;
    const auto rollback = [this, &before](core::Diagnostic diagnostic) {
        *document_ = before;
        object_.reset();
        biological_proxy_receipt_.reset();
        return core::Result<void>::failure(std::move(diagnostic));
    };

    auto object = document_->create_object(object_name_, transform);
    if (!object) {
        return rollback(object.error());
    }
    const scene::ObjectId object_id = object.value();
    if (auto attach = document_->attach_mesh(object_id, mesh_asset); !attach) {
        return rollback(attach.error());
    }
    const auto* inserted = document_->scene().find(object_id);
    if (!inserted || inserted->mesh_asset != std::optional<std::uint64_t>{mesh_asset} ||
        inserted->parent.has_value() ||
        !inserted->visible || inserted->locked) {
        return rollback(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicated mesh instance violated root-level instance invariants"));
    }
    object_ = *inserted;
    const auto source_receipt = document_->biological_proxy_instance_receipts_.find(source_);
    if (source_receipt != document_->biological_proxy_instance_receipts_.end()) {
        biological_proxy_receipt_ = source_receipt->second;
        biological_proxy_receipt_->object = object_id;
        biological_proxy_receipt_->mesh_asset = mesh_asset;
        if (auto result = biological_proxy_receipt_->validate(); !result) {
            return rollback(result.error().with_context(
                "duplicated biological proxy source receipt"));
        }
        if (!document_->biological_proxy_instance_receipts_.emplace(
                object_id, *biological_proxy_receipt_).second) {
            return rollback(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "duplicated biological proxy receipt identity is already present"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> DuplicateProjectMeshInstanceCommand::restore_instance() {
    if (!object_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance command has incomplete restoration state"));
    }
    const project::ProjectDocument before = *document_;
    if (auto result = document_->insert_object(*object_); !result) return result;
    if (biological_proxy_receipt_.has_value() &&
        !document_->biological_proxy_instance_receipts_.emplace(
            object_->id, *biological_proxy_receipt_).second) {
        *document_ = before;
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "duplicated biological proxy receipt could not be restored"));
    }
    return core::Result<void>::success();
}

core::Result<void> DuplicateProjectMeshInstanceCommand::remove_instance() {
    return document_->remove_object(object_->id);
}

DuplicateProjectMeshInstanceBatchCommand::DuplicateProjectMeshInstanceBatchCommand(
    ProjectCommandAdmission admission,
    project::ProjectDocument& document,
    scene::ObjectId source,
    std::vector<MeshInstanceBatchInstance> instances)
    : document_(&document),
      admission_(admission),
      source_(source),
      instances_(std::move(instances)) {}

core::Result<void> DuplicateProjectMeshInstanceBatchCommand::execute() {
    if (!document_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch command has no project"));
    }
    if (instances_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "duplicate mesh instance batch must contain at least one placement"));
    }
    if (instances_.size() > kMaxMeshInstanceBatchInstances) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "duplicate mesh instance batch exceeds the bounded instance count"));
    }
    if (!delegates_.empty()) return restore_instances();
    return create_first_instances();
}

core::Result<void> DuplicateProjectMeshInstanceBatchCommand::undo() {
    if (!document_ || delegates_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch has no instances to undo"));
    }
    return remove_instances();
}

core::Result<void> DuplicateProjectMeshInstanceBatchCommand::create_first_instances() {
    if (!source_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "duplicate mesh instance batch requires a non-zero source object id"));
    }
    for (const auto& instance : instances_) {
        if (instance.object_name.empty()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "duplicate mesh instance batch entries require non-empty names"));
        }
        if (!instance.local_translation_offset.finite()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "duplicate mesh instance batch offsets must contain finite values"));
        }
        if (instance.local_translation_offset.length() > kMaxMeshInstanceBatchOffsetMagnitude) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "duplicate mesh instance batch offset exceeds the bounded placement extent"));
        }
    }

    const auto* source = document_->scene().find(source_);
    if (!source) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "duplicate mesh instance batch source object does not exist"));
    }
    if (source->locked) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch source object is locked"));
    }
    if (!source->visible) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch source object is hidden"));
    }
    if (source->parent.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch only supports root-level source objects"));
    }
    if (!source->mesh_asset.has_value() || !document_->meshes().contains(*source->mesh_asset)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch source object has no available mesh asset"));
    }

    delegates_.reserve(instances_.size());
    for (const auto& instance : instances_) {
        delegates_.push_back(std::make_unique<DuplicateProjectMeshInstanceCommand>(
            admission_, *document_, source_, instance.object_name,
            instance.local_translation_offset));
        if (auto result = delegates_.back()->execute(); !result) {
            for (std::size_t prior = delegates_.size() - 1U; prior > 0U; --prior) {
                const auto undone = delegates_[prior - 1U]->undo();
                static_cast<void>(undone);
            }
            delegates_.clear();
            return result;
        }
    }
    return core::Result<void>::success();
}

core::Result<void> DuplicateProjectMeshInstanceBatchCommand::restore_instances() {
    if (delegates_.size() != instances_.size()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "duplicate mesh instance batch redo has incomplete instance history"));
    }
    for (std::size_t index = 0U; index < delegates_.size(); ++index) {
        if (auto result = delegates_[index]->execute(); !result) {
            for (std::size_t prior = index; prior > 0U; --prior) {
                const auto undone = delegates_[prior - 1U]->undo();
                static_cast<void>(undone);
            }
            return result;
        }
    }
    return core::Result<void>::success();
}

core::Result<void> DuplicateProjectMeshInstanceBatchCommand::remove_instances() {
    for (auto delegate = delegates_.rbegin(); delegate != delegates_.rend(); ++delegate) {
        if (auto result = (*delegate)->undo(); !result) {
            return result;
        }
    }
    return core::Result<void>::success();
}

CreatePlantScatterBatchCommand::CreatePlantScatterBatchCommand(
    ProjectCommandAdmission admission,
    project::ProjectDocument& document,
    scene::ObjectId botanical_source_object,
    condition::VegetationPreviewResult preview)
    : document_(&document),
      botanical_source_object_(botanical_source_object),
      preview_(std::move(preview)) {
    static_cast<void>(admission);
}

core::Result<void> CreatePlantScatterBatchCommand::execute() {
    if (!document_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION vegetation batch command has no project"));
    }
    if (instances_.empty()) return create_first_instances();
    return restore_instances();
}

core::Result<void> CreatePlantScatterBatchCommand::undo() {
    if (!document_ || instances_.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION vegetation batch command has no committed instances"));
    }
    return remove_instances();
}

core::Result<void> CreatePlantScatterBatchCommand::create_first_instances() {
    if (!botanical_source_object_ || preview_.recipe_id == 0U ||
        preview_.project_revision.value() == 0U ||
        preview_.application_revision.value() == 0U ||
        preview_.authoring_digest.is_zero() ||
        preview_.preview_digest.is_zero() || !preview_.plant_source.has_value() ||
        preview_.instances.empty() ||
        preview_.instances.size() > kMaxMeshInstanceBatchInstances) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "CONDITION vegetation commit requires one bounded source-bound current preview"));
    }
    if (preview_.application_revision != document_->revision()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION vegetation batch command received a stale project revision"));
    }
    const auto* source = document_->scene().find(botanical_source_object_);
    if (!source || source->parent.has_value() || !source->visible || source->locked ||
        !source->mesh_asset.has_value() || !document_->meshes().contains(*source->mesh_asset)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION vegetation source must be a visible, unlocked root botanical mesh"));
    }
    constexpr double kIdentityTolerance = 1.0e-12;
    const auto& source_transform = source->local_transform;
    if (source_transform.translation.length() > kIdentityTolerance ||
        std::abs(source_transform.rotation.x) > kIdentityTolerance ||
        std::abs(source_transform.rotation.y) > kIdentityTolerance ||
        std::abs(source_transform.rotation.z) > kIdentityTolerance ||
        std::abs(source_transform.rotation.w - 1.0) > kIdentityTolerance ||
        std::abs(source_transform.scale.x - 1.0) > kIdentityTolerance ||
        std::abs(source_transform.scale.y - 1.0) > kIdentityTolerance ||
        std::abs(source_transform.scale.z - 1.0) > kIdentityTolerance) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION vegetation prototype must retain its identity transform"));
    }
    std::set<std::uint64_t> stable_ids;
    for (const auto& placement : preview_.instances) {
        if (placement.recipe_id != preview_.recipe_id || placement.stable_id == 0U ||
            !placement.position.finite() ||
            !std::isfinite(placement.yaw_radians) || placement.yaw_radians < 0.0 ||
            placement.yaw_radians >= 6.28318530717958647692 ||
            !std::isfinite(placement.uniform_scale) || placement.uniform_scale <= 0.0 ||
            !std::isfinite(placement.suitability) || placement.suitability < 0.0 ||
            placement.suitability > 1.0 || !stable_ids.insert(placement.stable_id).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "CONDITION vegetation preview contains an invalid or duplicate placement"));
        }
        const bool already_committed = std::any_of(
            document_->condition_vegetation_placement_receipts_.begin(),
            document_->condition_vegetation_placement_receipts_.end(),
            [&placement](const auto& entry) {
                return entry.second.recipe_id == placement.recipe_id &&
                    entry.second.stable_placement_id == placement.stable_id;
            });
        if (already_committed) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "CONDITION vegetation placement identity is already committed"));
        }
    }

    const project::ProjectDocument before = *document_;
    const auto rollback = [this, &before](core::Diagnostic diagnostic) {
        *document_ = before;
        instances_.clear();
        receipts_.clear();
        created_objects_.clear();
        return core::Result<void>::failure(std::move(diagnostic));
    };
    instances_.reserve(preview_.instances.size());
    receipts_.reserve(preview_.instances.size());
    created_objects_.reserve(preview_.instances.size());
    const std::uint64_t mesh_asset = *source->mesh_asset;
    for (const auto& placement : preview_.instances) {
        const double half_yaw = placement.yaw_radians * 0.5;
        const core::Transform transform{
            placement.position,
            core::Quaternion{0.0, std::sin(half_yaw), 0.0, std::cos(half_yaw)},
            {placement.uniform_scale, placement.uniform_scale, placement.uniform_scale}};
        const std::string object_name = "Plant " + std::to_string(preview_.recipe_id) +
            " " + std::to_string(placement.stable_id);
        auto object_id = document_->create_object(object_name, transform);
        if (!object_id) return rollback(object_id.error());
        if (auto attached = document_->attach_mesh(object_id.value(), mesh_asset); !attached) {
            return rollback(attached.error());
        }
        const auto* created = document_->scene().find(object_id.value());
        if (!created) {
            return rollback(core::Diagnostic(core::ErrorCode::invalid_state,
                "CONDITION vegetation instance disappeared during batch creation"));
        }
        project::ConditionVegetationPlacementReceipt receipt{
            created->id,
            mesh_asset,
            placement.stable_id,
            placement.recipe_id,
            placement.species_asset_id,
            preview_.plant_source->identity,
            preview_.plant_source->source_revision,
            preview_.plant_source->digest,
            preview_.application_revision,
            preview_.project_revision,
            preview_.authoring_digest,
            preview_.preview_digest,
            preview_.fields,
            placement.position,
            placement.yaw_radians,
            placement.uniform_scale,
            placement.suitability,
        };
        if (auto valid = receipt.validate(); !valid) return rollback(valid.error());
        if (!document_->condition_vegetation_placement_receipts_.emplace(
                receipt.object, receipt).second) {
            return rollback(core::Diagnostic(core::ErrorCode::validation_failed,
                "CONDITION vegetation placement receipt object identity is duplicated"));
        }
        instances_.push_back(*created);
        receipts_.push_back(std::move(receipt));
        created_objects_.push_back(object_id.value());
    }
    return core::Result<void>::success();
}

core::Result<void> CreatePlantScatterBatchCommand::restore_instances() {
    if (instances_.size() != receipts_.size() ||
        instances_.size() != created_objects_.size()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "CONDITION vegetation redo has incomplete restoration state"));
    }
    const project::ProjectDocument before = *document_;
    for (std::size_t index = 0U; index < instances_.size(); ++index) {
        if (auto inserted = document_->insert_object(instances_[index]); !inserted) {
            *document_ = before;
            return inserted;
        }
        if (!document_->condition_vegetation_placement_receipts_.emplace(
                receipts_[index].object, receipts_[index]).second) {
            *document_ = before;
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "CONDITION vegetation redo could not restore a placement receipt"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> CreatePlantScatterBatchCommand::remove_instances() {
    const project::ProjectDocument before = *document_;
    for (auto instance = instances_.rbegin(); instance != instances_.rend(); ++instance) {
        if (auto removed = document_->remove_object(instance->id); !removed) {
            *document_ = before;
            return removed;
        }
    }
    return core::Result<void>::success();
}

} // namespace carto::editor
