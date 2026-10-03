#include <carto/application/application.hpp>
#include <carto/project/recovery.hpp>

#include <charconv>
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
// ProjectDocument bounds serialized projects at 128 MiB. Keep the durable
// snapshot envelope capable of carrying that governed project state so large
// imported anatomy meshes do not require an unjournaled mutation path.
constexpr std::size_t kMaxJournalPayloadBytes = 128U * 1024U * 1024U;
constexpr std::string_view kSnapshotEvent = "cartographer.snapshot";
constexpr std::string_view kActionEvent = "cartographer.application_action";

journal::JournalLimits application_journal_limits() {
    journal::JournalLimits limits;
    limits.max_payload_bytes = kMaxJournalPayloadBytes;
    limits.max_record_bytes = 256ULL * 1024ULL * 1024ULL;
    limits.max_file_bytes = 1024ULL * 1024ULL * 1024ULL;
    return limits;
}

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
    const bool is_snapshot = entry.event_type == kSnapshotEvent;
    const bool is_mutation = entry.event_type == kActionEvent;
    if (!is_snapshot && !is_mutation) return false;

    std::size_t cursor = 0U;
    const auto next_line = [&payload, &cursor]() -> std::optional<std::string_view> {
        const std::size_t end = payload.find('\n', cursor);
        if (end == std::string::npos) return std::nullopt;
        const std::string_view line(payload.data() + cursor, end - cursor);
        cursor = end + 1U;
        return line;
    };
    const auto header = next_line();
    if (!header.has_value() || *header !=
            (is_snapshot ? "CARTOGRAPHER_PROJECT_SNAPSHOT_V1"
                         : "CARTOGRAPHER_APPLICATION_MUTATION_V1")) {
        return false;
    }
    if (is_mutation) {
        const auto operation = next_line();
        if (!operation.has_value() || !operation->starts_with("operation=") ||
            operation->size() == std::string_view("operation=").size() ||
            operation->find_first_of("\r\n") != std::string_view::npos) {
            return false;
        }
    }
    const auto document_bytes = next_line();
    constexpr std::string_view kDocumentBytes = "document_bytes=";
    if (!document_bytes.has_value() || !document_bytes->starts_with(kDocumentBytes)) {
        return false;
    }
    const std::string_view value = document_bytes->substr(kDocumentBytes.size());
    if (value.empty()) return false;
    std::uint64_t declared_bytes = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), declared_bytes);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        declared_bytes != serialized_document.size()) {
        return false;
    }
    return std::string_view(payload).substr(cursor) == serialized_document;
}

} // namespace

core::Result<DispatchReceipt> HumanApplicationAccess::dispatch(
    ApplicationSession& session,
    const ApplicationAction& action) {
    return session.dispatch(HumanActionAdmission{}, action);
}

core::Result<DispatchReceipt> HumanApplicationAccess::commit_preview(
    ApplicationSession& session,
    editor::AuthoringPreview& preview) {
    return session.commit_preview(HumanActionAdmission{}, preview);
}

core::Result<DispatchReceipt> HumanApplicationAccess::commit_preview(
    ApplicationSession& session,
    editor::AuthoringPreview& preview,
    OperationSource source,
    std::string_view provenance_id) {
    return session.commit_preview(HumanActionAdmission{}, preview, source, provenance_id);
}

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

core::Result<void> RecoveryInspection::validate() const {
    if (project_path.empty() || project_path.filename().empty() ||
        journal_path.empty() || journal_path.filename().empty()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery inspection paths must name files"));
    }
    if (journal_entry_count != journal_entries.size()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "recovery inspection entry count does not match its entries"));
    }
    if (journal_entries.empty()) {
        if (durable_revision != core::Revision{}) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "empty recovery inspection cannot have a durable revision"));
        }
    } else if (durable_revision != journal_entries.back().revision_after) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "recovery inspection durable revision does not match its journal tail"));
    }
    if (state == RecoveryInspectionState::blocked && !diagnostic.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "blocked recovery inspection requires a diagnostic"));
    }
    if (state != RecoveryInspectionState::blocked && diagnostic.has_value() &&
        state == RecoveryInspectionState::clean) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "clean recovery inspection cannot carry a blocking diagnostic"));
    }
    for (const auto& entry : journal_entries) {
        if (entry.event_type.empty() || entry.revision_after <= entry.revision_before) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "recovery inspection contains an invalid journal summary"));
        }
    }
    return core::Result<void>::success();
}

ApplicationSession::ApplicationSession() {
    static_cast<void>(tools_.register_builtin_tools());
    saved_revision_ = document_.revision();
}

core::Result<RecoveryInspection> ApplicationSession::inspect_recovery(
    const std::filesystem::path& project_path) {
    if (project_path.empty() || project_path.filename().empty()) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery inspection requires a project file path"));
    }

    RecoveryInspection inspection;
    inspection.project_path = project_path;
    inspection.journal_path = journal_path_for(project_path);

    std::error_code error;
    const bool project_exists = std::filesystem::exists(project_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project path"));
    }
    const bool journal_exists = std::filesystem::exists(inspection.journal_path, error);
    if (error) {
        return core::Result<RecoveryInspection>::failure(core::Diagnostic(
            core::ErrorCode::io_error,
            "unable to inspect the project journal path"));
    }

    // Inspect the project and its journal under the same writer lease used by
    // save, mutation publication, and recovery. Without this lease a writer
    // could replace the project between the project load and journal read,
    // producing a recovery classification for two different revisions.
    std::optional<project::FileLock> project_lock;
    if (project_exists || journal_exists) {
        project_lock.emplace(project_path);
        if (auto result = project_lock->acquire(); !result) {
            return core::Result<RecoveryInspection>::failure(result.error().with_context(
                "recovery inspection lock"));
        }
    }

    if (project_exists) {
        const auto document = project::ProjectDocument::load(project_path);
        if (!document) {
            inspection.state = RecoveryInspectionState::blocked;
            inspection.diagnostic = document.error().with_context(
                "recovery inspection project load");
        } else {
            inspection.project_revision = document.value().revision();
        }
    }

    if (!journal_exists) {
        if (inspection.state != RecoveryInspectionState::blocked) {
            inspection.state = RecoveryInspectionState::unavailable;
        }
        if (auto result = inspection.validate(); !result) {
            return core::Result<RecoveryInspection>::failure(result.error());
        }
        return core::Result<RecoveryInspection>::success(std::move(inspection));
    }

    journal::Journal journal(inspection.journal_path, application_journal_limits());
    const auto entries = journal.read_all();
    if (!entries) {
        inspection.state = RecoveryInspectionState::blocked;
        inspection.diagnostic = entries.error().with_context(
            "recovery inspection journal verification");
    } else {
        inspection.journal_entry_count = entries.value().size();
        inspection.journal_entries.reserve(entries.value().size());
        for (const auto& entry : entries.value()) {
            inspection.journal_entries.push_back(RecoveryJournalEntry{
                entry.revision_before, entry.revision_after, entry.event_type});
        }
        if (!entries.value().empty()) {
            inspection.durable_revision = entries.value().back().revision_after;
        }

        if (inspection.state != RecoveryInspectionState::blocked) {
            if (!project_exists) {
                inspection.state = RecoveryInspectionState::pending;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::not_found,
                    "durable journal exists but the project file is missing");
            } else if (entries.value().empty()) {
                if (inspection.project_revision.value_or(core::Revision{}) == core::Revision{}) {
                    inspection.state = RecoveryInspectionState::clean;
                } else {
                    inspection.state = RecoveryInspectionState::blocked;
                    inspection.diagnostic = core::Diagnostic(
                        core::ErrorCode::stale_data,
                        "project has a revision but its durable journal is empty");
                }
            } else if (inspection.project_revision.value() < inspection.durable_revision) {
                inspection.state = RecoveryInspectionState::pending;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "durable journal is ahead of the saved project");
            } else if (inspection.project_revision.value() > inspection.durable_revision) {
                inspection.state = RecoveryInspectionState::blocked;
                inspection.diagnostic = core::Diagnostic(
                    core::ErrorCode::stale_data,
                    "saved project is ahead of its durable journal");
            } else {
                const auto document = project::ProjectDocument::load(project_path);
                if (!document || !journal_entry_matches_document(
                        entries.value().back(), document.value().serialize())) {
                    inspection.state = RecoveryInspectionState::blocked;
                    inspection.diagnostic = core::Diagnostic(
                        core::ErrorCode::validation_failed,
                        "durable journal tail does not match the saved project");
                } else {
                    inspection.state = RecoveryInspectionState::clean;
                }
            }
        }
    }

    if (auto result = inspection.validate(); !result) {
        return core::Result<RecoveryInspection>::failure(result.error());
    }
    return core::Result<RecoveryInspection>::success(std::move(inspection));
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
    std::uintmax_t original_bytes = 0U;
    if (existed) {
        std::error_code size_error;
        original_bytes = std::filesystem::file_size(path, size_error);
        if (size_error) {
            return core::Result<PreparedJournal>::failure(core::Diagnostic(
                core::ErrorCode::io_error,
                "unable to inspect the project journal size"));
        }
    }

    journal::Journal candidate(path, application_journal_limits());
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
            PreparedJournal{std::move(candidate), false, false, original_bytes});
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
        return core::Result<PreparedJournal>::success(
            PreparedJournal{std::move(candidate), !existed, true, original_bytes});
    }

    return core::Result<PreparedJournal>::success(
        PreparedJournal{std::move(candidate), !existed, false, original_bytes});
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
    const auto append = [&]() -> core::Result<void> {
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
    };
    if (project_path_.has_value()) {
        project::FileLock project_lock(*project_path_);
        if (auto result = project_lock.acquire(); !result) {
            return core::Result<void>::failure(result.error().with_context(
                "project mutation lock"));
        }
        return append();
    }
    return append();
}

core::Result<DispatchReceipt> ApplicationSession::dispatch(
    HumanActionAdmission,
    const ApplicationAction& action,
    OperationSource source,
    std::string_view provenance_id) {
    const OperationSource previous_source = active_source_;
    std::string previous_provenance = std::move(active_provenance_id_);
    active_source_ = source;
    active_provenance_id_ = std::string(provenance_id);
    struct SourceRestore final {
        ApplicationSession* session;
        OperationSource source;
        std::string provenance;
        ~SourceRestore() {
            session->active_source_ = source;
            session->active_provenance_id_ = std::move(provenance);
        }
    } restore{this, previous_source, std::move(previous_provenance)};
    return std::visit(
        [this](const auto& value) -> core::Result<DispatchReceipt> {
            using Action = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Action, NewProjectAction>) {
                return new_project(value);
            } else if constexpr (std::is_same_v<Action, OpenProjectAction>) {
                return open_project(value);
            } else if constexpr (std::is_same_v<Action, RecoverProjectAction>) {
                return recover_project(value);
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
            } else if constexpr (std::is_same_v<Action, ConvertSelectionAction>) {
                return convert_selection(value);
            } else if constexpr (std::is_same_v<Action, ExpandSelectionAction>) {
                return expand_selection(value);
            } else if constexpr (std::is_same_v<Action, SelectShortestPathAction>) {
                return select_shortest_path(value);
            } else if constexpr (std::is_same_v<Action, SelectObjectAction>) {
                return select_object(value);
            } else if constexpr (std::is_same_v<Action, SelectVertexAction>) {
                return select_vertex(value);
            } else if constexpr (std::is_same_v<Action, SelectFaceAction>) {
                return select_face(value);
            } else if constexpr (std::is_same_v<Action, SelectEdgeAction>) {
                return select_edge(value);
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
                editor::ProjectCommandAdmission admission;
                return execute_command(
                    std::make_unique<editor::SetProjectObjectTransformCommand>(
                        admission,
                        document_, value.object, object->local_transform, value.transform),
                    "Set Object Transform",
                    AffectedSet{{value.object}, {}, {}, {}},
                    ParameterPayload{
                        value.transform, std::nullopt, std::nullopt, std::nullopt,
                        std::nullopt, std::nullopt});
            } else if constexpr (std::is_same_v<Action, InvokeToolAction>) {
                return invoke_tool(value);
            } else if constexpr (std::is_same_v<Action, RepeatLastToolAction>) {
                return repeat_last_tool();
            } else if constexpr (std::is_same_v<Action, AdjustLastToolAction>) {
                return adjust_last_tool(value);
            } else if constexpr (std::is_same_v<Action, CreateBoxAction>) {
                return create_box(value);
            } else if constexpr (std::is_same_v<Action, CreatePlaneAction>) {
                return create_plane(value);
            } else if constexpr (std::is_same_v<Action, CreateMeshObjectAction>) {
                return create_mesh_object(value);
            } else if constexpr (std::is_same_v<Action, UndoAction>) {
                return undo();
            } else {
                return redo();
            }
        },
        action);
}

core::Result<editor::AuthoringContext> ApplicationSession::authoring_context() const {
    auto context = editor::AuthoringContext::from_selection(document_.revision(), selection_);
    if (!context) {
        return context;
    }

    if (selection_.mode() == editor::SelectionMode::object) {
        if (auto result = selection_.validate(document_.scene(), nullptr); !result) {
            return core::Result<editor::AuthoringContext>::failure(result.error().with_context(
                "authoring selection"));
        }
        return context;
    }

    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context has no owning scene object"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return core::Result<editor::AuthoringContext>::failure(invalid_state(
            "component authoring context owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return core::Result<editor::AuthoringContext>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "component authoring context references a missing mesh asset"));
    }
    if (auto result = selection_.validate(document_.scene(), &mesh->second); !result) {
        return core::Result<editor::AuthoringContext>::failure(result.error().with_context(
            "authoring selection"));
    }
    return context;
}

core::Result<editor::AuthoringPreview> ApplicationSession::begin_preview(
    editor::PreviewKind kind) const {
    auto context = authoring_context();
    if (!context) {
        return core::Result<editor::AuthoringPreview>::failure(context.error());
    }
    return editor::AuthoringPreview::begin(document_.revision(), context.value(), kind);
}

core::Result<ApplicationAction> ApplicationSession::preview_action(
    const editor::AuthoringPreview& preview) const {
    const auto& context = preview.context();
    const auto& parameters = preview.parameters();
    if (preview.kind() == editor::PreviewKind::object_transform) {
        if (!parameters.transform.has_value() || context.objects.size() != 1U) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "object transform preview has no complete action payload"));
        }
        return core::Result<ApplicationAction>::success(SetObjectTransformAction{
            context.objects.front(), *parameters.transform});
    }
    if (preview.kind() == editor::PreviewKind::extrude_face) {
        if (!parameters.distance.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face extrusion preview has no distance"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.extrude-face", editor::ToolArguments{
                *parameters.distance, {}, true, 0.5, std::nullopt}});
    }
    if (preview.kind() == editor::PreviewKind::inset_face) {
        if (!parameters.distance.has_value()) {
            return core::Result<ApplicationAction>::failure(invalid_state(
                "face inset preview has no distance"));
        }
        return core::Result<ApplicationAction>::success(InvokeToolAction{
            "mesh.inset-face", editor::ToolArguments{
                *parameters.distance, {}, true, 0.5, std::nullopt}});
    }
    if (!parameters.position.has_value()) {
        return core::Result<ApplicationAction>::failure(invalid_state(
            "vertex position preview has no position"));
    }
    return core::Result<ApplicationAction>::success(InvokeToolAction{
        "mesh.set-vertex-position", editor::ToolArguments{
            0.0, *parameters.position, true, 0.5, std::nullopt}});
}

core::Result<DispatchReceipt> ApplicationSession::commit_preview(
    HumanActionAdmission admission,
    editor::AuthoringPreview& preview,
    OperationSource source,
    std::string_view provenance_id) {
    if (auto result = preview.validate_commit(document_.revision()); !result) {
        return failure(result.error().with_context("preview commit"));
    }
    auto current_context = authoring_context();
    if (!current_context) {
        return failure(current_context.error().with_context("preview commit context"));
    }
    if (current_context.value() != preview.context()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "authoring preview context no longer matches the current selection"));
    }
    auto action = preview_action(preview);
    if (!action) {
        return failure(action.error().with_context("preview commit action"));
    }
    auto result = dispatch(admission, action.value(), source, provenance_id);
    if (result) {
        preview.mark_committed();
    }
    return result;
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
    project::FileLock project_lock(action.path);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project read lock"));
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

core::Result<DispatchReceipt> ApplicationSession::recover_project(
    const RecoverProjectAction& action) {
    if (active_source_ != OperationSource::human) {
        return failure(invalid_state(
            "journal recovery requires an explicit human application action"));
    }
    if (auto result = require_discard_confirmation(action.discard_dirty); !result) {
        return failure(result.error());
    }
    if (action.path.empty() || action.path.filename().empty()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "recovery requires a project file path"));
    }

    project::FileLock project_lock(action.path);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project recovery lock"));
    }
    journal::Journal journal(journal_path_for(action.path), application_journal_limits());
    const auto recovered = project::recover_latest_snapshot(journal);
    if (!recovered) {
        return failure(recovered.error().with_context("recover project"));
    }
    auto prepared_journal = prepare_journal(action.path, recovered.value().document);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("recover project journal"));
    }
    const auto saved = recovered.value().document.save_atomic_unlocked(action.path);
    if (!saved) {
        return failure(saved.error().with_context("recover project save"));
    }

    const core::Revision before = document_.revision();
    document_ = recovered.value().document;
    project_path_ = action.path;
    journal_ = std::move(prepared_journal.value().journal);
    reset_editor_state();
    saved_revision_ = document_.revision();
    if (project_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        project_generation_ = 0;
    } else {
        ++project_generation_;
    }
    const OperationSource previous_source = active_source_;
    active_source_ = OperationSource::recovery;
    auto receipt = accepted("Recover Project", before);
    active_source_ = previous_source;
    if (receipt) {
        // Recovery can restore a revision already present in the active
        // session while still replacing the complete document state.
        receipt.value().document_changed = true;
    }
    return receipt;
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
    project::FileLock project_lock(*selected);
    if (auto result = project_lock.acquire(); !result) {
        return failure(result.error().with_context("project write lock"));
    }
    auto prepared_journal = prepare_journal(*selected, document_);
    if (!prepared_journal) {
        return failure(prepared_journal.error().with_context("save project journal"));
    }
    const auto saved = document_.save_atomic_unlocked(*selected);
    if (!saved) {
        std::error_code cleanup_error;
        if (prepared_journal.value().created_file) {
            std::filesystem::remove(prepared_journal.value().journal.path(), cleanup_error);
        } else if (prepared_journal.value().appended_baseline) {
            std::filesystem::resize_file(
                prepared_journal.value().journal.path(),
                prepared_journal.value().original_bytes,
                cleanup_error);
        }
        if (cleanup_error) {
            return failure(core::Diagnostic(
                core::ErrorCode::invalid_state,
                "project save failed and durable journal rollback failed: " +
                    cleanup_error.message()));
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
    std::string action,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    if (auto result = history_.execute(std::move(command)); !result) {
        return failure(result.error().with_context(action));
    }
    return accept_command_mutation(
        std::move(action), before, std::move(before_document), std::move(affected),
        std::move(parameters));
}

core::Result<DispatchReceipt> ApplicationSession::accept_command_mutation(
    std::string action,
    core::Revision revision_before,
    project::ProjectDocument before_document,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
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
    return accepted(
        std::move(action), revision_before, std::move(affected), std::move(parameters));
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

core::Result<DispatchReceipt> ApplicationSession::select_edge(const SelectEdgeAction& action) {
    const auto* object = document_.scene().find(action.object);
    if (!object) {
        return failure(core::Diagnostic(core::ErrorCode::not_found, "cannot select on a missing object"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state("cannot select an edge on an object without a mesh"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(core::ErrorCode::stale_data, "object references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_edge(
            document_.scene(), action.object, mesh->second, action.edge, action.operation);
        !result) {
        return failure(result.error().with_context("edge selection"));
    }
    return accepted("Select Edge", before);
}

core::Result<DispatchReceipt> ApplicationSession::convert_selection(
    const ConvertSelectionAction& action) {
    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "selection conversion requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection conversion owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "selection conversion owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection conversion owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.convert_mode(action.mode, mesh->second); !result) {
        return failure(result.error().with_context("selection conversion"));
    }
    return accepted("Convert Selection", before);
}

core::Result<DispatchReceipt> ApplicationSession::expand_selection(
    const ExpandSelectionAction& action) {
    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "selection expansion requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection expansion owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "selection expansion owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selection expansion owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.expand(action.expansion, mesh->second); !result) {
        return failure(result.error().with_context("selection expansion"));
    }
    return accepted("Expand Selection", before);
}

core::Result<DispatchReceipt> ApplicationSession::select_shortest_path(
    const SelectShortestPathAction& action) {
    const auto owner = selection_.component_object();
    if (!owner.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_argument,
            "shortest-path selection requires an object-bound component selection"));
    }
    const auto* object = document_.scene().find(*owner);
    if (!object) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shortest-path selection owner object is no longer present"));
    }
    if (!object->mesh_asset.has_value()) {
        return failure(invalid_state(
            "shortest-path selection owner has no mesh asset"));
    }
    const auto mesh = document_.meshes().find(*object->mesh_asset);
    if (mesh == document_.meshes().end()) {
        return failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "shortest-path selection owner references a missing mesh"));
    }
    const core::Revision before = document_.revision();
    if (auto result = selection_.select_shortest_path(action.goal, mesh->second); !result) {
        return failure(result.error().with_context("shortest-path selection"));
    }
    return accepted("Select Shortest Path", before);
}

core::Result<DispatchReceipt> ApplicationSession::invoke_tool(const InvokeToolAction& action) {
    const core::Revision before = document_.revision();
    project::ProjectDocument before_document = document_;
    editor::ProjectCommandAdmission admission;
    editor::ToolContext context(admission, document_, selection_);
    AffectedSet affected;
    std::optional<ParameterPayload> parameters;
    std::optional<editor::AuthoringContext> invocation_context;
    if (auto authoring = authoring_context(); authoring) {
        invocation_context = authoring.value();
        affected.objects = authoring.value().objects;
        affected.vertices = authoring.value().vertices;
        affected.edges = authoring.value().edges;
        affected.faces = authoring.value().faces;
        if (authoring.value().component_object.has_value()) {
            affected.objects.push_back(*authoring.value().component_object);
        }
        if (action.tool_id == "mesh.extrude-face") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.inset-face") {
            parameters = ParameterPayload{
                std::nullopt,
                action.arguments.distance,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.remove-face") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.remove_orphaned_vertices,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.set-vertex-position") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                action.arguments.position,
                std::nullopt,
                std::nullopt,
                std::nullopt};
        } else if (action.tool_id == "mesh.split-edge") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.factor,
                std::nullopt};
        } else if (action.tool_id == "mesh.slide-vertex") {
            parameters = ParameterPayload{
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                action.arguments.factor,
                action.arguments.support_edge};
            if (action.arguments.support_edge.has_value()) {
                affected.edges.push_back(*action.arguments.support_edge);
            }
        }
    }
    if (auto result = tools_.invoke(action.tool_id, context, action.arguments, history_); !result) {
        return failure(result.error().with_context("tool invocation"));
    }
    auto result = accept_command_mutation(
        "Invoke Tool: " + action.tool_id,
        before,
        std::move(before_document),
        std::move(affected),
        std::move(parameters));
    if (result && (action.tool_id == "mesh.inset-face" ||
                   action.tool_id == "mesh.poke-face" ||
                   action.tool_id == "mesh.remove-face" ||
                   action.tool_id == "mesh.split-edge" ||
                   action.tool_id == "mesh.dissolve-edge" ||
                   action.tool_id == "mesh.tri-to-quad" ||
                   action.tool_id == "mesh.merge-vertices")) {
        selection_.clear();
    }
    if (result) {
        last_tool_action_ = action;
        last_tool_context_ = std::move(invocation_context);
    }
    return result;
}

core::Result<void> ApplicationSession::rebind_last_tool_selection() {
    if (!last_tool_action_.has_value() || !last_tool_context_.has_value()) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "repeat-last requires a previously accepted tool invocation"));
    }

    const auto& saved = *last_tool_context_;
    if (auto result = saved.validate(); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "repeat-last authoring context"));
    }

    selection_.clear();
    if (auto result = selection_.set_mode(saved.selection_mode); !result) {
        return core::Result<void>::failure(result.error().with_context(
            "repeat-last selection mode"));
    }

    if (saved.selection_mode == editor::SelectionMode::object) {
        for (const scene::ObjectId object : saved.objects) {
                if (auto result = selection_.select_object(
                    document_.scene(), object,
                    selection_.empty()
                        ? editor::SelectionOperation::replace
                    : editor::SelectionOperation::add);
                !result) {
                return core::Result<void>::failure(result.error().with_context(
                    "repeat-last object selection"));
            }
        }
    } else {
        if (!saved.component_object.has_value()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last component context has no owning scene object"));
        }
        const auto* object = document_.scene().find(*saved.component_object);
        if (!object) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last owner object is no longer present"));
        }
        if (!object->mesh_asset.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "repeat-last owner object has no mesh asset"));
        }
        const auto mesh = document_.meshes().find(*object->mesh_asset);
        if (mesh == document_.meshes().end()) {
            return core::Result<void>::failure(core::Diagnostic(
                core::ErrorCode::stale_data,
                "repeat-last owner object references a missing mesh"));
        }
        const auto bind = [&](auto&& ids, auto&& select) -> core::Result<void> {
            bool first = true;
            for (const auto id : ids) {
                if (auto result = select(
                        id,
                        first ? editor::SelectionOperation::replace
                              : editor::SelectionOperation::add);
                    !result) {
                    return result;
                }
                first = false;
            }
            return core::Result<void>::success();
        };
        core::Result<void> rebound = core::Result<void>::success();
        if (saved.selection_mode == editor::SelectionMode::vertex) {
            rebound = bind(saved.vertices, [&](geometry::VertexId id, editor::SelectionOperation op) {
                return selection_.select_vertex(document_.scene(), *saved.component_object,
                                                mesh->second, id, op);
            });
        } else if (saved.selection_mode == editor::SelectionMode::edge) {
            rebound = bind(saved.edges, [&](geometry::EdgeId id, editor::SelectionOperation op) {
                return selection_.select_edge(document_.scene(), *saved.component_object,
                                              mesh->second, id, op);
            });
        } else {
            rebound = bind(saved.faces, [&](geometry::FaceId id, editor::SelectionOperation op) {
                return selection_.select_face(document_.scene(), *saved.component_object,
                                              mesh->second, id, op);
            });
        }
        if (!rebound) {
            return core::Result<void>::failure(rebound.error().with_context(
                "repeat-last selection rebind"));
        }
    }
    return core::Result<void>::success();
}

core::Result<DispatchReceipt> ApplicationSession::repeat_last_tool() {
    const editor::SelectionState before_selection = selection_;
    if (auto result = rebind_last_tool_selection(); !result) {
        selection_ = before_selection;
        return failure(result.error());
    }
    auto result = invoke_tool(*last_tool_action_);
    if (!result) selection_ = before_selection;
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::adjust_last_tool(
    const AdjustLastToolAction& action) {
    if (!last_tool_action_.has_value() || !last_tool_context_.has_value()) {
        return failure(core::Diagnostic(
            core::ErrorCode::invalid_state,
            "adjust-last requires a previously accepted tool invocation"));
    }
    const editor::SelectionState before_selection = selection_;
    const InvokeToolAction previous_action = *last_tool_action_;
    const editor::AuthoringContext previous_context = *last_tool_context_;
    last_tool_action_->arguments = action.arguments;
    if (auto result = rebind_last_tool_selection(); !result) {
        last_tool_action_ = previous_action;
        last_tool_context_ = previous_context;
        selection_ = before_selection;
        return failure(result.error().with_context("adjust-last selection"));
    }
    auto result = invoke_tool(*last_tool_action_);
    if (!result) {
        last_tool_action_ = previous_action;
        last_tool_context_ = previous_context;
        selection_ = before_selection;
    }
    return result;
}

core::Result<DispatchReceipt> ApplicationSession::create_box(const CreateBoxAction& action) {
    auto mesh = geometry::make_box(action.size);
    if (!mesh) {
        return failure(mesh.error().with_context("create box"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, std::move(mesh.value())),
        "Create Box");
}

core::Result<DispatchReceipt> ApplicationSession::create_plane(const CreatePlaneAction& action) {
    auto mesh = geometry::make_plane(action.width, action.depth);
    if (!mesh) {
        return failure(mesh.error().with_context("create plane"));
    }
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, std::move(mesh.value())),
        "Create Plane");
}

core::Result<DispatchReceipt> ApplicationSession::create_mesh_object(
    const CreateMeshObjectAction& action) {
    editor::ProjectCommandAdmission admission;
    return execute_command(
        std::make_unique<editor::CreateMeshObjectCommand>(
            admission,
            document_, action.object_name, action.mesh),
        "Create Mesh Object");
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
    core::Revision revision_before,
    AffectedSet affected,
    std::optional<ParameterPayload> parameters) {
    const OperationId operation_id = next_operation_id_;
    next_operation_id_ = next_operation_id_ == std::numeric_limits<OperationId>::max()
        ? OperationId{1U}
        : next_operation_id_ + 1U;
    return core::Result<DispatchReceipt>::success(DispatchReceipt{
        std::move(action), revision_before, document_.revision(),
        revision_before != document_.revision(), operation_id, active_source_,
        active_provenance_id_,
        std::move(affected), std::move(parameters), {}});
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
    last_tool_action_.reset();
    last_tool_context_.reset();
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
    result.selection.edges = selection_.selected_edges();
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
