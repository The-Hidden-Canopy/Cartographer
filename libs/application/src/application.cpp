#include <carto/application/application.hpp>

#include <memory>
#include <limits>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace carto::application {

namespace {

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

constexpr std::size_t kMaxProblems = 128U;
constexpr std::size_t kMaxJournalPayloadBytes = 64U * 1024U * 1024U;
constexpr std::string_view kSnapshotEvent = "cartographer.snapshot";
constexpr std::string_view kActionEvent = "cartographer.application_action";

std::filesystem::path journal_path_for(const std::filesystem::path& project_path) {
    std::filesystem::path journal_path = project_path;
    journal_path += ".journal";
    return journal_path;
}

std::string document_snapshot_payload(
    std::string_view header,
    std::string_view operation,
    std::string_view serialized_document) {
    return std::string(header) + "\n" +
        (operation.empty() ? std::string{} : "operation=" + std::string(operation) + "\n") +
        "document_bytes=" + std::to_string(serialized_document.size()) + "\n" +
        std::string(serialized_document);
}

bool journal_entry_matches_document(
    const journal::JournalEntry& entry,
    std::string_view serialized_document) {
    const std::string payload(entry.payload.begin(), entry.payload.end());
    const std::string marker =
        "document_bytes=" + std::to_string(serialized_document.size()) + "\n";
    const std::size_t marker_offset = payload.find(marker);
    if (marker_offset == std::string::npos) return false;
    const std::size_t document_offset = marker_offset + marker.size();
    return payload.compare(document_offset, std::string::npos, serialized_document) == 0;
}

} // namespace

core::Result<void> WorkspaceState::validate() const {
    if (visible_panes.empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "workspace must expose at least one pane"));
    }
    std::set<Pane> unique_panes;
    for (const Pane pane : visible_panes) {
        const bool known_pane = pane == Pane::outliner || pane == Pane::viewport ||
            pane == Pane::inspector || pane == Pane::problems || pane == Pane::history;
        if (!known_pane) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "workspace contains an unknown pane"));
        }
        if (!unique_panes.insert(pane).second) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_argument,
                "workspace cannot contain duplicate panes"));
        }
    }
    if (!unique_panes.contains(Pane::viewport)) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "workspace must keep the viewport pane visible"));
    }
    return core::Result<void>::success();
}

ApplicationSession::ApplicationSession() {
    static_cast<void>(tools_.register_builtin_tools());
    saved_revision_ = document_.revision();
}

core::Result<ApplicationSession::PreparedJournal> ApplicationSession::prepare_journal(
    const std::filesystem::path& project_path,
    const project::ProjectDocument& document) const {
    if (project_path.empty() || project_path.filename().empty()) {
        return core::Result<PreparedJournal>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "journal binding requires a project file path"));
    }

    const auto path = journal_path_for(project_path);
    std::error_code exists_error;
    const bool existed = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return core::Result<PreparedJournal>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }

    journal::Journal candidate(path);
    const auto entries = candidate.read_all();
    if (!entries) {
        return core::Result<PreparedJournal>::failure(entries.error().with_context(
            "project journal verification"));
    }

    if (!entries.value().empty()) {
        const core::Revision tail = entries.value().back().revision_after;
        if (tail != document.revision()) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "project revision does not match the durable journal; recovery is required"));
        }
        const std::string serialized = document.serialize();
        if (!journal_entry_matches_document(entries.value().back(), serialized)) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project contents do not match the latest durable journal snapshot"));
        }
        return core::Result<PreparedJournal>::success(
            PreparedJournal{std::move(candidate), false});
    }

    if (document.revision().value() != 0U) {
        const std::string snapshot = document.serialize();
        if (snapshot.size() > kMaxJournalPayloadBytes) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project snapshot exceeds the durable journal payload limit"));
        }
        const std::string payload_text = document_snapshot_payload(
            "CARTOGRAPHER_PROJECT_SNAPSHOT_V1", {}, snapshot);
        if (payload_text.size() > kMaxJournalPayloadBytes) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::validation_failed,
                "project snapshot journal envelope exceeds the payload limit"));
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
        const std::span<const std::uint8_t> payload(bytes, payload_text.size());
        const auto baseline = candidate.append(journal::JournalAppend{
            core::Revision{},
            document.revision(),
            std::string(kSnapshotEvent),
            payload,
            std::nullopt,
        });
        if (!baseline) {
            return core::Result<PreparedJournal>::failure(baseline.error().with_context(
                "project journal baseline"));
        }
    }

    return core::Result<PreparedJournal>::success(
        PreparedJournal{std::move(candidate), !existed});
}

core::Result<void> ApplicationSession::append_mutation_event(
    std::string_view action,
    core::Revision revision_before,
    core::Revision revision_after) {
    if (!journal_.has_value() || revision_before == revision_after) {
        return core::Result<void>::success();
    }
    const std::string serialized_document = document_.serialize();
    const std::string payload_text = document_snapshot_payload(
        "CARTOGRAPHER_APPLICATION_MUTATION_V1", action, serialized_document);
    if (payload_text.size() > kMaxJournalPayloadBytes) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::validation_failed,
            "application mutation snapshot exceeds the durable journal payload limit"));
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload_text.data());
    const std::span<const std::uint8_t> payload(bytes, payload_text.size());
    const auto appended = journal_->append(journal::JournalAppend{
        revision_before,
        revision_after,
        std::string(kActionEvent),
        payload,
        std::nullopt,
    });
    if (!appended) {
        return core::Result<void>::failure(appended.error().with_context(
            "durable application journal append"));
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::dispatch(const ApplicationAction& action) {
    return std::visit(
        [this](const auto& value) -> core::Result<DispatchReceipt> {
            using Action = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Action, NewProjectAction>) {
                return new_project(value);
            } else if constexpr (std::is_same_v<Action, OpenProjectAction>) {
                return open_project(value);
            } else if constexpr (std::is_same_v<Action, SaveProjectAction>) {
                return save_project(value);
            } else if constexpr (std::is_same_v<Action, SetWorkspaceAction>) {
                const core::Revision before = document_.revision();
                if (auto result = set_workspace(value.workspace); !result) {
                    return failure(result.error().with_context("workspace update"));
                }
                return accepted("Set Workspace", before);
            } else if constexpr (std::is_same_v<Action, SetSelectionModeAction>) {
                const core::Revision before = document_.revision();
                if (auto result = selection_.set_mode(value.mode); !result) {
                    return failure(result.error().with_context("selection mode"));
                }
                return accepted("Set Selection Mode", before);
            } else if constexpr (std::is_same_v<Action, SelectObjectAction>) {
                return select_object(value);
            } else if constexpr (std::is_same_v<Action, SelectVertexAction>) {
                return select_vertex(value);
            } else if constexpr (std::is_same_v<Action, SelectFaceAction>) {
                return select_face(value);
            } else if constexpr (std::is_same_v<Action, SetObjectTransformAction>) {
                const auto* object = document_.scene().find(value.object);
                if (!object) {
                    return failure(core::Diagnostic(
                        core::ErrorCode::not_found,
                        "cannot transform a missing scene object"));
                }
                if (object->locked) {
                    return failure(invalid_state("locked scene objects cannot be transformed"));
                }
                return execute_command(
                    std::make_unique<editor::SetProjectObjectTransformCommand>(
                        document_, value.object, object->local_transform, value.transform),
                    "Set Object Transform");
            } else if constexpr (std::is_same_v<Action, InvokeToolAction>) {
                return invoke_tool(value);
            } else if constexpr (std::is_same_v<Action, CreateBoxAction>) {
                return create_box(value);
            } else if constexpr (std::is_same_v<Action, CreatePlaneAction>) {
                return create_plane(value);
            } else if constexpr (std::is_same_v<Action, UndoAction>) {
                return undo();
            } else {
                return redo();
            }
        },
        action);
}

core::Result<void> ApplicationSession::set_workspace(WorkspaceState workspace) {
    if (auto result = workspace.validate(); !result) {
        return result;
    }
    workspace_ = std::move(workspace);
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::new_project(const NewProjectAction& action) {
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    auto created = project::ProjectDocument::create(action.name);
    if (!created) {
        return failure(created.error());
    }
    const core::Revision before = document_.revision();
    document_ = std::move(created.value());
    project_path_.reset();
    journal_.reset();
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    return accepted("New Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::open_project(const OpenProjectAction& action) {
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    auto loaded = project::ProjectDocument::load(action.path);
    if (!loaded) {
        return failure(loaded.error().with_context("open project"));
    }
    auto prepared_journal = prepare_journal(action.path, loaded.value());
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("open project journal"));
    }
    const core::Revision before = document_.revision();
    document_ = std::move(loaded.value());
    project_path_ = action.path;
    journal_ = std::move(prepared_journal.value().journal);
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    return accepted("Open Project", before);
}

core::Result<void> ApplicationSession::can_close(bool discard_dirty) const {
    return require_discard_confirmation(discard_dirty);
}

core::Result<void> ApplicationSession::require_discard_confirmation(bool discard_dirty) const {
    if (!discard_dirty && document_.revision() != saved_revision_) {
        return core::Result<void>::failure(invalid_state(
            "unsaved project changes require explicit discard confirmation"));
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::save_project(const SaveProjectAction& action) {
    const std::filesystem::path* selected = nullptr;
    if (action.path.has_value()) {
        selected = &action.path.value();
    } else if (project_path_.has_value()) {
        selected = &project_path_.value();
    }
    if (!selected) {
        return failure(invalid_state("save requires a project path"));
    }
    auto prepared_journal = prepare_journal(*selected, document_);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("save project journal"));
    }
    const auto saved = document_.save_atomic(*selected);
    if (!saved) {
        if (prepared_journal.value().created_file) {
            std::error_code ignored;
            std::filesystem::remove(prepared_journal.value().journal.path(), ignored);
        }
        return failure(saved.error().with_context("save project"));
    }
    const core::Revision before = document_.revision();
    project_path_ = *selected;
    journal_ = std::move(prepared_journal.value().journal);
    saved_revision_ = document_.revision();
    return accepted("Save Project", before);
}

core::Result<DispatchReceipt> ApplicationSession::execute_command(
    std::unique_ptr<editor::EditorCommand> command,
    std::string action) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.execute(std::move(command)); !result) {
        return failure(result.error().with_context(action));
    }
    return accept_command_mutation(std::move(action), before, std::move(before_document));
}

core::Result<DispatchReceipt> ApplicationSession::accept_command_mutation(
    std::string action,
    core::Revision revision_before,
    project::ProjectDocument before_document) {
    const core::Revision revision_after = document_.revision();
    if (auto result = append_mutation_event(action, revision_before, revision_after); !result) {
        const auto rollback = history_.rollback_last_execute();
        document_ = std::move(before_document);
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and command rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("mutation rolled back"));
    }
    return accepted(std::move(action), revision_before);
}

core::Result<DispatchReceipt> ApplicationSession::select_object(const SelectObjectAction& action) {
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_object(document_.scene(), action.object, action.operation); !result) {
        return failure(result.error().with_context("object selection"));
    }
    return accepted("Select Object", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_vertex(const SelectVertexAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select a vertex on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_vertex(
            document_.scene(), action.object, mesh->second, action.vertex, action.operation);
        !result) {
        return failure(result.error().with_context("vertex selection"));
    }
    return accepted("Select Vertex", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_face(const SelectFaceAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select a face on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_face(
            document_.scene(), action.object, mesh->second, action.face, action.operation);
        !result) {
        return failure(result.error().with_context("face selection"));
    }
    return accepted("Select Face", before);
}

core::Result<DispatchReceipt> ApplicationSession::invoke_tool(const InvokeToolAction& action) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    editor::ToolContext context(document_, selection_);
    if (auto result = tools_.invoke(action.tool_id, context, action.arguments, history_); !result) {
        return failure(result.error().with_context("tool invocation"));
    }
    return accept_command_mutation(
        "Invoke Tool: " + action.tool_id,
        before,
        std::move(before_document));
}

core::Result<DispatchReceipt> ApplicationSession::create_box(const CreateBoxAction& action) {
    auto mesh = geometry::make_box(action.size);
    if (!mesh) {
        return failure(mesh.error().with_context("create box"));
    }
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            document_, action.object_name, std::move(mesh.value())),
        "Create Box");
}

core::Result<DispatchReceipt> ApplicationSession::create_plane(const CreatePlaneAction& action) {
    auto mesh = geometry::make_plane(action.width, action.depth);
    if (!mesh) {
        return failure(mesh.error().with_context("create plane"));
    }
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            document_, action.object_name, std::move(mesh.value())),
        "Create Plane");
}

core::Result<DispatchReceipt> ApplicationSession::undo() {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.undo(); !result) {
        return failure(result.error().with_context("undo"));
    }
    if (auto result = append_mutation_event("Undo", before, document_.revision()); !result) {
        const auto rollback = history_.redo();
        document_ = std::move(before_document);
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and undo rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("undo rolled back"));
    }
    return accepted("Undo", before);
}

core::Result<DispatchReceipt> ApplicationSession::redo() {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.redo(); !result) {
        return failure(result.error().with_context("redo"));
    }
    if (auto result = append_mutation_event("Redo", before, document_.revision()); !result) {
        const auto rollback = history_.undo();
        document_ = std::move(before_document);
        if (!rollback) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "durable journal append failed and redo rollback failed: " +
                    rollback.error().message));
        }
        return failure(result.error().with_context("redo rolled back"));
    }
    return accepted("Redo", before);
}

core::Result<DispatchReceipt> ApplicationSession::accepted(
    std::string action,
    core::Revision revision_before) const {
    return core::Result<DispatchReceipt>::success(DispatchReceipt{
        std::move(action), revision_before, document_.revision(),
        revision_before != document_.revision()});
}

core::Result<DispatchReceipt> ApplicationSession::failure(core::Diagnostic diagnostic) {
    if (problems_.size() >= kMaxProblems) {
        problems_.erase(problems_.begin());
    }
    problems_.push_back(diagnostic);
    return core::Result<DispatchReceipt>::failure(std::move(diagnostic));
}

void ApplicationSession::reset_editor_state() noexcept {
    selection_.clear();
    history_.clear_history();
    problems_.clear();
}

ApplicationSnapshot ApplicationSession::snapshot() const {
    ApplicationSnapshot result;
    result.project_name = document_.name();
    result.project_path = project_path_;
    result.project_generation = project_generation_;
    result.project_revision = document_.revision();
    result.dirty = document_.revision() != saved_revision_;
    result.workspace = workspace_;
    result.tools = tools_.descriptors();
    result.undo_count = history_.undo_count();
    result.redo_count = history_.redo_count();
    result.problems = problems_;
    const auto append_problem = [&result](core::Diagnostic diagnostic) {
        if (result.problems.size() >= kMaxProblems) {
            result.problems.erase(result.problems.begin());
        }
        result.problems.push_back(std::move(diagnostic));
    };

    for (const auto& object : document_.scene().objects_sorted()) {
        auto world = document_.scene().world_transform(object.id);
        if (!world) {
            append_problem(world.error().with_context("scene snapshot"));
            continue;
        }
        result.objects.push_back(ObjectView{object, world.value()});
    }

    result.selection.mode = selection_.mode();
    result.selection.objects = selection_.selected_objects();
    result.selection.vertices = selection_.selected_vertices();
    result.selection.faces = selection_.selected_faces();
    result.selection.component_object = selection_.component_object();
    result.selection.component_mesh_revision = selection_.component_mesh_revision();
    result.viewport.project_revision = document_.revision();

    render::RenderScene viewport_scene;
    for (const auto& object : document_.scene().objects_sorted()) {
        if (!object.visible || !object.mesh_asset.has_value()) {
            continue;
        }
        const auto mesh = document_.meshes().find(*object.mesh_asset);
        if (mesh == document_.meshes().end()) {
            result.viewport.error = core::Diagnostic(
                core::ErrorCode::stale_data,
                "scene object references a missing mesh asset");
            break;
        }
        auto compiled = mesh->second.compile();
        if (!compiled) {
            result.viewport.error = compiled.error().with_context("viewport compilation");
            break;
        }
        auto world = document_.scene().world_transform(object.id);
        if (!world) {
            result.viewport.error = world.error().with_context("viewport transform");
            break;
        }
        auto compiled_mesh = std::make_shared<geometry::CompiledMesh>(std::move(compiled.value()));
        if (auto inserted = viewport_scene.upsert({
                object.id, compiled_mesh, world.value(), compiled_mesh->source_revision});
            !inserted) {
            result.viewport.error = inserted.error().with_context("viewport snapshot");
            break;
        }
    }
    if (!result.viewport.error.has_value()) {
        result.viewport.scene = std::move(viewport_scene);
    }
    return result;
}

} // namespace carto::application
