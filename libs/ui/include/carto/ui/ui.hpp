#pragma once

#include <carto/application/application.hpp>
#include <carto/core/result.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
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

enum class ProjectStatus {
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
    ai_proposal,
};

enum class Shortcut {
    save,
    undo,
    redo,
    object_mode,
    vertex_mode,
    edge_mode,
    face_mode,
    toggle_command_palette,
    close_overlay,
    person_first,
    ai_first,
};

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

struct UiPreferences {
    OperatorMode operator_mode = OperatorMode::person_first;
    Workspace workspace = Workspace::model;
    Density density = Density::standard;
    Theme theme = Theme::dark;
    BottomPanel bottom_panel = BottomPanel::operations;
    std::vector<Panel> visible_panels{
        Panel::scene,
        Panel::viewport,
        Panel::inspector,
        Panel::operations,
        Panel::problems,
    };

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
    ProjectStatus project_status = ProjectStatus::saved;
    std::string project_status_text;
    bool command_palette_open = false;
    std::string command_palette_query;
    std::vector<Panel> visible_panels;
    std::vector<OperationView> operations;
    std::vector<CommandView> commands;
    std::vector<core::Diagnostic> ui_problems;
    UiColorTokens colors;
    bool ai_available = false;
    std::string ai_status;
};

[[nodiscard]] UiColorTokens color_tokens(Theme theme) noexcept;
[[nodiscard]] const char* theme_name(Theme theme) noexcept;
[[nodiscard]] const char* density_name(Density density) noexcept;
[[nodiscard]] const char* operator_mode_name(OperatorMode mode) noexcept;
[[nodiscard]] const char* workspace_name(Workspace workspace) noexcept;
[[nodiscard]] const char* panel_name(Panel panel) noexcept;
[[nodiscard]] const char* bottom_panel_name(BottomPanel panel) noexcept;

class UiController final {
public:
    explicit UiController(application::ApplicationSession& session);

    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;

    [[nodiscard]] core::Result<application::DispatchReceipt> dispatch(
        const application::ApplicationAction& action,
        OperationCategory category = OperationCategory::committed_command);
    [[nodiscard]] core::Result<void> set_operator_mode(OperatorMode mode);
    [[nodiscard]] core::Result<void> set_workspace(Workspace workspace);
    [[nodiscard]] core::Result<void> set_density(Density density);
    [[nodiscard]] core::Result<void> set_theme(Theme theme);
    [[nodiscard]] core::Result<void> set_bottom_panel(BottomPanel panel);
    [[nodiscard]] core::Result<void> set_panel_visible(Panel panel, bool visible);
    [[nodiscard]] core::Result<void> reset_preferences();
    [[nodiscard]] core::Result<void> handle_shortcut(Shortcut shortcut);
    [[nodiscard]] core::Result<void> set_command_palette_query(std::string query);
    [[nodiscard]] core::Result<void> save_preferences(
        const std::filesystem::path& path) const;
    [[nodiscard]] core::Result<void> load_preferences(
        const std::filesystem::path& path);

    [[nodiscard]] core::Result<void> can_close(bool discard_dirty = false) const;
    [[nodiscard]] UiPreferences preferences() const;
    [[nodiscard]] UiSnapshot snapshot() const;

private:
    [[nodiscard]] core::Result<void> apply_preferences(const UiPreferences& preferences);
    [[nodiscard]] core::Result<void> sync_application_workspace(
        const std::vector<Panel>& panels);
    [[nodiscard]] std::vector<CommandView> command_views() const;
    void record_operation(
        const application::DispatchReceipt& receipt,
        OperationCategory category);
    void record_ui_problem(core::Diagnostic diagnostic);

    application::ApplicationSession& session_;
    UiPreferences preferences_;
    bool command_palette_open_ = false;
    std::string command_palette_query_;
    std::vector<OperationView> operations_;
    std::vector<core::Diagnostic> ui_problems_;
    std::uint64_t next_operation_id_ = 1U;
};

} // namespace carto::ui
