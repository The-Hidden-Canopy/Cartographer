#pragma once

#include <carto/ai/ai.hpp>
#include <carto/application/application.hpp>
#include <carto/agent/workbench.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/vegetation_preview.hpp>
#include <carto/core/result.hpp>
#include <carto/engineering/authoring.hpp>
#include <carto/market/market.hpp>
#include <carto/surface/core.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace carto::ui {

struct UiColor {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 1.0F;
};

struct UiColorTokens {
    UiColor canvas_0;
    UiColor canvas_1;
    UiColor panel_0;
    UiColor panel_1;
    UiColor panel_hover;
    UiColor panel_selected;
    UiColor text_primary;
    UiColor text_secondary;
    UiColor text_disabled;
    UiColor border_soft;
    UiColor border_focus;
    UiColor accent_primary;
    UiColor accent_secondary;
    UiColor semantic_success;
    UiColor semantic_warning;
    UiColor semantic_error;
    UiColor semantic_info;
    UiColor provisional;
    UiColor stale;
    UiColor modified;
};

enum class Theme {
    dark,
    light,
    high_contrast,
};

enum class Density {
    compact,
    standard,
    touch,
};

enum class OperatorMode {
    person_first,
    ai_first,
};

enum class Workspace {
    model,
    sculpt,
    engineering,
    condition,
    cad,
    build,
    material,
    animate,
    review,
    ai,
};

enum class Panel {
    scene,
    viewport,
    inspector,
    operations,
    graph,
    assets,
    timeline,
    problems,
    console,
};

enum class BottomPanel {
    operations,
    graph,
    assets,
    timeline,
    problems,
    console,
};

// A workspace definition is UI configuration, not project truth. The
// registry gives native shells and headless consumers one authoritative list
// of available/future workspaces and their bounded default layout.
struct WorkspaceDefinition {
    Workspace workspace = Workspace::model;
    std::string id;
    std::string label;
    std::vector<Panel> default_panels;
    BottomPanel default_bottom_panel = BottomPanel::operations;
    bool available = false;
    std::string unavailable_reason;

    [[nodiscard]] core::Result<void> validate() const;
};

enum class ProjectStatus {
    unsaved,
    saved,
    modified,
    render_error,
    ui_error,
};

enum class OperationCategory {
    committed_command,
    undo,
    redo,
    project,
    recovery,
    provider,
};

enum class Shortcut {
    save,
    undo,
    redo,
    repeat_last_tool,
    object_mode,
    vertex_mode,
    edge_mode,
    face_mode,
    toggle_command_palette,
    close_overlay,
    person_first,
    ai_first,
};

// The native shell translates platform key messages into this small, toolkit-
// independent contract.  It lets the authoritative UI controller own core
// shortcuts without depending on Dear ImGui frame state.
enum class NativeKey {
    s,
    z,
    y,
    r,
    k,
    digit_1,
    digit_2,
    digit_3,
    digit_4,
    escape,
};

inline constexpr std::uint8_t kNativeModifierControl = 1U << 0U;
inline constexpr std::uint8_t kNativeModifierShift = 1U << 1U;
inline constexpr std::uint8_t kNativeModifierAlt = 1U << 2U;

struct NativeKeyEvent {
    NativeKey key = NativeKey::escape;
    std::uint8_t modifiers = 0U;
    bool pressed = true;
    bool repeat = false;
};

[[nodiscard]] std::optional<Shortcut> shortcut_for_native_key(
    const NativeKeyEvent& event) noexcept;

// Native pointer coordinates are client-area pixels supplied by the platform
// shell.  The UI layer only interprets the safe selection modifiers; hit
// testing and command admission remain owned by the host/controller boundary.
enum class NativePointerButton {
    primary,
    secondary,
    middle,
};

struct NativePointerEvent {
    NativePointerButton button = NativePointerButton::primary;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t modifiers = 0U;
    bool pressed = true;
};

// Triangle identity and source positions captured from one viewport snapshot.
// The desktop's current orthographic view looks down +Y; `camera_depth` stores
// world Y so overlapping screen-space hits resolve to the frontmost surface.
struct SculptViewportTriangle {
    scene::ObjectId object;
    geometry::FaceId face;
    std::array<core::Vec2d, 3> screen_positions{};
    std::array<geometry::VertexId, 3> vertices{};
    std::array<core::Vec3d, 3> local_positions{};
    std::array<double, 3> camera_depth{};
};

[[nodiscard]] core::Result<production::SculptStrokeSample> sample_sculpt_viewport_surface(
    std::span<const SculptViewportTriangle> triangles,
    scene::ObjectId object,
    core::Vec2d screen_position);

[[nodiscard]] core::Result<std::vector<production::SculptStrokeSample>>
sample_sculpt_viewport_stroke(
    std::span<const SculptViewportTriangle> triangles,
    scene::ObjectId object,
    std::span<const core::Vec2d> screen_path,
    double maximum_spacing_pixels);

[[nodiscard]] std::optional<editor::SelectionOperation>
selection_operation_for_native_pointer(const NativePointerEvent& event) noexcept;

struct OperationView {
    std::uint64_t operation_id = 0U;
    OperationCategory category = OperationCategory::committed_command;
    std::string action;
    core::Revision revision_before;
    core::Revision revision_after;
    bool document_changed = false;
};

struct CommandView {
    std::string id;
    std::string label;
    std::string shortcut;
    bool supported = true;
    std::string unavailable_reason;
};

enum class MeshImportFormat {
    obj,
    ply,
    stl,
};

// A front-end import result carries both interchange evidence and the
// authoritative application receipt. The parsed mesh is never exposed as a
// second mutable UI copy: admission finishes through CreateMeshObjectAction.
struct MeshImportReceipt {
    MeshImportFormat format = MeshImportFormat::obj;
    std::string object_name;
    std::size_t vertices = 0U;
    std::size_t faces = 0U;
    std::size_t triangles = 0U;
    std::vector<std::string> warnings;
    application::DispatchReceipt dispatch;
};

enum class MarketStatus {
    unconfigured,
    empty,
    ready,
    error,
};

// A read-only projection of the local, data-only market library. This is
// deliberately not a catalog, entitlement, payment, or runtime-authority
// model. The UI can show what the local library validated and installed; it
// cannot claim remote availability or executable capability.
struct MarketPackageView {
    std::string package_id;
    std::string listing_id;
    std::string listing_version_id;
    std::string publisher_id;
    std::string license_id;
    std::optional<std::string> source_revision;
    std::string content_digest;
    std::string package_digest;
    std::uint64_t payload_bytes = 0U;
    std::uint32_t payload_files = 0U;
    bool publication_receipt_bound = false;
};

struct MarketView {
    MarketStatus status = MarketStatus::unconfigured;
    std::string status_text;
    std::filesystem::path library_root;
    std::vector<MarketPackageView> packages;
};

struct UiPreferences {
    OperatorMode operator_mode = OperatorMode::person_first;
    Workspace workspace = Workspace::model;
    Density density = Density::standard;
    Theme theme = Theme::dark;
    BottomPanel bottom_panel = BottomPanel::operations;
    bool ai_auto_approve = true;
    std::vector<Panel> visible_panels{
        Panel::scene,
        Panel::viewport,
        Panel::inspector,
        Panel::operations,
        Panel::problems,
    };

    [[nodiscard]] core::Result<void> validate() const;
};

// Cartographer's physical workbench is local presentation state. It is kept
// separate from document/project state and is persisted in its own bounded
// preference file so instrument geometry cannot become project truth.
enum class WorkbenchInstrument {
    scene,
    assets,
    layers,
    tools,
    references,
    draft,
    inspector,
    material,
    constraint,
    modify,
    measure,
    transform,
    extrude,
};

inline constexpr std::size_t kWorkbenchInstrumentCount = 13U;
inline constexpr std::size_t kWorkbenchFlowMemoryCount = 4U;
inline constexpr std::uint32_t kMaxWorkbenchFlowUses = 10000U;

struct WorkbenchInstrumentLayout {
    bool visible = false;
    bool positioned = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct WorkbenchPreferences {
    bool ledger_collapsed = false;
    // Bounded local workflow memory. The desktop maps the four preset flow
    // slots to Focus, Author, Inspect, and Draft without making this state
    // part of project truth.
    std::array<std::uint32_t, kWorkbenchFlowMemoryCount> flow_use_counts{};
    std::int32_t last_flow_index = -1;
    std::array<WorkbenchInstrumentLayout, kWorkbenchInstrumentCount> instruments{};

    [[nodiscard]] core::Result<void> validate() const;
};

enum class SurfaceFidelityStatus {
    idle,
    ready,
    stale,
    unavailable,
    rejected,
};

// The native workbench invokes the reference compiler synchronously. Keep
// that presentation-only path explicitly bounded until a worker-owned
// compiler service exists; large production meshes belong in an offline or
// renderer-owned pipeline, not in the UI event loop.
inline constexpr std::uint32_t kMaxSurfacePreviewTriangles = 4096U;

// A bounded presentation projection of a surface compiler result. The
// compiled polygon itself remains a derived preview owned by the controller;
// this view carries only the evidence the native shell needs to display and
// never becomes project or renderer truth.
struct SurfaceFidelityView {
    SurfaceFidelityStatus status = SurfaceFidelityStatus::idle;
    std::string status_text;
    std::optional<scene::ObjectId> object;
    std::uint64_t project_generation = 0U;
    core::Revision project_revision;
    core::Revision source_revision;
    std::uint32_t source_triangles = 0U;
    std::uint32_t target_triangles = 0U;
    surface::RepresentationMode mode = surface::RepresentationMode::fidelity_closure;
    surface::SurfaceErrorEnvelope error;
    bool admitted = false;
    std::shared_ptr<const surface::SurfaceClosurePolygon> compiled;
    std::string domain_digest;
    std::string representation_digest;
    std::optional<core::Diagnostic> diagnostic;

    [[nodiscard]] core::Result<void> validate() const;
};

// A UI snapshot is a presentation projection. The inherited application
// snapshot remains the only authoring data source; UI-only state is kept in
// the additional fields below and is never serialized into a project.
struct UiSnapshot : application::ApplicationSnapshot {
    OperatorMode operator_mode = OperatorMode::person_first;
    Workspace workspace_kind = Workspace::model;
    Density density = Density::standard;
    Theme theme = Theme::dark;
    BottomPanel bottom_panel = BottomPanel::operations;
    ProjectStatus project_status = ProjectStatus::unsaved;
    std::string project_status_text;
    bool command_palette_open = false;
    std::string command_palette_query;
    std::vector<Panel> visible_panels;
    std::vector<OperationView> operations;
    std::vector<CommandView> commands;
    std::vector<core::Diagnostic> ui_problems;
    UiColorTokens colors;
    SurfaceFidelityView surface_fidelity;
    MarketView market;
    bool ai_available = false;
    bool ai_auto_approve = true;
    std::string ai_status;
    std::string agent_goal;
    std::vector<agent::AgentPlanStep> agent_plan_draft;
    std::vector<agent::ClarificationQuestion> agent_clarifications_draft;
    bool agent_repair_ready = false;
    bool agent_checkpoint_loaded = false;
    bool agent_checkpoint_rebased = false;
    std::string agent_checkpoint_task_id;
    core::Revision agent_checkpoint_base_revision;
    core::Revision agent_checkpoint_rebase_revision;
    std::optional<agent::AgentTaskSnapshot> agent_task;
};

[[nodiscard]] UiColorTokens color_tokens(Theme theme) noexcept;
[[nodiscard]] const char* theme_name(Theme theme) noexcept;
[[nodiscard]] const char* density_name(Density density) noexcept;
[[nodiscard]] const char* operator_mode_name(OperatorMode mode) noexcept;
[[nodiscard]] const char* workspace_name(Workspace workspace) noexcept;
[[nodiscard]] const std::vector<WorkspaceDefinition>& workspace_registry() noexcept;
[[nodiscard]] core::Result<WorkspaceDefinition> workspace_definition(Workspace workspace);
[[nodiscard]] const char* panel_name(Panel panel) noexcept;
[[nodiscard]] const char* bottom_panel_name(BottomPanel panel) noexcept;
[[nodiscard]] const char* mesh_import_format_name(MeshImportFormat format) noexcept;
[[nodiscard]] const char* market_status_name(MarketStatus status) noexcept;
[[nodiscard]] const char* workbench_instrument_name(WorkbenchInstrument instrument) noexcept;
[[nodiscard]] const char* surface_fidelity_status_name(
    SurfaceFidelityStatus status) noexcept;
[[nodiscard]] const char* surface_fidelity_mode_name(
    surface::RepresentationMode mode) noexcept;

[[nodiscard]] core::Result<void> save_workbench_preferences(
    const std::filesystem::path& path,
    const WorkbenchPreferences& preferences);
[[nodiscard]] core::Result<WorkbenchPreferences> load_workbench_preferences(
    const std::filesystem::path& path);

class UiController final {
public:
    explicit UiController(application::ApplicationSession& session);

    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;

    [[nodiscard]] core::Result<application::DispatchReceipt> dispatch(
        const application::ApplicationAction& action);
    [[nodiscard]] core::Result<editor::AuthoringPreview> begin_preview(
        editor::PreviewKind kind) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> commit_preview(
        editor::AuthoringPreview& preview);
    // Native AI proposals carry their copied context so a stale selection or
    // project revision cannot be reused. Safe native operations may be
    // auto-applied by policy; the host still uses the typed preview boundary.
    [[nodiscard]] core::Result<ai::Proposal> propose_ai(std::string_view intent);
    [[nodiscard]] core::Result<editor::AuthoringPreview> begin_ai_preview(
        const ai::Proposal& proposal) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> apply_ai_intent(
        std::string_view intent);
    [[nodiscard]] core::Result<application::DispatchReceipt> commit_ai_preview(
        editor::AuthoringPreview& preview,
        const ai::Proposal& proposal);
    [[nodiscard]] core::Result<application::DispatchReceipt> submit_ai_proposal(
        const application::ApplicationAction& action);
    [[nodiscard]] core::Result<void> queue_agent_step(
        std::string intent,
        std::vector<std::string> dependencies = {});
    [[nodiscard]] core::Result<void> queue_agent_clarification(
        std::string prompt,
        agent::ClarificationImpact impact);
    [[nodiscard]] core::Result<void> answer_agent_clarification(
        std::string goal,
        std::string_view question_id,
        std::string answer);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> run_agent_plan(std::string goal);
    [[nodiscard]] core::Result<void> revise_agent_plan_step(
        agent::AgentPlanStep revised_step);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> repair_agent_plan(
        std::string justification);
    [[nodiscard]] core::Result<void> save_agent_checkpoint(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> load_agent_checkpoint(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> rebase_agent_checkpoint();
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> resume_agent_checkpoint();
    [[nodiscard]] core::Result<application::DispatchReceipt> accept_agent_plan();
    [[nodiscard]] core::Result<void> reject_agent_plan();
    [[nodiscard]] core::Result<void> clear_agent_plan();
    // Resolves the built-in provider, parses bounded interchange input, and
    // admits the resulting mesh through the same human application boundary
    // used by native modelling controls. Failures never mutate the project.
    [[nodiscard]] core::Result<MeshImportReceipt> import_mesh(
        const std::filesystem::path& path,
        MeshImportFormat format);
    [[nodiscard]] core::Result<assets::SourceReimportPlan> preview_source_reimport(
        std::vector<assets::SourceImportRecord> incoming) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> apply_source_reimport_plan(
        assets::SourceReimportPlan plan);
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_fixture();
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_authoring(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> export_engineering_authoring(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> invalidate_engineering_source(
        engineering::Id source_id,
        std::string new_revision,
        assets::Sha256Digest new_digest);
    [[nodiscard]] core::Result<engineering::InspectionPackage>
    build_engineering_package() const;
    [[nodiscard]] core::Result<engineering::VantaAdmissionResult>
    admit_engineering_package() const;
    [[nodiscard]] core::Result<void> publish_engineering_package(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> import_condition_fixture();
    [[nodiscard]] core::Result<application::DispatchReceipt> import_condition_authoring(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> import_condition_audio_clip(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> set_condition_source_reviewed(
        condition::Id source_id, bool reviewed);
    [[nodiscard]] core::Result<void> export_condition_authoring(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> invalidate_condition_source(
        condition::Id source_id,
        std::string new_revision,
        assets::Sha256Digest new_digest);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_audio_source(
        condition::AuthoredAudioSource source);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_acoustic_zone(
        condition::AuthoredAcousticZone zone);
    [[nodiscard]] core::Result<application::DispatchReceipt> update_condition_audio_source_distances(
        condition::Id source_id,
        core::Revision expected_authoring_revision,
        double minimum_distance,
        double maximum_distance);
    [[nodiscard]] core::Result<application::DispatchReceipt> set_condition_audio_source_enabled(
        condition::Id source_id,
        core::Revision expected_authoring_revision,
        bool enabled);
    [[nodiscard]] core::Result<application::DispatchReceipt> update_condition_acoustic_zone_volume(
        condition::Id zone_id,
        core::Revision expected_authoring_revision,
        core::Vec3d half_extents,
        double radius);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_effect_source(
        condition::EffectSource source);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_particle_emitter(
        condition::ParticleEmitterSource emitter);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_field_influence(
        condition::FieldInfluenceBinding influence);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_vegetation_scatter(
        condition::VegetationScatterRecipe recipe);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_state_branch(
        condition::AuthoredStateBranch branch);
    [[nodiscard]] core::Result<condition::EffectPreviewResult> preview_condition_effect(
        condition::Id effect_source_id,
        std::string_view operating_state,
        double elapsed_seconds) const;
    [[nodiscard]] core::Result<condition::VegetationPreviewResult>
    preview_condition_vegetation(condition::Id recipe_id) const;
    [[nodiscard]] core::Result<condition::StateComparisonResult>
    compare_condition_state_branches(condition::Id left_branch_id,
        condition::Id right_branch_id) const;
    [[nodiscard]] core::Result<condition::Package> build_condition_package() const;
    [[nodiscard]] core::Result<condition::AdmissionResult> admit_condition_package() const;
    [[nodiscard]] core::Result<void> publish_condition_package(
        const std::filesystem::path& path) const;
    // Connects the native shell to a local, provider-neutral market library.
    // Refresh and install revalidate package contents and receipts; no network
    // or executable package path is reachable through this controller.
    [[nodiscard]] core::Result<void> configure_market_library(
        const std::filesystem::path& root);
    [[nodiscard]] core::Result<void> refresh_market_library();
    [[nodiscard]] core::Result<market::MarketInstallReceipt> install_market_directory(
        const std::filesystem::path& package_root);
    [[nodiscard]] core::Result<market::MarketInstallReceipt> install_market_archive(
        const std::filesystem::path& archive_path);
    [[nodiscard]] core::Result<void> set_operator_mode(OperatorMode mode);
    [[nodiscard]] core::Result<void> set_workspace(Workspace workspace);
    [[nodiscard]] core::Result<void> set_density(Density density);
    [[nodiscard]] core::Result<void> set_theme(Theme theme);
    [[nodiscard]] core::Result<void> set_bottom_panel(BottomPanel panel);
    [[nodiscard]] core::Result<void> set_ai_auto_approve(bool enabled);
    [[nodiscard]] core::Result<void> set_panel_visible(Panel panel, bool visible);
    [[nodiscard]] core::Result<void> reset_preferences();
    [[nodiscard]] core::Result<void> handle_shortcut(Shortcut shortcut);
    [[nodiscard]] core::Result<void> set_command_palette_query(std::string query);
    // Compiles a revision-bound, read-only surface representation preview from
    // the currently selected viewport instance. It never mutates authoring
    // state or records a project operation.
    [[nodiscard]] core::Result<void> compile_surface_preview(
        std::uint32_t target_triangles,
        surface::RepresentationMode mode = surface::RepresentationMode::fidelity_closure);
    [[nodiscard]] core::Result<void> clear_surface_preview();
    [[nodiscard]] core::Result<void> save_preferences(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> load_preferences(
        const std::filesystem::path& path);

    [[nodiscard]] core::Result<void> can_close(bool discard_dirty = false) const;
    [[nodiscard]] UiPreferences preferences() const;
    [[nodiscard]] UiSnapshot snapshot() const;

private:
    [[nodiscard]] core::Result<application::DispatchReceipt> dispatch_with_category(
        const application::ApplicationAction& action,
        OperationCategory category);
    [[nodiscard]] core::Result<void> apply_preferences(const UiPreferences& preferences);
    [[nodiscard]] core::Result<void> sync_application_workspace(
        const std::vector<Panel>& panels);
    [[nodiscard]] std::vector<CommandView> command_views() const;
    void record_operation(
        const application::DispatchReceipt& receipt,
        OperationCategory category);
    void record_ui_problem(core::Diagnostic diagnostic);
    void record_market_failure(core::Diagnostic diagnostic);

    application::ApplicationSession& session_;
    UiPreferences preferences_;
    ai::NativePlanner native_planner_;
    bool command_palette_open_ = false;
    std::string command_palette_query_;
    std::vector<OperationView> operations_;
    std::vector<core::Diagnostic> ui_problems_;
    std::optional<SurfaceFidelityView> surface_fidelity_;
    std::unique_ptr<market::MarketLibrary> market_library_;
    MarketView market_view_;
    std::uint64_t next_operation_id_ = 1U;
    std::vector<agent::AgentPlanStep> agent_plan_draft_;
    std::vector<agent::ClarificationQuestion> agent_clarifications_draft_;
    std::optional<core::Revision> agent_plan_base_revision_;
    std::optional<agent::AgentTask> agent_task_;
    std::optional<agent::AgentTaskCheckpoint> agent_checkpoint_pending_;
    bool agent_checkpoint_rebased_ = false;
    std::string agent_goal_;
    std::string agent_plan_id_;
    std::string agent_objective_id_;
    bool agent_repair_mode_ = false;
    std::uint64_t next_agent_task_id_ = 1U;
};

} // namespace carto::ui
