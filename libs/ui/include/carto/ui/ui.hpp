#pragma once

#include <carto/ai/ai.hpp>
#include <carto/agent/scene_query.hpp>
#include <carto/application/application.hpp>
#include <carto/agent/workbench.hpp>
#include <carto/botanical/source.hpp>
#include <carto/condition/authoring.hpp>
#include <carto/condition/vegetation_preview.hpp>
#include <carto/core/math.hpp>
#include <carto/core/result.hpp>
#include <carto/geometry/compiled_mesh.hpp>
#include <carto/editor/observation.hpp>
#include <carto/engineering/authoring.hpp>
#include <carto/engineering/cad_provider.hpp>
#include <carto/io/generated_mesh.hpp>
#include <carto/market/market.hpp>
#include <carto/runtime_api/client.hpp>
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

struct ReferenceImagePlaneViewEdit {
    std::string identity;
    project::ReferencePlaneView view = project::ReferencePlaneView::perspective;
    std::string view_set_identity;
    std::optional<scene::ObjectId> pinned_camera;
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
    uv,
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
    [[nodiscard]] std::string availability_summary() const;
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

// Native pointer coordinates must match the cached UI and viewport hit targets.
// The single-window desktop shell uses client-local ImGui viewport pixels.
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

// Converts a platform client-area event to desktop screen-space coordinates
// for callers whose hit targets are desktop-relative. Do not apply this to
// the single-window desktop shell's ImGui-local hit targets.
[[nodiscard]] NativePointerEvent native_pointer_event_to_screen_space(
    NativePointerEvent client_event,
    std::int32_t screen_offset_x,
    std::int32_t screen_offset_y) noexcept;

struct ProjectedEdgePickCandidate {
    scene::ObjectId object;
    geometry::EdgeId edge;
    core::Vec2d first;
    core::Vec2d second;
};

enum class ProjectedEdgePickStatus {
    hit,
    miss,
    ambiguous,
    invalid_input,
};

struct ProjectedEdgePickResult {
    ProjectedEdgePickStatus status = ProjectedEdgePickStatus::miss;
    scene::ObjectId object;
    geometry::EdgeId edge;
    double distance_pixels = 0.0;
};

// Resolves one projected edge only when the closest hit is separated from
// every other distinct edge by the requested ambiguity margin. Repeated
// triangles that reference one source edge collapse to one candidate.
[[nodiscard]] ProjectedEdgePickResult resolve_projected_edge_pick(
    std::span<const ProjectedEdgePickCandidate> candidates,
    core::Vec2d screen_position,
    double hit_radius_pixels = 12.0,
    double ambiguity_margin_pixels = 1.0);

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

enum class ProjectedSurfaceHitStatus : std::uint8_t {
    miss,
    hit,
    ambiguous,
};

struct ProjectedSurfaceHitResolution {
    ProjectedSurfaceHitStatus status = ProjectedSurfaceHitStatus::miss;
    scene::ObjectId object;
    production::SculptStrokeSample sample;
    double camera_depth = 0.0;
};

inline constexpr std::size_t kMaxDecalOutlineSourceTriangles = 65'536U;
inline constexpr std::size_t kMaxDecalOutlinePolygons = 1'024U;
inline constexpr std::size_t kMaxDecalOutlineVerticesPerPolygon = 16U;

// Resolves a persisted projection only against its exact compiled source mesh.
// This is a bounded presentation outline; it does not render material pixels.
[[nodiscard]] core::Result<std::vector<std::vector<core::Vec3d>>>
resolve_decal_projection_outline(
    const production::DecalPlacement& placement,
    const production::DecalProjection& projection,
    std::uint64_t current_mesh_asset,
    core::Revision current_mesh_revision,
    const geometry::CompiledMesh& source_mesh,
    const geometry::CompiledMesh& visible_mesh);

// Resolves the frontmost surface over all visible scene triangles. Equal-depth
// hits from distinct objects or source faces are ambiguous so a caller cannot
// place through an overlapping object or choose an arbitrary surface normal.
[[nodiscard]] core::Result<ProjectedSurfaceHitResolution>
resolve_frontmost_viewport_surface(
    std::span<const SculptViewportTriangle> triangles,
    core::Vec2d screen_position);

inline constexpr std::size_t kMaxKnifeViewportPathPoints = 4096U;

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

// Projects one viewport stroke onto the exact source face and snaps its two
// endpoints to distinct authored face vertices or strict boundary-edge
// interiors. Returned positions are in mesh-local space for the persistent
// knife patch.
[[nodiscard]] core::Result<std::vector<core::Vec3d>> sample_knife_viewport_stroke(
    std::span<const SculptViewportTriangle> triangles,
    scene::ObjectId object,
    geometry::FaceId face,
    std::span<const core::Vec2d> screen_path,
    double endpoint_snap_radius_pixels = 12.0);

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
    bool source_linked = false;
    std::string source_identity;
    std::string relative_source_path;
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
inline constexpr double kWorkbenchSnapDistancePixels = 128.0;
inline constexpr double kWorkbenchInstrumentMaxSurfaceFraction = 0.68;
inline constexpr double kWorkbenchInstrumentDragGripHeight = 32.0;
inline constexpr double kWorkbenchInstrumentMinimumHeight = 112.0;
inline constexpr double kWorkbenchInstrumentDragControlsStackBreakpoint = 200.0;
inline constexpr std::uint32_t kMaxWorkbenchFlowUses = 10000U;

enum class WorkbenchPane {
    navigation,
    workspace_selector,
    agent_task_strip,
    toolbar,
    tool_rack,
    work_surface,
    operation_ledger,
    instrument_bay,
    ai_context,
};

inline constexpr std::size_t kWorkbenchPaneCount = 9U;

struct WorkbenchInstrumentLayout {
    bool visible = false;
    bool positioned = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct WorkbenchPaneLayout {
    bool visible = false;
    bool positioned = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

// Record measured desktop geometry as a complete, serializable layout. Returns
// false for non-finite measurements and leaves the existing layout untouched.
bool record_workbench_instrument_geometry(
    WorkbenchInstrumentLayout& layout,
    double x, double y, double width, double height) noexcept;

struct WorkbenchSnapWindow {
    core::Vec2d position{};
    core::Vec2d size{};
};

struct WorkbenchInstrumentDragTarget {
    WorkbenchInstrument instrument = WorkbenchInstrument::scene;
    std::int32_t display_order = 0;
    bool visible = false;
    core::Vec2d grip_minimum{};
    core::Vec2d grip_maximum{};
    core::Vec2d titlebar_minimum{};
    core::Vec2d titlebar_maximum{};
    bool titlebar_valid = false;
    // When present, prevents a lower instrument's drag region from being
    // selected through a front window that occludes it.
    core::Vec2d window_minimum{};
    core::Vec2d window_maximum{};
    bool window_valid = false;
};

struct ConditionKinematicPartPose {
    std::uint64_t scene_object_id = 0U;
    core::Transform world_transform = core::Transform::identity();
};

// Ephemeral render-only pose derived from a current authored hinge source.
// This value is never written into project state or the operation journal.
struct ConditionKinematicPosePreview {
    std::uint64_t assembly_id = 0U;
    std::uint64_t joint_id = 0U;
    core::Revision project_revision;
    core::Revision source_revision;
    double angle_radians = 0.0;
    std::vector<ConditionKinematicPartPose> part_poses;
};

struct WorkbenchPreferences {
    bool ledger_collapsed = false;
    // Bounded local workflow memory. The desktop maps the four preset flow
    // slots to Focus, Author, Inspect, and Draft without making this state
    // part of project truth.
    std::array<std::uint32_t, kWorkbenchFlowMemoryCount> flow_use_counts{};
    std::int32_t last_flow_index = -1;
    std::array<WorkbenchInstrumentLayout, kWorkbenchInstrumentCount> instruments{};
    std::array<WorkbenchPaneLayout, kWorkbenchPaneCount> panes = [] {
        std::array<WorkbenchPaneLayout, kWorkbenchPaneCount> layouts{};
        layouts[static_cast<std::size_t>(WorkbenchPane::navigation)].visible = true;
        layouts[static_cast<std::size_t>(WorkbenchPane::workspace_selector)].visible = true;
        layouts[static_cast<std::size_t>(WorkbenchPane::toolbar)].visible = true;
        layouts[static_cast<std::size_t>(WorkbenchPane::work_surface)].visible = true;
        layouts[static_cast<std::size_t>(WorkbenchPane::operation_ledger)].visible = true;
        return layouts;
    }();

    [[nodiscard]] core::Result<void> validate() const;
};

// Returns the closest admissible workbench snap point. Kept in the UI layer
// so placement behavior is deterministic and can be exercised without a
// native windowing backend.
[[nodiscard]] std::optional<core::Vec2d> nearest_workbench_snap_target(
    core::Vec2d position,
    std::span<const core::Vec2d> candidates,
    double snap_distance) noexcept;
[[nodiscard]] std::optional<core::Vec2d> nearest_workbench_snap_target(
    core::Vec2d position,
    std::span<const core::Vec2d> candidates,
    double snap_distance,
    core::Vec2d minimum,
    core::Vec2d maximum) noexcept;
[[nodiscard]] std::optional<core::Vec2d> workbench_instrument_snap_position(
    core::Vec2d position,
    core::Vec2d instrument_size,
    core::Vec2d work_surface_origin,
    core::Vec2d work_surface_extent,
    std::span<const WorkbenchSnapWindow> other_windows,
    double gap,
    double snap_distance = kWorkbenchSnapDistancePixels,
    double margin = 12.0);
[[nodiscard]] core::Vec2d fit_workbench_instrument_size(
    core::Vec2d requested_size,
    core::Vec2d work_surface_extent,
    double maximum_surface_fraction = kWorkbenchInstrumentMaxSurfaceFraction) noexcept;
[[nodiscard]] core::Vec2d clamp_workbench_instrument_position(
    core::Vec2d position,
    core::Vec2d instrument_size,
    core::Vec2d work_surface_origin,
    core::Vec2d work_surface_extent,
    double margin = 12.0) noexcept;
[[nodiscard]] core::Vec2d workbench_placement_window_position(
    core::Vec2d pointer,
    core::Vec2d instrument_size,
    core::Vec2d pointer_grab_offset,
    core::Vec2d work_surface_origin,
    core::Vec2d work_surface_extent,
    double margin = 12.0) noexcept;
[[nodiscard]] double workbench_instrument_drag_grip_width(
    double available_content_width) noexcept;
[[nodiscard]] bool workbench_instrument_drag_controls_stacked(
    double available_content_width) noexcept;
[[nodiscard]] const char* workbench_instrument_drag_grip_label(
    double grip_width) noexcept;
[[nodiscard]] bool workbench_drag_grip_contains(
    core::Vec2d pointer,
    core::Vec2d minimum,
    core::Vec2d maximum) noexcept;
[[nodiscard]] bool workbench_instrument_drag_region_contains(
    core::Vec2d pointer,
    core::Vec2d grip_minimum,
    core::Vec2d grip_maximum,
    core::Vec2d titlebar_minimum,
    core::Vec2d titlebar_maximum,
    bool titlebar_valid) noexcept;
[[nodiscard]] std::optional<WorkbenchInstrument> topmost_workbench_drag_target(
    core::Vec2d pointer,
    std::span<const WorkbenchInstrumentDragTarget> targets) noexcept;
[[nodiscard]] std::optional<WorkbenchInstrument> topmost_workbench_drag_grip_target(
    core::Vec2d pointer,
    std::span<const WorkbenchInstrumentDragTarget> targets) noexcept;
[[nodiscard]] bool workbench_instrument_window_contains(
    core::Vec2d pointer,
    core::Vec2d window_minimum,
    core::Vec2d window_maximum,
    bool visible) noexcept;
[[nodiscard]] bool workbench_instrument_should_settle(
    bool pending_snap,
    bool titlebar_drag_moved,
    bool explicit_grip_drag_moved,
    bool mouse_released) noexcept;
[[nodiscard]] bool native_pointer_requests_pan(
    NativePointerButton button,
    bool pan_tool_active,
    bool space_down,
    bool alt_down) noexcept;
[[nodiscard]] bool work_surface_pointer_requests_pan(
    NativePointerButton button,
    bool pan_tool_active,
    bool space_down,
    bool alt_down,
    bool empty_surface) noexcept;
// Camera navigation has its own eligibility; viewport selection may be
// intentionally disabled while a pan gesture is active.
[[nodiscard]] bool work_surface_camera_pan_allowed(
    bool viewport_valid,
    bool camera_input_allowed,
    bool pointer_inside_viewport) noexcept;
[[nodiscard]] bool work_surface_camera_hotkey_allowed(
    bool viewport_focused,
    bool camera_input_allowed,
    bool text_input_active,
    bool keyboard_captured,
    bool key_pressed) noexcept;
[[nodiscard]] core::Vec2d work_surface_camera_keyboard_pan_delta(
    bool left_pressed,
    bool right_pressed,
    bool up_pressed,
    bool down_pressed,
    core::Vec2d viewport_extent) noexcept;
[[nodiscard]] bool work_surface_pointer_is_unclaimed(
    bool hit_cache_matches_revision,
    bool pointer_hits_scene) noexcept;
[[nodiscard]] core::Vec2d workbench_release_position(
    bool has_native_release,
    core::Vec2d native_release_position,
    core::Vec2d fallback_position) noexcept;
[[nodiscard]] bool workbench_drag_threshold_crossed(
    core::Vec2d press_position,
    core::Vec2d current_position,
    double threshold) noexcept;
[[nodiscard]] core::Vec2d workbench_dragged_position(
    core::Vec2d initial_window_position,
    core::Vec2d initial_pointer_position,
    core::Vec2d current_pointer_position) noexcept;

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

struct AgentTaskStripView {
    std::string task_id;
    agent::TaskStatus status = agent::TaskStatus::prepared;
    std::size_t passed_steps = 0U;
    std::size_t total_steps = 0U;
    std::size_t passed_required_gates = 0U;
    std::size_t required_gates = 0U;
    std::optional<core::Diagnostic> blocker;
};

struct AgentDemonstrationTargetBindingView {
    std::size_t target_set_index = 0U;
    std::uint32_t target_alias = 0U;
    std::uint32_t component_alias = 0U;
    agent::AgentDemonstrationTargetKind target_kind =
        agent::AgentDemonstrationTargetKind::object;
    bool bound = false;
    // Current-project display only. Never serialized into traces or recipes.
    std::string target_label;
};

struct AgentNamedTargetChoiceView {
    std::string id;
    std::string label;
};

struct AgentNamedTargetClarificationView {
    std::string prompt;
    std::string rationale;
    core::Revision source_revision;
    std::uint64_t source_generation = 0U;
    bool stale = false;
    std::vector<AgentNamedTargetChoiceView> candidates;
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
    std::optional<AgentTaskStripView> agent_task_strip;
    MarketView market;
    bool ai_available = false;
    bool ai_auto_approve = true;
    std::string ai_status;
    std::string agent_goal;
    std::string agent_recipe_id;
    std::vector<agent::AgentPlanStep> agent_plan_draft;
    std::vector<agent::HardConstraint> agent_hard_constraints_draft;
    std::vector<agent::ClarificationQuestion> agent_clarifications_draft;
    std::optional<AgentNamedTargetClarificationView> agent_named_target_clarification;
    bool agent_repair_ready = false;
    bool agent_checkpoint_loaded = false;
    bool agent_checkpoint_rebased = false;
    std::string agent_checkpoint_task_id;
    core::Revision agent_checkpoint_base_revision;
    core::Revision agent_checkpoint_rebase_revision;
    std::vector<std::string> agent_checkpoint_directly_conflicting_steps;
    std::vector<std::string> agent_checkpoint_invalidated_steps;
    std::optional<agent::AgentTaskSnapshot> agent_task;
    std::optional<agent::SceneQueryResult> agent_condition_inventory;
    std::optional<agent::AgentDemonstration> agent_demonstration;
    std::optional<agent::AgentDemonstrationTrace> agent_demonstration_trace;
    std::string agent_demonstration_recipe_name;
    std::vector<AgentDemonstrationTargetBindingView> agent_demonstration_bindings;
    std::size_t agent_demonstration_target_set_count = 1U;
    bool agent_demonstration_similar_targets_supported = false;
    bool agent_demonstration_ready_to_preview = false;
};

inline constexpr std::size_t kMaxEngineeringPathProjectionPoints = 2048U;

struct EngineeringPathProjectionPoint {
    std::uint32_t sequence = 0U;
    core::Vec3d position;
    std::optional<std::uint64_t> target_id;
    std::string status;
};

struct EngineeringPathProjectionMarker {
    std::uint64_t id = 0U;
    std::string kind;
    std::string name;
    core::Vec3d position;
    bool reference_frame = false;
};

struct EngineeringPathProjectionKeepOut {
    std::uint64_t id = 0U;
    std::string kind;
    std::string purpose;
    double minimum_x = 0.0;
    double maximum_x = 0.0;
    double minimum_z = 0.0;
    double maximum_z = 0.0;
};

// Bounded presentation-only projection of imported path samples. Coordinates
// remain in the authored workcell frame; no feasibility is inferred here.
struct EngineeringPathProjection {
    std::vector<EngineeringPathProjectionPoint> points;
    std::vector<EngineeringPathProjectionMarker> workcell_markers;
    std::vector<EngineeringPathProjectionKeepOut> keep_outs;
    double minimum_x = 0.0;
    double maximum_x = 0.0;
    double minimum_z = 0.0;
    double maximum_z = 0.0;
    std::size_t source_sample_count = 0U;
    std::size_t omitted_sample_count = 0U;
};

[[nodiscard]] UiColorTokens color_tokens(Theme theme) noexcept;
[[nodiscard]] const char* theme_name(Theme theme) noexcept;
[[nodiscard]] const char* density_name(Density density) noexcept;
[[nodiscard]] const char* operator_mode_name(OperatorMode mode) noexcept;
[[nodiscard]] const char* workspace_name(Workspace workspace) noexcept;
[[nodiscard]] bool workspace_supports_scene_object_translation(Workspace workspace) noexcept;
[[nodiscard]] const std::vector<WorkspaceDefinition>& workspace_registry() noexcept;
[[nodiscard]] core::Result<WorkspaceDefinition> workspace_definition(Workspace workspace);
[[nodiscard]] const char* panel_name(Panel panel) noexcept;
[[nodiscard]] const char* bottom_panel_name(BottomPanel panel) noexcept;
[[nodiscard]] const char* mesh_import_format_name(MeshImportFormat format) noexcept;
[[nodiscard]] const char* market_status_name(MarketStatus status) noexcept;
[[nodiscard]] const char* workbench_instrument_name(WorkbenchInstrument instrument) noexcept;
[[nodiscard]] const char* workbench_pane_name(WorkbenchPane pane) noexcept;
[[nodiscard]] const char* surface_fidelity_status_name(
    SurfaceFidelityStatus status) noexcept;
[[nodiscard]] const char* surface_fidelity_mode_name(
    surface::RepresentationMode mode) noexcept;
[[nodiscard]] core::Result<EngineeringPathProjection> project_engineering_path_xz(
    const application::EngineeringAuthoringView::Path& path,
    const application::EngineeringAuthoringView::Workcell* workcell = nullptr);

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
    [[nodiscard]] core::Result<project::MaterialGraph> material_graph_source(
        std::string_view identity) const;
    [[nodiscard]] core::Result<editor::MaterialGraphAuthoringReview>
    begin_material_graph_review(std::string identity) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> commit_material_graph_review(
        editor::MaterialGraphAuthoringReview& review);
    [[nodiscard]] core::Result<production::DecalProjection> preview_decal_placement(
        const production::DecalPlacement& placement,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<geometry::CompiledMesh> decal_placement_source_mesh(
        const production::DecalPlacement& placement,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<production::DecalPlacement> make_decal_placement(
        scene::ObjectId target_object,
        std::uint64_t material_id,
        core::Vec3d center,
        core::Vec3d normal,
        core::Vec3d tangent,
        double width,
        double height,
        double depth,
        double minimum_normal_dot,
        core::Revision expected_project_revision) const;
    [[nodiscard]] core::Result<editor::AuthoringPreview> begin_preview(
        editor::PreviewKind kind) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> commit_preview(
        editor::AuthoringPreview& preview);
    [[nodiscard]] core::Result<application::MeshCleanupReview> review_mesh_cleanup(
        scene::ObjectId object,
        std::vector<geometry::VertexId> candidates,
        application::MeshCleanupOptions options) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> commit_mesh_cleanup(
        application::MeshCleanupReview& review,
        std::string justification);
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
    [[nodiscard]] core::Result<void> compile_agent_goal_sequence(std::string goal);
    [[nodiscard]] core::Result<void> resolve_agent_named_goal_target(
        std::string_view candidate_id);
    [[nodiscard]] core::Result<void> set_agent_hard_constraints(
        std::vector<agent::HardConstraint> constraints);
    [[nodiscard]] core::Result<application::DispatchReceipt> set_project_authoring_standards(
        project::ProjectAuthoringStandards standards);
    [[nodiscard]] core::Result<void> set_agent_recipe(std::string recipe_id);
    [[nodiscard]] core::Result<void> queue_agent_clarification(
        std::string prompt,
        agent::ClarificationImpact impact,
        std::vector<agent::ClarificationCandidate> candidates = {},
        std::string rationale = {});
    [[nodiscard]] core::Result<void> answer_agent_clarification(
        std::string goal,
        std::string_view question_id,
        std::string answer);
    [[nodiscard]] core::Result<void> choose_agent_clarification_candidate(
        std::string goal,
        std::string_view question_id,
        std::string candidate_id);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> prepare_agent_plan(
        std::string goal,
        std::optional<agent::AgentWorkEnvelope> requested_envelope = std::nullopt);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> run_agent_plan(
        std::string goal,
        std::optional<agent::AgentWorkEnvelope> requested_envelope = std::nullopt);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> run_prepared_agent_plan();
    [[nodiscard]] core::Result<agent::SceneQueryResult> query_agent_condition_inventory();
    [[nodiscard]] core::Result<void> revise_agent_plan_step(
        agent::AgentPlanStep revised_step);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> repair_agent_plan(
        std::string justification);
    [[nodiscard]] core::Result<void> save_agent_checkpoint(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> load_agent_checkpoint(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> save_agent_demonstration(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> load_agent_demonstration(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> clear_agent_demonstration();
    [[nodiscard]] core::Result<void> capture_agent_demonstration_trace(
        application::OperationId first_operation,
        application::OperationId last_operation);
    [[nodiscard]] core::Result<void> bind_agent_demonstration_target(
        std::uint32_t target_alias,
        std::uint32_t component_alias,
        std::size_t target_set_index = 0U);
    [[nodiscard]] core::Result<std::size_t> apply_agent_demonstration_to_similar();
    [[nodiscard]] core::Result<void> set_agent_demonstration_target_set_count(
        std::size_t target_set_count);
    [[nodiscard]] core::Result<void> clear_agent_demonstration_target_bindings();
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot>
    preview_agent_demonstration_trace();
    [[nodiscard]] core::Result<void> save_agent_demonstration_trace(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> load_agent_demonstration_trace(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> save_agent_demonstration_recipe(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> load_agent_demonstration_recipe(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<void> clear_agent_demonstration_trace();
    [[nodiscard]] core::Result<void> rebase_agent_checkpoint();
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> resume_agent_checkpoint();
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> correct_agent_ghost_transform(
        scene::ObjectId object,
        core::Transform transform,
        std::string justification);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot>
    correct_agent_ghost_vertex_position(
        scene::ObjectId object,
        geometry::VertexId vertex,
        core::Vec3d position,
        std::string justification);
    [[nodiscard]] core::Result<agent::AgentTaskSnapshot> correct_agent_ghost_face(
        scene::ObjectId object,
        geometry::FaceId face,
        agent::GhostCorrectionKind kind,
        double distance,
        std::string justification);
    [[nodiscard]] core::Result<application::DispatchReceipt> accept_agent_plan();
    [[nodiscard]] core::Result<void> reject_agent_plan();
    [[nodiscard]] core::Result<void> clear_agent_plan();
    // Resolves the built-in provider, parses bounded interchange input, and
    // admits the resulting mesh through the same human application boundary
    // used by native modelling controls. Failures never mutate the project.
    [[nodiscard]] core::Result<MeshImportReceipt> import_mesh(
        const std::filesystem::path& path,
        MeshImportFormat format);
    // Promotes only a completed, digest-verified private botanical job. The
    // exact source and generated glTF/GLB are stored in the project asset tree,
    // and mesh admission uses the normal human-reviewed import command.
    [[nodiscard]] core::Result<application::DispatchReceipt>
    import_generated_botanical_gltf(
        std::span<const std::uint8_t> gltf_bytes,
        const botanical::Source& source,
        const runtime_api::JobReceipt& job,
        std::string_view expected_organization_scope,
        std::string object_name);
    // Inspects a bounded image source, stores its exact bytes in the saved
    // project's content-addressed asset tree, and commits only digest-bound
    // source metadata. Pixel decoding and viewport shading are not implied.
    [[nodiscard]] core::Result<application::DispatchReceipt> import_material_texture(
        const std::filesystem::path& path);
    // Inspects and stores exact image bytes in the saved project's content-
    // addressed asset tree, then registers a revision-bound review plane.
    // Pixel decoding and viewport rendering are not implied.
    [[nodiscard]] core::Result<application::DispatchReceipt> import_reference_image(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<assets::DecodedRgba8Texture>
    decode_reference_image_tga(std::string_view identity);
    // Applies human-authored placement/display edits to one existing reference
    // plane through the revision-bound presentation-catalog command path.
    [[nodiscard]] core::Result<application::DispatchReceipt>
    update_reference_image_plane(
        std::uint64_t expected_project_generation,
        core::Revision expected_project_revision,
        project::ReferenceImagePlane plane);
    // Applies one or more view changes atomically so front/side/top sets stay
    // complete and camera-pinned planes always reference an authored camera.
    [[nodiscard]] core::Result<application::DispatchReceipt>
    update_reference_image_plane_views(
        std::uint64_t expected_project_generation,
        core::Revision expected_project_revision,
        std::vector<ReferenceImagePlaneViewEdit> edits);
    [[nodiscard]] core::Result<assets::SourceReimportPlan> preview_source_reimport(
        std::vector<assets::SourceImportRecord> incoming) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> apply_source_reimport_plan(
        assets::SourceReimportPlan plan);
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_fixture();
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_authoring(
        const std::filesystem::path& path);
    // Reads only the bounded STEP exchange header. This does not import
    // geometry, register a source, or mutate the project.
    [[nodiscard]] core::Result<engineering::ImportProbe> probe_engineering_step(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_bom_csv(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_requirement_csv(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> import_engineering_path_result(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> attach_engineering_evidence(
        const std::filesystem::path& path,
        engineering::SourceKind kind);
    [[nodiscard]] core::Result<application::DispatchReceipt> create_engineering_characteristic(
        const application::CreateEngineeringCharacteristicAction& action);
    [[nodiscard]] core::Result<application::DispatchReceipt> create_engineering_target(
        const application::CreateEngineeringTargetAction& action);
    [[nodiscard]] core::Result<application::DispatchReceipt> update_engineering_workcell(
        const application::UpdateEngineeringWorkcellAction& action);
    [[nodiscard]] core::Result<application::DispatchReceipt> create_engineering_keep_out(
        const application::CreateEngineeringKeepOutAction& action);
    [[nodiscard]] core::Result<application::DispatchReceipt> create_engineering_workcell_frame(
        const application::CreateEngineeringWorkcellFrameAction& action);
    [[nodiscard]] core::Result<void> export_engineering_authoring(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> invalidate_engineering_source(
        engineering::Id source_id,
        std::string new_revision,
        assets::Sha256Digest new_digest);
    [[nodiscard]] core::Result<engineering::InspectionPackage>
    build_engineering_package() const;
    [[nodiscard]] core::Result<engineering::PackageAdmissionResult>
    admit_engineering_package() const;
    [[nodiscard]] core::Result<void> publish_engineering_package(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<application::DispatchReceipt> import_condition_fixture();
    [[nodiscard]] core::Result<application::DispatchReceipt> import_condition_authoring(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> import_condition_audio_clip(
        const std::filesystem::path& path);
    [[nodiscard]] core::Result<application::DispatchReceipt> bind_condition_asset_to_object(
        condition::Id asset_id,
        scene::ObjectId object_id,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt>
    add_condition_interaction_anchor(
        condition::InteractionAnchor anchor,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_prefab(
        condition::PrefabRecord prefab,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt> instantiate_condition_prefab(
        condition::Id prefab_id,
        std::string name,
        core::Vec3d world_position,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_collection(
        condition::CollectionRecord collection,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt> add_condition_region_kit(
        condition::RegionKitManifest region_kit,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt>
    replace_condition_authored_record(
        application::ConditionAuthoredRecord record,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
    [[nodiscard]] core::Result<application::DispatchReceipt>
    delete_condition_authored_record(
        application::ConditionAuthoredRecordKind kind,
        condition::Id id,
        core::Revision expected_project_revision,
        core::Revision expected_authoring_revision);
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
    [[nodiscard]] core::Result<application::DispatchReceipt>
    resolve_condition_state_branch_merge(
        const condition::StateComparisonResult& comparison,
        const std::vector<condition::StateMergeChoice>& choices,
        std::string name,
        std::string applicability = {});
    [[nodiscard]] core::Result<condition::EffectPreviewResult> preview_condition_effect(
        condition::Id effect_source_id,
        std::string_view operating_state,
        double elapsed_seconds) const;
    [[nodiscard]] core::Result<condition::VegetationPreviewResult>
    preview_condition_vegetation(condition::Id recipe_id) const;
    [[nodiscard]] core::Result<application::DispatchReceipt>
    commit_condition_vegetation_preview(
        const condition::VegetationPreviewResult& preview,
        scene::ObjectId botanical_source_object);
    [[nodiscard]] core::Result<ConditionKinematicPosePreview>
    preview_condition_kinematic_hinge(
        const application::ApplicationSnapshot& snapshot,
        std::uint64_t assembly_id,
        std::uint64_t joint_id,
        double normalized_travel) const;
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
    // Runs a read-only point-sightline query against the exact current viewport
    // snapshot. A stale project/render revision or render error fails closed.
    [[nodiscard]] core::Result<editor::PointSightlineResult> evaluate_point_sightline(
        const editor::PointSightlineQuery& query,
        const editor::PointSightlineOptions& options = {}) const;
    // Evaluates landmark visibility from explicit route stations against the
    // same immutable viewport snapshot and refuses stale revisions.
    [[nodiscard]] core::Result<editor::RouteVisibilityResult>
        evaluate_route_visibility(
            const editor::RouteVisibilityQuery& query,
            const editor::RouteVisibilityOptions& options = {}) const;
    // Samples and evaluates one current authored cubic route. The expected
    // project revision and centerline digest fence source/viewport races.
    [[nodiscard]] core::Result<editor::CurveRouteVisibilityResult>
        evaluate_curve_route_visibility(
            std::string_view curve_identity,
            core::Revision expected_project_revision,
            core::Vec3d target,
            std::uint32_t samples_per_segment,
            std::optional<scene::ObjectId> observer_object = std::nullopt,
            std::optional<scene::ObjectId> target_object = std::nullopt,
            const editor::RouteVisibilityOptions& options = {}) const;
    // Evaluates a supplied row-major target grid against the exact current
    // viewport snapshot; target elevations and geometry are not synthesized.
    [[nodiscard]] core::Result<editor::LandmarkViewshedResult>
        evaluate_landmark_viewshed(
            const editor::LandmarkViewshedQuery& query,
            const editor::LandmarkViewshedOptions& options = {}) const;
    // Projects caller-supplied semantic targets through an explicit camera
    // cone against the exact current viewport snapshot; it never mutates.
    [[nodiscard]] core::Result<editor::AuthoringObservationResult>
        evaluate_authoring_observation(
            const editor::AuthoringObservationQuery& query,
            const editor::AuthoringObservationOptions& options = {}) const;
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
    struct AgentNamedTargetCandidate {
        std::string id;
        std::string label;
        scene::ObjectId object;
    };

    struct PendingAgentNamedTargetClarification {
        std::string goal;
        std::string exact_name;
        std::string prompt;
        std::string rationale;
        core::Revision source_revision;
        std::uint64_t source_generation = 0U;
        std::vector<AgentNamedTargetCandidate> candidates;
    };

    struct AgentNamedTargetResolution {
        PendingAgentNamedTargetClarification pending;
        std::string selected_candidate_id;
        scene::ObjectId selected_object;
    };

    struct AgentDemonstrationTargetBinding {
        std::size_t target_set_index = 0U;
        std::uint32_t target_alias = 0U;
        std::uint32_t component_alias = 0U;
        agent::TargetRef target;
    };

    [[nodiscard]] core::Result<application::DispatchReceipt> dispatch_with_category(
        const application::ApplicationAction& action,
        OperationCategory category);
    [[nodiscard]] core::Result<void> apply_preferences(const UiPreferences& preferences);
    [[nodiscard]] core::Result<void> record_agent_clarification_answer(
        std::string goal,
        std::string_view question_id,
        std::optional<std::string> candidate_id,
        std::optional<std::string> freeform_answer);
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
    std::vector<agent::HardConstraint> agent_hard_constraints_draft_;
    std::vector<agent::ClarificationQuestion> agent_clarifications_draft_;
    std::optional<PendingAgentNamedTargetClarification>
        agent_named_target_clarification_;
    std::optional<AgentNamedTargetResolution> agent_named_target_resolution_;
    std::string agent_recipe_id_;
    std::optional<core::Revision> agent_plan_base_revision_;
    std::optional<agent::AgentTask> agent_task_;
    std::optional<agent::SceneQueryResult> agent_condition_inventory_;
    std::vector<scene::ObjectId> agent_condition_inventory_scope_;
    agent::SceneQueryBudget agent_condition_inventory_query_budget_;
    std::optional<core::Revision> agent_condition_inventory_query_revision_;
    std::optional<agent::AgentDemonstration> agent_demonstration_;
    std::optional<agent::AgentDemonstrationTrace> agent_demonstration_trace_;
    std::string agent_demonstration_recipe_name_;
    std::vector<AgentDemonstrationTargetBinding> agent_demonstration_bindings_;
    std::optional<core::Revision> agent_demonstration_binding_revision_;
    std::size_t agent_demonstration_target_set_count_ = 1U;
    std::optional<agent::AgentTaskCheckpoint> agent_checkpoint_pending_;
    bool agent_checkpoint_rebased_ = false;
    std::vector<std::string> agent_checkpoint_directly_conflicting_steps_;
    std::vector<std::string> agent_checkpoint_invalidated_steps_;
    std::string agent_goal_;
    std::string agent_plan_id_;
    std::string agent_objective_id_;
    bool agent_repair_mode_ = false;
    std::uint64_t next_agent_task_id_ = 1U;
};

} // namespace carto::ui
