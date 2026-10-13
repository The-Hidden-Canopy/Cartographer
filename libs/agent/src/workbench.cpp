#include <carto/agent/workbench.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace carto::agent {
namespace {

constexpr std::size_t kMaxGoalBytes = 1024U;
constexpr std::size_t kMaxStepIntentBytes = 1024U;
constexpr std::size_t kMaxStepIdBytes = 64U;
constexpr std::size_t kMaxTaskIdBytes = 64U;
constexpr std::size_t kMaxScopeObjects = 512U;
constexpr std::size_t kMaxPlanSteps = 256U;
constexpr std::uint32_t kKnownEffects = effect_mask(Effect::geometry) |
    effect_mask(Effect::topology);

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic policy(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

bool safe_text(std::string_view value, std::size_t limit) noexcept {
    return !value.empty() && value.size() <= limit &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return character >= 0x20U && character != 0x7fU;
        });
}

template <typename Id>
core::Result<void> validate_object_ids(
    const std::vector<Id>& ids,
    std::string_view label,
    bool allow_empty = false) {
    if ((!allow_empty && ids.empty()) || ids.size() > kMaxScopeObjects) {
        return core::Result<void>::failure(invalid(
            std::string(label) + " count is outside its supported range"));
    }
    std::set<Id> unique;
    for (const Id id : ids) {
        if (!id || !unique.insert(id).second) {
            return core::Result<void>::failure(invalid(
                std::string(label) + " contains a zero or duplicate identity"));
        }
    }
    return core::Result<void>::success();
}

bool contains(const std::vector<scene::ObjectId>& objects, scene::ObjectId object) {
    return std::find(objects.begin(), objects.end(), object) != objects.end();
}

bool same_transform(const core::Transform& left, const core::Transform& right) noexcept {
    return left.translation.x == right.translation.x &&
        left.translation.y == right.translation.y &&
        left.translation.z == right.translation.z &&
        left.rotation.x == right.rotation.x && left.rotation.y == right.rotation.y &&
        left.rotation.z == right.rotation.z && left.rotation.w == right.rotation.w &&
        left.scale.x == right.scale.x && left.scale.y == right.scale.y &&
        left.scale.z == right.scale.z;
}

bool has_changes(const ObjectChange& change) noexcept {
    return change.added || change.removed || change.name_changed ||
        change.transform_changed || change.parent_changed ||
        change.mesh_binding_changed || change.mesh_content_changed ||
        change.visibility_changed || change.lock_changed;
}

assets::Sha256Digest project_result_digest(const project::ProjectDocument& after) {
    const std::string identity = after.serialize();
    return assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size()));
}

std::optional<std::uint32_t> effect_for_tool(std::string_view tool_id) noexcept {
    if (tool_id == "mesh.extrude-face" || tool_id == "mesh.inset-face") {
        return effect_mask(Effect::geometry) | effect_mask(Effect::topology);
    }
    if (tool_id == "mesh.set-vertex-position") return effect_mask(Effect::geometry);
    return std::nullopt;
}

std::string effect_names(std::uint32_t effects) {
    if ((effects & (effect_mask(Effect::geometry) | effect_mask(Effect::topology))) ==
        (effect_mask(Effect::geometry) | effect_mask(Effect::topology))) {
        return "geometry and topology";
    }
    if ((effects & effect_mask(Effect::topology)) != 0U) return "topology";
    if ((effects & effect_mask(Effect::geometry)) != 0U) return "geometry";
    return "no declared effect";
}

} // namespace

core::Result<void> TargetRef::validate() const {
    if (!object) return core::Result<void>::failure(invalid("plan target object is zero"));
    const bool valid_component = std::visit([](const auto id) { return static_cast<bool>(id); }, component);
    if (!valid_component) {
        return core::Result<void>::failure(invalid("plan target component is zero"));
    }
    return core::Result<void>::success();
}

core::Result<void> AuthoringObjective::validate() const {
    if (!safe_text(id, kMaxTaskIdBytes) || !safe_text(goal, kMaxGoalBytes)) {
        return core::Result<void>::failure(invalid("objective identity or goal is invalid"));
    }
    if (base_revision.exhausted()) {
        return core::Result<void>::failure(invalid("objective base revision is exhausted"));
    }
    if (auto result = validate_object_ids(scope_objects, "objective scope"); !result) {
        return result;
    }
    if (auto result = validate_object_ids(protected_objects, "protected object set", true); !result) {
        return result;
    }
    for (const auto object : protected_objects) {
        if (!contains(scope_objects, object)) {
            return core::Result<void>::failure(invalid(
                "protected objects must be inside the declared objective scope"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> AgentPlanStep::validate() const {
    if (!safe_text(id, kMaxStepIdBytes) || !safe_text(intent, kMaxStepIntentBytes)) {
        return core::Result<void>::failure(invalid("plan step identity or intent is invalid"));
    }
    return target.validate();
}

core::Result<void> AgentPlan::validate() const {
    if (!safe_text(id, kMaxTaskIdBytes)) {
        return core::Result<void>::failure(invalid("plan identity is invalid"));
    }
    if (auto result = objective.validate(); !result) return result;
    if (steps.empty() || steps.size() > kMaxPlanSteps) {
        return core::Result<void>::failure(invalid("plan step count is outside its supported range"));
    }
    std::set<std::string> step_ids;
    for (const auto& step : steps) {
        if (auto result = step.validate(); !result) return result;
        if (!step_ids.insert(step.id).second) {
            return core::Result<void>::failure(invalid("plan contains a duplicate step identity"));
        }
        if (!contains(objective.scope_objects, step.target.object)) {
            return core::Result<void>::failure(policy(
                "plan target is outside the objective scope"));
        }
        if (contains(objective.protected_objects, step.target.object)) {
            return core::Result<void>::failure(policy(
                "plan target is protected by the objective"));
        }
    }
    return core::Result<void>::success();
}

core::Result<void> AgentWorkEnvelope::validate() const {
    if (auto result = validate_object_ids(scope_objects, "work envelope scope"); !result) {
        return result;
    }
    if (permitted_effects == 0U || (permitted_effects & ~kKnownEffects) != 0U) {
        return core::Result<void>::failure(invalid("work envelope effect mask is invalid"));
    }
    if (max_commands == 0U || max_commands > kMaxPlanSteps ||
        max_objects_touched == 0U || max_objects_touched > kMaxScopeObjects) {
        return core::Result<void>::failure(invalid("work envelope budget is invalid"));
    }
    return core::Result<void>::success();
}

std::size_t ProjectDiff::changed_object_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        objects.begin(), objects.end(), has_changes));
}

std::size_t ProjectDiff::changed_mesh_count() const noexcept {
    return meshes.size();
}

AgentTask::AgentTask(
    application::ApplicationSession& source,
    AgentPlan plan,
    AgentWorkEnvelope envelope,
    std::unique_ptr<application::ApplicationSession> speculative,
    application::ApplicationSnapshot source_snapshot)
    : source_(&source),
      plan_(std::move(plan)),
      envelope_(std::move(envelope)),
      base_document_(source.document_),
      base_scene_(std::move(source_snapshot.viewport.scene)),
      base_viewport_error_(std::move(source_snapshot.viewport.error)),
      speculative_(std::move(speculative)) {}

core::Result<AgentTask> AgentTask::begin(
    application::ApplicationSession& source,
    AgentPlan plan,
    AgentWorkEnvelope envelope) {
    if (auto result = plan.validate(); !result) {
        return core::Result<AgentTask>::failure(result.error());
    }
    if (auto result = envelope.validate(); !result) {
        return core::Result<AgentTask>::failure(result.error());
    }
    auto source_snapshot = source.snapshot();
    if (plan.objective.base_revision != source_snapshot.project_revision) {
        return core::Result<AgentTask>::failure(stale(
            "objective was prepared against a different project revision"));
    }
    if (plan.steps.size() > envelope.max_commands) {
        return core::Result<AgentTask>::failure(policy(
            "plan exceeds the work envelope command budget"));
    }
    for (const auto object : plan.objective.scope_objects) {
        if (!contains(envelope.scope_objects, object)) {
            return core::Result<AgentTask>::failure(policy(
                "work envelope does not cover the complete objective scope"));
        }
    }
    for (const auto object : envelope.scope_objects) {
        if (!contains(plan.objective.scope_objects, object)) {
            return core::Result<AgentTask>::failure(policy(
                "work envelope expands beyond the declared objective scope"));
        }
    }
    auto speculative = std::unique_ptr<application::ApplicationSession>(
        new application::ApplicationSession(
            source, application::ApplicationSession::SpeculativeCopyTag{}));
    return core::Result<AgentTask>::success(AgentTask(
        source, std::move(plan), std::move(envelope), std::move(speculative),
        std::move(source_snapshot)));
}

bool AgentTask::scope_contains(scene::ObjectId object) const noexcept {
    return contains(envelope_.scope_objects, object);
}

core::Result<void> AgentTask::execute_step(const AgentPlanStep& step) {
    StepReceipt receipt;
    receipt.step_id = step.id;
    receipt.intent = step.intent;
    receipt.target_object = step.target.object;
    receipt.revision_before = speculative_->document_.revision();
    const auto fail = [this, &receipt](core::Diagnostic diagnostic) {
        receipt.status = StepStatus::failed;
        receipt.diagnostic = diagnostic;
        receipts_.push_back(std::move(receipt));
        return core::Result<void>::failure(std::move(diagnostic));
    };

    if (!scope_contains(step.target.object)) {
        return fail(policy("plan step target is outside the work envelope"));
    }
    const auto* target_object = speculative_->document_.scene().find(step.target.object);
    if (target_object == nullptr || !target_object->mesh_asset.has_value()) {
        return fail(policy("plan target does not resolve to an authored mesh asset"));
    }
    std::set<scene::ObjectId> affected_objects;
    for (const auto& object : speculative_->document_.scene().objects_sorted()) {
        if (object.mesh_asset != target_object->mesh_asset) continue;
        if (!scope_contains(object.id)) {
            return fail(policy(
                "plan target mesh is shared by an object outside the work envelope"));
        }
        if (contains(plan_.objective.protected_objects, object.id)) {
            return fail(policy(
                "plan target mesh is shared by an object protected by the objective"));
        }
        affected_objects.insert(object.id);
    }
    const auto newly_touched = static_cast<std::size_t>(std::count_if(
        affected_objects.begin(), affected_objects.end(), [this](scene::ObjectId object) {
            return !touched_objects_.contains(object);
        }));
    if (newly_touched > envelope_.max_objects_touched - touched_objects_.size()) {
        return fail(policy("plan exceeds the work envelope object-touch budget"));
    }

    application::ApplicationAction select_mode;
    application::ApplicationAction select_component;
    if (const auto* face = std::get_if<geometry::FaceId>(&step.target.component)) {
        select_mode = application::SetSelectionModeAction{editor::SelectionMode::face};
        select_component = application::SelectFaceAction{step.target.object, *face};
    } else {
        const auto vertex = std::get<geometry::VertexId>(step.target.component);
        select_mode = application::SetSelectionModeAction{editor::SelectionMode::vertex};
        select_component = application::SelectVertexAction{step.target.object, vertex};
    }
    auto selected = application::HumanApplicationAccess::dispatch(*speculative_, select_mode);
    if (!selected) return fail(selected.error().with_context("speculative plan selection mode"));
    selected = application::HumanApplicationAccess::dispatch(*speculative_, select_component);
    if (!selected) return fail(selected.error().with_context("speculative plan target selection"));

    const auto authoring = speculative_->authoring_context();
    if (!authoring) return fail(authoring.error().with_context("speculative authoring context"));
    const auto snapshot = speculative_->snapshot();
    const ai::Context context{
        snapshot.project_revision,
        authoring.value(),
        ai::build_operation_ontology(snapshot.tools)};
    if (auto result = context.validate(); !result) {
        return fail(result.error().with_context("speculative capability context"));
    }
    auto proposal = planner_.propose(step.intent, context);
    if (!proposal) return fail(proposal.error().with_context("native plan step"));
    receipt.tool_id = proposal.value().tool_id;
    if (auto result = proposal.value().validate(context); !result) {
        return fail(result.error().with_context("native proposal validation"));
    }
    const auto effects = effect_for_tool(proposal.value().tool_id);
    if (!effects.has_value()) {
        return fail(policy("plan selected a capability without a governed effect contract"));
    }
    if ((*effects & ~envelope_.permitted_effects) != 0U) {
        return fail(policy(
            "work envelope forbids the " + effect_names(*effects) + " effect"));
    }

    auto preview = speculative_->begin_preview(proposal.value().preview_kind);
    if (!preview) return fail(preview.error().with_context("speculative native preview"));
    if (auto result = preview.value().update(proposal.value().parameters); !result) {
        return fail(result.error().with_context("speculative preview parameters"));
    }
    const auto committed = application::HumanApplicationAccess::commit_preview(
        *speculative_, preview.value(), application::OperationSource::ai_proposal,
        plan_.id + ":" + step.id);
    if (!committed) return fail(committed.error().with_context("speculative native command"));
    if (auto result = speculative_->document_.validate(); !result) {
        return fail(result.error().with_context("speculative project validation"));
    }

    touched_objects_.insert(affected_objects.begin(), affected_objects.end());
    receipt.status = StepStatus::passed;
    receipt.revision_after = speculative_->document_.revision();
    receipts_.push_back(std::move(receipt));
    return core::Result<void>::success();
}

core::Result<AgentTaskSnapshot> AgentTask::run() {
    if (status_ != TaskStatus::prepared) {
        return core::Result<AgentTaskSnapshot>::failure(invalid_state(
            "agent plan can only run once"));
    }
    cached_snapshot_.reset();
    status_ = TaskStatus::running;
    for (const auto& step : plan_.steps) {
        auto result = execute_step(step);
        if (!result) {
            status_ = TaskStatus::blocked;
            diagnostic_ = result.error();
            break;
        }
    }
    gates_ = evaluate_gates();
    const bool required_gates_pass = std::all_of(
        gates_.begin(), gates_.end(), [](const AcceptanceGateResult& gate) {
            return !gate.required || gate.passed;
        });
    if (status_ == TaskStatus::running && required_gates_pass) {
        status_ = TaskStatus::review;
    } else if (status_ == TaskStatus::running) {
        status_ = TaskStatus::blocked;
        diagnostic_ = invalid_state("one or more required agent acceptance gates failed");
    }
    return core::Result<AgentTaskSnapshot>::success(snapshot());
}

ProjectDiff AgentTask::build_diff() const {
    ProjectDiff diff;
    diff.base_revision = plan_.objective.base_revision;
    diff.speculative_revision = speculative_->document_.revision();

    const auto before_objects = base_document_.scene().objects_sorted();
    const auto after_objects = speculative_->document_.scene().objects_sorted();
    std::map<scene::ObjectId, scene::SceneObject> before_by_id;
    std::map<scene::ObjectId, scene::SceneObject> after_by_id;
    for (const auto& object : before_objects) before_by_id.emplace(object.id, object);
    for (const auto& object : after_objects) after_by_id.emplace(object.id, object);
    std::set<scene::ObjectId> object_ids;
    for (const auto& [id, object] : before_by_id) { static_cast<void>(object); object_ids.insert(id); }
    for (const auto& [id, object] : after_by_id) { static_cast<void>(object); object_ids.insert(id); }
    for (const auto id : object_ids) {
        ObjectChange change;
        change.id = id;
        const auto before = before_by_id.find(id);
        const auto after = after_by_id.find(id);
        change.added = before == before_by_id.end();
        change.removed = after == after_by_id.end();
        if (!change.added && !change.removed) {
            change.name_changed = before->second.name != after->second.name;
            change.transform_changed = !same_transform(
                before->second.local_transform, after->second.local_transform);
            change.parent_changed = before->second.parent != after->second.parent;
            change.mesh_binding_changed = before->second.mesh_asset != after->second.mesh_asset;
            change.visibility_changed = before->second.visible != after->second.visible;
            change.lock_changed = before->second.locked != after->second.locked;
        }
        if (has_changes(change)) diff.objects.push_back(change);
    }

    const auto& before_meshes = base_document_.meshes();
    const auto& after_meshes = speculative_->document_.meshes();
    std::set<std::uint64_t> mesh_ids;
    for (const auto& [id, mesh] : before_meshes) { static_cast<void>(mesh); mesh_ids.insert(id); }
    for (const auto& [id, mesh] : after_meshes) { static_cast<void>(mesh); mesh_ids.insert(id); }
    for (const auto id : mesh_ids) {
        MeshChange change;
        change.asset_id = id;
        const auto before = before_meshes.find(id);
        const auto after = after_meshes.find(id);
        change.added = before == before_meshes.end();
        change.removed = after == after_meshes.end();
        if (before != before_meshes.end()) {
            change.before_revision = before->second.revision();
            change.before_vertices = before->second.vertex_count();
            change.before_faces = before->second.face_count();
        }
        if (after != after_meshes.end()) {
            change.after_revision = after->second.revision();
            change.after_vertices = after->second.vertex_count();
            change.after_faces = after->second.face_count();
        }
        if (before != before_meshes.end() && after != after_meshes.end()) {
            std::map<geometry::VertexId, core::Vec3d> before_vertices;
            std::map<geometry::VertexId, core::Vec3d> after_vertices;
            for (const auto& vertex : before->second.vertices_sorted()) {
                before_vertices.emplace(vertex.id, vertex.position);
            }
            for (const auto& vertex : after->second.vertices_sorted()) {
                after_vertices.emplace(vertex.id, vertex.position);
            }
            std::set<geometry::VertexId> vertex_ids;
            for (const auto& [vertex_id, position] : before_vertices) {
                static_cast<void>(position);
                vertex_ids.insert(vertex_id);
            }
            for (const auto& [vertex_id, position] : after_vertices) {
                static_cast<void>(position);
                vertex_ids.insert(vertex_id);
            }
            for (const auto vertex_id : vertex_ids) {
                VertexValueChange vertex_change;
                vertex_change.id = vertex_id;
                const auto old_position = before_vertices.find(vertex_id);
                const auto new_position = after_vertices.find(vertex_id);
                vertex_change.added = old_position == before_vertices.end();
                vertex_change.removed = new_position == after_vertices.end();
                if (old_position != before_vertices.end()) {
                    vertex_change.before_position = old_position->second;
                }
                if (new_position != after_vertices.end()) {
                    vertex_change.after_position = new_position->second;
                }
                vertex_change.position_changed = !vertex_change.added &&
                    !vertex_change.removed &&
                    (vertex_change.before_position.x != vertex_change.after_position.x ||
                     vertex_change.before_position.y != vertex_change.after_position.y ||
                     vertex_change.before_position.z != vertex_change.after_position.z);
                if (vertex_change.added || vertex_change.removed || vertex_change.position_changed) {
                    change.vertices.push_back(vertex_change);
                }
            }

            std::map<geometry::FaceId, std::vector<geometry::VertexId>> before_faces;
            std::map<geometry::FaceId, std::vector<geometry::VertexId>> after_faces;
            for (const auto& face : before->second.faces_sorted()) {
                before_faces.emplace(face.id, face.vertices);
            }
            for (const auto& face : after->second.faces_sorted()) {
                after_faces.emplace(face.id, face.vertices);
            }
            std::set<geometry::FaceId> face_ids;
            for (const auto& [face_id, vertices] : before_faces) {
                static_cast<void>(vertices);
                face_ids.insert(face_id);
            }
            for (const auto& [face_id, vertices] : after_faces) {
                static_cast<void>(vertices);
                face_ids.insert(face_id);
            }
            for (const auto face_id : face_ids) {
                FaceTopologyChange face_change;
                face_change.id = face_id;
                const auto old_vertices = before_faces.find(face_id);
                const auto new_vertices = after_faces.find(face_id);
                face_change.added = old_vertices == before_faces.end();
                face_change.removed = new_vertices == after_faces.end();
                if (old_vertices != before_faces.end()) {
                    face_change.before_vertices = old_vertices->second;
                }
                if (new_vertices != after_faces.end()) {
                    face_change.after_vertices = new_vertices->second;
                }
                face_change.connectivity_changed = !face_change.added &&
                    !face_change.removed &&
                    face_change.before_vertices != face_change.after_vertices;
                if (face_change.added || face_change.removed ||
                    face_change.connectivity_changed) {
                    change.faces.push_back(std::move(face_change));
                }
            }
        }
        if (change.added || change.removed ||
            change.before_revision != change.after_revision) {
            diff.meshes.push_back(std::move(change));
        }
    }
    for (const scene::ObjectId id : object_ids) {
        const auto* before_object = base_document_.scene().find(id);
        const auto* after_object = speculative_->document_.scene().find(id);
        if (before_object == nullptr || after_object == nullptr ||
            before_object->mesh_asset != after_object->mesh_asset ||
            !after_object->mesh_asset.has_value()) {
            continue;
        }
        const bool mesh_content_changed = std::any_of(
            diff.meshes.begin(), diff.meshes.end(), [&after_object](const MeshChange& mesh) {
                return mesh.asset_id == *after_object->mesh_asset;
            });
        if (!mesh_content_changed) continue;
        const auto existing = std::find_if(
            diff.objects.begin(), diff.objects.end(), [id](const ObjectChange& change) {
                return change.id == id;
            });
        if (existing != diff.objects.end()) {
            existing->mesh_content_changed = true;
        } else {
            ObjectChange change;
            change.id = id;
            change.mesh_content_changed = true;
            diff.objects.push_back(std::move(change));
        }
    }
    diff.engineering_changed = base_document_.engineering_authoring() !=
        speculative_->document_.engineering_authoring();
    diff.condition_changed = base_document_.condition_authoring() !=
        speculative_->document_.condition_authoring();
    diff.digest = project_result_digest(speculative_->document_);
    return diff;
}

std::vector<AcceptanceGateResult> AgentTask::evaluate_gates() const {
    std::vector<AcceptanceGateResult> gates;
    const auto project_validation = speculative_->document_.validate();
    gates.push_back(AcceptanceGateResult{
        "project.valid", true, static_cast<bool>(project_validation),
        project_validation ? "shadow ProjectDocument validation passed"
                           : project_validation.error().message});
    const auto viewport = speculative_->snapshot().viewport;
    gates.push_back(AcceptanceGateResult{
        "viewport.compiles", true, !viewport.error.has_value(),
        viewport.error ? viewport.error->message : "shadow viewport compiled"});
    const bool all_steps_passed = receipts_.size() == plan_.steps.size() &&
        std::all_of(receipts_.begin(), receipts_.end(), [](const StepReceipt& receipt) {
            return receipt.status == StepStatus::passed;
        });
    gates.push_back(AcceptanceGateResult{
        "plan.steps", true, all_steps_passed,
        std::to_string(receipts_.size()) + " of " + std::to_string(plan_.steps.size()) +
            " native steps passed"});
    const ProjectDiff diff = build_diff();
    const bool changed = diff.changed_object_count() != 0U ||
        diff.changed_mesh_count() != 0U || diff.engineering_changed || diff.condition_changed;
    gates.push_back(AcceptanceGateResult{
        "diff.non_empty", true, changed,
        changed ? "structured source diff contains changes" : "no source changes were produced"});
    return gates;
}

AgentTaskSnapshot AgentTask::snapshot() const {
    if (!cached_snapshot_.has_value()) cached_snapshot_ = build_snapshot();
    AgentTaskSnapshot view = *cached_snapshot_;
    view.status = status_;
    view.diagnostic = diagnostic_;
    view.can_accept = status_ == TaskStatus::review && std::all_of(
        gates_.begin(), gates_.end(), [](const AcceptanceGateResult& gate) {
            return !gate.required || gate.passed;
        });
    return view;
}

AgentTaskSnapshot AgentTask::build_snapshot() const {
    AgentTaskSnapshot view;
    view.status = status_;
    view.task_id = plan_.id;
    view.goal = plan_.objective.goal;
    view.base_revision = plan_.objective.base_revision;
    const auto speculative_snapshot = speculative_->snapshot();
    view.speculative_revision = speculative_snapshot.project_revision;
    view.steps = receipts_;
    view.gates = gates_;
    view.diff = build_diff();
    view.before_scene = base_scene_;
    view.after_scene = speculative_snapshot.viewport.scene;
    view.before_viewport_error = base_viewport_error_;
    view.after_viewport_error = speculative_snapshot.viewport.error;
    view.diagnostic = diagnostic_;
    view.can_accept = status_ == TaskStatus::review && std::all_of(
        gates_.begin(), gates_.end(), [](const AcceptanceGateResult& gate) {
            return !gate.required || gate.passed;
        });
    return view;
}

core::Result<application::DispatchReceipt> AgentTask::accept(
    application::HumanActionAdmission) {
    if (status_ != TaskStatus::review) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "agent result can only be accepted after all required gates pass"));
    }
    const std::string diff_digest = build_diff().digest.hex();
    const auto accepted = source_->accept_speculative_result(
        speculative_->document_, plan_.objective.base_revision, plan_.id, diff_digest);
    if (!accepted) {
        status_ = TaskStatus::blocked;
        diagnostic_ = accepted.error();
        return accepted;
    }
    status_ = TaskStatus::accepted;
    return accepted;
}

core::Result<void> AgentTask::reject() {
    if (status_ == TaskStatus::accepted) {
        return core::Result<void>::failure(invalid_state(
            "an accepted agent result cannot be rejected"));
    }
    if (status_ == TaskStatus::rejected) return core::Result<void>::success();
    status_ = TaskStatus::rejected;
    return core::Result<void>::success();
}

const char* task_status_name(TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::prepared: return "prepared";
    case TaskStatus::running: return "running";
    case TaskStatus::review: return "review";
    case TaskStatus::blocked: return "blocked";
    case TaskStatus::accepted: return "accepted";
    case TaskStatus::rejected: return "rejected";
    }
    return "unknown";
}

} // namespace carto::agent

namespace carto::application {

core::Result<DispatchReceipt> HumanApplicationAccess::accept_agent_task(
    ::carto::agent::AgentTask& task) {
    return task.accept(HumanActionAdmission{});
}

} // namespace carto::application
