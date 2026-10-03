#include <carto/ui/ui.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    include <windows.h>
#endif

namespace carto::ui {

namespace {

constexpr std::size_t kMaxOperations = 256U;
constexpr std::size_t kMaxUiProblems = 64U;
constexpr std::size_t kMaxPreferenceBytes = 16U * 1024U;
constexpr std::size_t kMaxPreferenceLineBytes = 1024U;
constexpr std::size_t kWorkbenchPreferenceV1FieldCount = 2U + kWorkbenchInstrumentCount;
constexpr std::size_t kWorkbenchPreferenceV2FieldCount =
    kWorkbenchPreferenceV1FieldCount + 2U;
std::mutex g_preferences_mutex;
std::atomic<std::uint64_t> g_preference_temp_counter{0U};

core::Diagnostic invalid(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_argument, std::move(message));
}

core::Diagnostic invalid_state(std::string message) {
    return core::Diagnostic(core::ErrorCode::invalid_state, std::move(message));
}

core::Diagnostic stale(std::string message) {
    return core::Diagnostic(core::ErrorCode::stale_data, std::move(message));
}

core::Diagnostic io_error(std::string message) {
    return core::Diagnostic(core::ErrorCode::io_error, std::move(message));
}

core::Diagnostic validation(std::string message) {
    return core::Diagnostic(core::ErrorCode::validation_failed, std::move(message));
}

bool valid(Theme value) noexcept {
    return value == Theme::dark || value == Theme::light || value == Theme::high_contrast;
}

bool valid(Density value) noexcept {
    return value == Density::compact || value == Density::standard || value == Density::touch;
}

bool valid(OperatorMode value) noexcept {
    return value == OperatorMode::person_first || value == OperatorMode::ai_first;
}

bool valid(Workspace value) noexcept {
    return value == Workspace::model || value == Workspace::sculpt ||
        value == Workspace::cad || value == Workspace::build ||
        value == Workspace::material || value == Workspace::animate ||
        value == Workspace::review || value == Workspace::ai;
}

bool valid(Panel value) noexcept {
    return value == Panel::scene || value == Panel::viewport || value == Panel::inspector ||
        value == Panel::operations || value == Panel::graph || value == Panel::assets ||
        value == Panel::timeline || value == Panel::problems || value == Panel::console;
}

bool valid(BottomPanel value) noexcept {
    return value == BottomPanel::operations || value == BottomPanel::graph ||
        value == BottomPanel::assets || value == BottomPanel::timeline ||
        value == BottomPanel::problems || value == BottomPanel::console;
}

std::vector<WorkspaceDefinition> make_workspace_registry() {
    const std::vector<Panel> model_panels{
        Panel::scene, Panel::viewport, Panel::inspector, Panel::operations, Panel::problems};
    const std::vector<Panel> graph_panels{
        Panel::scene, Panel::viewport, Panel::inspector, Panel::graph, Panel::problems};
    const std::vector<Panel> asset_panels{
        Panel::scene, Panel::viewport, Panel::assets, Panel::inspector, Panel::problems};
    const std::vector<Panel> timeline_panels{
        Panel::scene, Panel::viewport, Panel::inspector, Panel::timeline, Panel::problems};
    const auto future = [](Workspace workspace,
                           std::string id,
                           std::string label,
                           std::vector<Panel> panels,
                           BottomPanel bottom,
                           std::string reason) {
        return WorkspaceDefinition{
            workspace, std::move(id), std::move(label), std::move(panels), bottom, false,
            std::move(reason)};
    };
    return {
        {Workspace::model, "model", "Model", model_panels, BottomPanel::operations, true, {}},
        future(Workspace::sculpt, "sculpt", "Sculpt", model_panels, BottomPanel::operations,
               "sculpt authoring tools are not available in this build"),
        future(Workspace::cad, "cad", "CAD", graph_panels, BottomPanel::graph,
               "CAD sketch and constraint tools are not available in this build"),
        future(Workspace::build, "build", "Build", asset_panels, BottomPanel::assets,
               "build and assembly tools are not available in this build"),
        future(Workspace::material, "material", "Material", asset_panels, BottomPanel::assets,
               "material authoring tools are not available in this build"),
        future(Workspace::animate, "animate", "Animate", timeline_panels, BottomPanel::timeline,
               "animation tools are not available in this build"),
        future(Workspace::review, "review", "Review", model_panels, BottomPanel::problems,
               "review and comparison tools are not available in this build"),
        {Workspace::ai, "ai", "AI", graph_panels, BottomPanel::graph, true, {}},
    };
}

std::vector<application::Pane> application_panes(const std::vector<Panel>& panels) {
    std::vector<application::Pane> result;
    const auto add = [&result](application::Pane pane) {
        if (std::find(result.begin(), result.end(), pane) == result.end()) result.push_back(pane);
    };
    for (const Panel panel : panels) {
        switch (panel) {
        case Panel::scene: add(application::Pane::outliner); break;
        case Panel::viewport: add(application::Pane::viewport); break;
        case Panel::inspector: add(application::Pane::inspector); break;
        case Panel::problems: add(application::Pane::problems); break;
        case Panel::operations: add(application::Pane::history); break;
        case Panel::graph:
        case Panel::assets:
        case Panel::timeline:
        case Panel::console:
            break;
        }
    }
    return result;
}

bool is_operation_action(const application::ApplicationAction& action) {
    return std::visit([](const auto& value) {
        using Action = std::decay_t<decltype(value)>;
        return std::is_same_v<Action, application::NewProjectAction> ||
            std::is_same_v<Action, application::OpenProjectAction> ||
            std::is_same_v<Action, application::SaveProjectAction> ||
            std::is_same_v<Action, application::UndoAction> ||
            std::is_same_v<Action, application::RedoAction> ||
            std::is_same_v<Action, application::InvokeToolAction> ||
            std::is_same_v<Action, application::RepeatLastToolAction> ||
            std::is_same_v<Action, application::AdjustLastToolAction> ||
            std::is_same_v<Action, application::CreateBoxAction> ||
            std::is_same_v<Action, application::CreatePlaneAction> ||
            std::is_same_v<Action, application::CreateMeshObjectAction> ||
            std::is_same_v<Action, application::SetObjectTransformAction>;
    }, action);
}

ai::Context make_ai_context(
    const application::ApplicationSnapshot& snapshot,
    const editor::AuthoringContext& authoring) {
    return ai::Context{
        snapshot.project_revision,
        authoring,
        ai::build_operation_ontology(snapshot.tools)};
}

bool same_ai_preview(
    const ai::Proposal& proposal,
    const editor::AuthoringPreview& preview) {
    if (proposal.base_revision != preview.base_revision() ||
        proposal.context != preview.context() ||
        proposal.preview_kind != preview.kind()) {
        return false;
    }
    const auto& expected = proposal.parameters;
    const auto& actual = preview.parameters();
    if (expected.distance != actual.distance) return false;
    if (expected.position.has_value() != actual.position.has_value()) return false;
    if (expected.position.has_value()) {
        const auto& left = *expected.position;
        const auto& right = *actual.position;
        if (left.x != right.x || left.y != right.y || left.z != right.z) return false;
    }
    if (expected.transform.has_value() != actual.transform.has_value()) return false;
    return !expected.transform.has_value();
}

std::string serialize_panels(const std::vector<Panel>& panels) {
    std::string result;
    for (std::size_t index = 0U; index < panels.size(); ++index) {
        if (index != 0U) result.push_back(',');
        result += panel_name(panels[index]);
    }
    return result;
}

core::Result<Panel> parse_panel(std::string_view value) {
    for (const Panel panel : {
             Panel::scene, Panel::viewport, Panel::inspector, Panel::operations,
             Panel::graph, Panel::assets, Panel::timeline, Panel::problems, Panel::console}) {
        if (value == panel_name(panel)) return core::Result<Panel>::success(panel);
    }
    return core::Result<Panel>::failure(validation("workspace preference panel is invalid"));
}

template <typename Enum>
core::Result<Enum> parse_enum(std::string_view value, const std::vector<std::pair<std::string_view, Enum>>& names) {
    for (const auto& [name, parsed] : names) {
        if (value == name) return core::Result<Enum>::success(parsed);
    }
    return core::Result<Enum>::failure(validation("workspace preference enum is invalid"));
}

core::Result<UiPreferences> parse_preferences(std::string_view text) {
    if (text.size() > kMaxPreferenceBytes) {
        return core::Result<UiPreferences>::failure(validation("workspace preferences are too large"));
    }
    std::map<std::string, std::string> fields;
    std::size_t cursor = 0U;
    while (cursor < text.size()) {
        const std::size_t end = text.find('\n', cursor);
        const std::size_t line_end = end == std::string_view::npos ? text.size() : end;
        std::string_view line = text.substr(cursor, line_end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (line.size() > kMaxPreferenceLineBytes) {
            return core::Result<UiPreferences>::failure(validation("workspace preference line is too long"));
        }
        if (!line.empty()) {
            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0U ||
                !fields.emplace(std::string(line.substr(0U, separator)),
                                std::string(line.substr(separator + 1U))).second) {
                return core::Result<UiPreferences>::failure(validation("workspace preference field is malformed"));
            }
        }
        if (end == std::string_view::npos) break;
        cursor = end + 1U;
    }
    const auto header = fields.find("header");
    const bool legacy_format = header != fields.end() &&
        header->second == "CARTOGRAPHER_UI_PREFS_V1";
    const bool current_format = header != fields.end() &&
        header->second == "CARTOGRAPHER_UI_PREFS_V2";
    if (!legacy_format && !current_format) {
        return core::Result<UiPreferences>::failure(validation("workspace preference header is invalid"));
    }
    if (fields.size() != (current_format ? 8U : 7U)) {
        return core::Result<UiPreferences>::failure(validation("workspace preference field set is invalid"));
    }
    const auto get = [&fields](std::string_view key) -> core::Result<std::string> {
        const auto found = fields.find(std::string(key));
        if (found == fields.end()) {
            return core::Result<std::string>::failure(
                validation("workspace preference field is missing: " + std::string(key)));
        }
        return core::Result<std::string>::success(found->second);
    };
    const auto mode = get("operator_mode");
    const auto workspace = get("workspace");
    const auto density = get("density");
    const auto theme = get("theme");
    const auto bottom = get("bottom_panel");
    const auto panels = get("panels");
    const auto auto_approve = [&]() -> core::Result<bool> {
        if (!current_format) return core::Result<bool>::success(true);
        const auto value = get("ai_auto_approve");
        if (!value) return core::Result<bool>::failure(value.error());
        if (value.value() == "1") return core::Result<bool>::success(true);
        if (value.value() == "0") return core::Result<bool>::success(false);
        return core::Result<bool>::failure(
            validation("workspace preference AI auto-approve flag is invalid"));
    }();
    if (!mode || !workspace || !density || !theme || !bottom || !panels || !auto_approve) {
        const auto& diagnostic = !mode ? mode.error() : !workspace ? workspace.error() :
            !density ? density.error() : !theme ? theme.error() :
            !bottom ? bottom.error() : !panels ? panels.error() : auto_approve.error();
        return core::Result<UiPreferences>::failure(diagnostic);
    }
    const auto parsed_mode = parse_enum<OperatorMode>(mode.value(), {
        {"person-first", OperatorMode::person_first}, {"ai-first", OperatorMode::ai_first}});
    const auto parsed_workspace = parse_enum<Workspace>(workspace.value(), {
        {"model", Workspace::model}, {"sculpt", Workspace::sculpt}, {"cad", Workspace::cad},
        {"build", Workspace::build}, {"material", Workspace::material},
        {"animate", Workspace::animate}, {"review", Workspace::review}, {"ai", Workspace::ai}});
    const auto parsed_density = parse_enum<Density>(density.value(), {
        {"compact", Density::compact}, {"standard", Density::standard}, {"touch", Density::touch}});
    const auto parsed_theme = parse_enum<Theme>(theme.value(), {
        {"dark", Theme::dark}, {"light", Theme::light}, {"high-contrast", Theme::high_contrast}});
    const auto parsed_bottom = parse_enum<BottomPanel>(bottom.value(), {
        {"operations", BottomPanel::operations}, {"graph", BottomPanel::graph},
        {"assets", BottomPanel::assets}, {"timeline", BottomPanel::timeline},
        {"problems", BottomPanel::problems}, {"console", BottomPanel::console}});
    if (!parsed_mode || !parsed_workspace || !parsed_density || !parsed_theme || !parsed_bottom) {
        const auto& diagnostic = !parsed_mode ? parsed_mode.error() :
            !parsed_workspace ? parsed_workspace.error() : !parsed_density ? parsed_density.error() :
            !parsed_theme ? parsed_theme.error() : parsed_bottom.error();
        return core::Result<UiPreferences>::failure(diagnostic);
    }

    UiPreferences result;
    result.operator_mode = parsed_mode.value();
    result.workspace = parsed_workspace.value();
    result.density = parsed_density.value();
    result.theme = parsed_theme.value();
    result.bottom_panel = parsed_bottom.value();
    result.ai_auto_approve = auto_approve.value();
    result.visible_panels.clear();
    std::size_t panel_cursor = 0U;
    while (panel_cursor <= panels.value().size()) {
        const std::size_t separator = panels.value().find(',', panel_cursor);
        const std::string_view name = std::string_view(panels.value()).substr(
            panel_cursor, separator == std::string::npos ? std::string::npos : separator - panel_cursor);
        if (name.empty()) {
            return core::Result<UiPreferences>::failure(validation("workspace preference panel list is invalid"));
        }
        const auto parsed_panel = parse_panel(name);
        if (!parsed_panel) return core::Result<UiPreferences>::failure(parsed_panel.error());
        result.visible_panels.push_back(parsed_panel.value());
        if (separator == std::string::npos) break;
        panel_cursor = separator + 1U;
    }
    if (auto valid_result = result.validate(); !valid_result) {
        return core::Result<UiPreferences>::failure(valid_result.error());
    }
    return core::Result<UiPreferences>::success(std::move(result));
}

std::string serialize_preferences(const UiPreferences& preferences) {
    return std::string("header=CARTOGRAPHER_UI_PREFS_V2\n") +
        "operator_mode=" + operator_mode_name(preferences.operator_mode) + "\n" +
        "workspace=" + workspace_name(preferences.workspace) + "\n" +
        "density=" + density_name(preferences.density) + "\n" +
        "theme=" + theme_name(preferences.theme) + "\n" +
        "bottom_panel=" + bottom_panel_name(preferences.bottom_panel) + "\n" +
        "ai_auto_approve=" + std::string(preferences.ai_auto_approve ? "1\n" : "0\n") +
        "panels=" + serialize_panels(preferences.visible_panels) + "\n";
}

core::Result<void> atomic_replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.wstring().c_str(), destination.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return core::Result<void>::failure(io_error("unable to publish workspace preferences atomically"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return core::Result<void>::failure(io_error(
            "unable to publish workspace preferences atomically: " + error.message()));
    }
    return core::Result<void>::success();
#endif
}

std::filesystem::path preference_temporary_path(const std::filesystem::path& path) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto counter = g_preference_temp_counter.fetch_add(1U, std::memory_order_relaxed);
    return std::filesystem::path(
        path.string() + ".tmp-" + std::to_string(static_cast<unsigned long long>(ticks)) +
        "-" + std::to_string(static_cast<unsigned long long>(counter)));
}

constexpr std::array<WorkbenchInstrument, kWorkbenchInstrumentCount>
    all_workbench_instruments() noexcept {
    return {
        WorkbenchInstrument::scene,
        WorkbenchInstrument::assets,
        WorkbenchInstrument::layers,
        WorkbenchInstrument::tools,
        WorkbenchInstrument::references,
        WorkbenchInstrument::draft,
        WorkbenchInstrument::inspector,
        WorkbenchInstrument::material,
        WorkbenchInstrument::constraint,
        WorkbenchInstrument::modify,
        WorkbenchInstrument::measure,
        WorkbenchInstrument::transform,
        WorkbenchInstrument::extrude,
    };
}

bool parse_integer(std::string_view text, std::int32_t& value) noexcept {
    if (text.empty()) return false;
    std::int64_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed < std::numeric_limits<std::int32_t>::min() ||
        parsed > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    value = static_cast<std::int32_t>(parsed);
    return true;
}

bool parse_flow_count(std::string_view text, std::uint32_t& value) noexcept {
    if (text.empty()) return false;
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed > kMaxWorkbenchFlowUses) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_flow_counts(
    std::string_view text,
    std::array<std::uint32_t, kWorkbenchFlowMemoryCount>& values) noexcept {
    std::size_t cursor = 0U;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        const std::size_t separator = text.find(',', cursor);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        if (!parse_flow_count(text.substr(cursor, end - cursor), values[index])) return false;
        if (separator == std::string_view::npos) {
            return index + 1U == values.size();
        }
        cursor = separator + 1U;
    }
    return false;
}

bool parse_flag(std::string_view text, bool& value) noexcept {
    if (text == "0") {
        value = false;
        return true;
    }
    if (text == "1") {
        value = true;
        return true;
    }
    return false;
}

core::Result<WorkbenchInstrumentLayout> parse_workbench_layout(std::string_view text) {
    std::array<std::string_view, 6U> fields{};
    std::size_t field_index = 0U;
    std::size_t cursor = 0U;
    while (field_index < fields.size()) {
        const std::size_t separator = text.find(',', cursor);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        fields[field_index] = text.substr(cursor, end - cursor);
        if (fields[field_index].empty()) {
            return core::Result<WorkbenchInstrumentLayout>::failure(
                validation("workbench instrument layout contains an empty field"));
        }
        ++field_index;
        if (separator == std::string_view::npos) break;
        cursor = separator + 1U;
    }
    if (field_index != fields.size() || text.find(',', cursor) != std::string_view::npos) {
        return core::Result<WorkbenchInstrumentLayout>::failure(
            validation("workbench instrument layout must contain six fields"));
    }

    WorkbenchInstrumentLayout result;
    if (!parse_flag(fields[0], result.visible) || !parse_flag(fields[1], result.positioned) ||
        !parse_integer(fields[2], result.x) || !parse_integer(fields[3], result.y) ||
        !parse_integer(fields[4], result.width) || !parse_integer(fields[5], result.height)) {
        return core::Result<WorkbenchInstrumentLayout>::failure(
            validation("workbench instrument layout contains an invalid value"));
    }
    return core::Result<WorkbenchInstrumentLayout>::success(result);
}

core::Result<WorkbenchPreferences> parse_workbench_preferences(std::string_view text) {
    if (text.size() > kMaxPreferenceBytes) {
        return core::Result<WorkbenchPreferences>::failure(
            validation("workbench preferences are too large"));
    }
    std::map<std::string, std::string> fields;
    std::size_t cursor = 0U;
    while (cursor < text.size()) {
        const std::size_t end = text.find('\n', cursor);
        const std::size_t line_end = end == std::string_view::npos ? text.size() : end;
        std::string_view line = text.substr(cursor, line_end - cursor);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
        if (line.size() > kMaxPreferenceLineBytes) {
            return core::Result<WorkbenchPreferences>::failure(
                validation("workbench preference line is too long"));
        }
        if (!line.empty()) {
            const std::size_t separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0U ||
                !fields.emplace(std::string(line.substr(0U, separator)),
                                std::string(line.substr(separator + 1U))).second) {
                return core::Result<WorkbenchPreferences>::failure(
                    validation("workbench preference field is malformed"));
            }
        }
        if (end == std::string_view::npos) break;
        cursor = end + 1U;
    }

    const auto header = fields.find("header");
    const bool legacy_format = header != fields.end() &&
        header->second == "CARTOGRAPHER_WORKBENCH_PREFS_V1";
    const bool current_format = header != fields.end() &&
        header->second == "CARTOGRAPHER_WORKBENCH_PREFS_V2";
    if (!legacy_format && !current_format) {
        return core::Result<WorkbenchPreferences>::failure(
            validation("workbench preference header is invalid"));
    }
    const std::size_t expected_field_count = current_format
        ? kWorkbenchPreferenceV2FieldCount : kWorkbenchPreferenceV1FieldCount;
    if (fields.size() != expected_field_count) {
        return core::Result<WorkbenchPreferences>::failure(
            validation("workbench preference field set is invalid"));
    }

    const auto ledger = fields.find("ledger_collapsed");
    if (ledger == fields.end()) {
        return core::Result<WorkbenchPreferences>::failure(
            validation("workbench preference field is missing: ledger_collapsed"));
    }
    WorkbenchPreferences result;
    if (!parse_flag(ledger->second, result.ledger_collapsed)) {
        return core::Result<WorkbenchPreferences>::failure(
            validation("workbench preference ledger flag is invalid"));
    }
    if (current_format) {
        const auto last_flow = fields.find("workflow_last_flow");
        const auto flow_counts = fields.find("workflow_flow_counts");
        if (last_flow == fields.end() || flow_counts == fields.end() ||
            !parse_integer(last_flow->second, result.last_flow_index) ||
            !parse_flow_counts(flow_counts->second, result.flow_use_counts)) {
            return core::Result<WorkbenchPreferences>::failure(
                validation("workbench workflow memory is invalid"));
        }
    }
    for (std::size_t index = 0U; index < kWorkbenchInstrumentCount; ++index) {
        const auto instrument = all_workbench_instruments()[index];
        const auto field = fields.find(workbench_instrument_name(instrument));
        if (field == fields.end()) {
            return core::Result<WorkbenchPreferences>::failure(validation(
                "workbench preference field is missing: " +
                std::string(workbench_instrument_name(instrument))));
        }
        const auto parsed = parse_workbench_layout(field->second);
        if (!parsed) return core::Result<WorkbenchPreferences>::failure(parsed.error());
        result.instruments[index] = parsed.value();
    }
    if (auto valid_result = result.validate(); !valid_result) {
        return core::Result<WorkbenchPreferences>::failure(valid_result.error());
    }
    return core::Result<WorkbenchPreferences>::success(std::move(result));
}

std::string serialize_workbench_preferences(const WorkbenchPreferences& preferences) {
    std::string result = "header=CARTOGRAPHER_WORKBENCH_PREFS_V2\n";
    result += std::string("ledger_collapsed=") + (preferences.ledger_collapsed ? "1\n" : "0\n");
    result += "workflow_last_flow=" + std::to_string(preferences.last_flow_index) + "\n";
    result += "workflow_flow_counts=";
    for (std::size_t index = 0U; index < preferences.flow_use_counts.size(); ++index) {
        if (index != 0U) result.push_back(',');
        result += std::to_string(preferences.flow_use_counts[index]);
    }
    result.push_back('\n');
    for (std::size_t index = 0U; index < kWorkbenchInstrumentCount; ++index) {
        const auto& layout = preferences.instruments[index];
        result += workbench_instrument_name(all_workbench_instruments()[index]);
        result += "=";
        result += layout.visible ? "1," : "0,";
        result += layout.positioned ? "1," : "0,";
        result += std::to_string(layout.x) + "," + std::to_string(layout.y) + "," +
            std::to_string(layout.width) + "," + std::to_string(layout.height) + "\n";
    }
    return result;
}

} // namespace

core::Result<void> WorkspaceDefinition::validate() const {
    if (!valid(workspace) || id.empty() || id.size() > 32U || label.empty() ||
        label.size() > 64U || !valid(default_bottom_panel) || default_panels.empty()) {
        return core::Result<void>::failure(invalid("workspace definition identity or defaults are invalid"));
    }
    std::set<Panel> panels;
    for (const Panel panel : default_panels) {
        if (!valid(panel) || !panels.insert(panel).second) {
            return core::Result<void>::failure(invalid(
                "workspace definition contains an invalid or duplicate default panel"));
        }
    }
    if (!panels.contains(Panel::viewport)) {
        return core::Result<void>::failure(invalid_state(
            "workspace definition must keep the viewport visible"));
    }
    if (available && !unavailable_reason.empty()) {
        return core::Result<void>::failure(invalid(
            "available workspace definition cannot have an unavailable reason"));
    }
    if (!available && unavailable_reason.empty()) {
        return core::Result<void>::failure(invalid(
            "unavailable workspace definition requires a reason"));
    }
    return core::Result<void>::success();
}

core::Result<void> UiPreferences::validate() const {
    if (!valid(operator_mode) || !valid(workspace) || !valid(density) ||
        !valid(theme) || !valid(bottom_panel) || visible_panels.empty()) {
        return core::Result<void>::failure(invalid("workspace preferences contain an invalid enum or empty panel set"));
    }
    std::set<Panel> unique;
    for (const Panel panel : visible_panels) {
        if (!valid(panel) || !unique.insert(panel).second) {
            return core::Result<void>::failure(invalid("workspace preferences contain an invalid or duplicate panel"));
        }
    }
    if (!unique.contains(Panel::viewport)) {
        return core::Result<void>::failure(invalid_state("workspace preferences must keep the viewport visible"));
    }
    return core::Result<void>::success();
}

core::Result<void> WorkbenchPreferences::validate() const {
    if (last_flow_index < -1 ||
        last_flow_index >= static_cast<std::int32_t>(kWorkbenchFlowMemoryCount)) {
        return core::Result<void>::failure(
            invalid("workbench workflow memory contains an invalid last flow"));
    }
    for (const std::uint32_t uses : flow_use_counts) {
        if (uses > kMaxWorkbenchFlowUses) {
            return core::Result<void>::failure(
                invalid("workbench workflow memory exceeds its safe bound"));
        }
    }
    for (const auto& layout : instruments) {
        if (!layout.positioned) {
            if (layout.x != 0 || layout.y != 0 || layout.width != 0 || layout.height != 0) {
                return core::Result<void>::failure(
                    invalid("unpositioned workbench instruments cannot have geometry"));
            }
            continue;
        }
        if (layout.width < 120 || layout.width > 4096 || layout.height < 80 ||
            layout.height > 4096 || layout.x < -65536 || layout.x > 65536 ||
            layout.y < -65536 || layout.y > 65536) {
            return core::Result<void>::failure(
                invalid("workbench instrument layout is outside safe bounds"));
        }
    }
    return core::Result<void>::success();
}

UiColorTokens color_tokens(Theme theme) noexcept {
    if (theme == Theme::light) {
        return {
            // HAVEN-derived light surfaces: cool canvas, white cards, and a
            // restrained blue interaction layer keep the workbench readable.
            {0.961F, 0.969F, 0.984F, 1.0F}, {0.969F, 0.976F, 0.992F, 1.0F},
            {1.0F, 1.0F, 1.0F, 1.0F}, {0.941F, 0.953F, 0.973F, 1.0F},
            {0.145F, 0.388F, 0.922F, 0.10F}, {0.145F, 0.388F, 0.922F, 0.20F},
            {0.090F, 0.125F, 0.200F, 1.0F}, {0.361F, 0.400F, 0.471F, 1.0F},
            {0.463F, 0.502F, 0.573F, 1.0F}, {0.847F, 0.875F, 0.918F, 1.0F},
            {0.145F, 0.388F, 0.922F, 1.0F}, {0.145F, 0.388F, 0.922F, 1.0F},
            {0.486F, 0.227F, 0.929F, 1.0F}, {0.133F, 0.773F, 0.369F, 1.0F},
            {0.961F, 0.620F, 0.043F, 1.0F}, {0.937F, 0.267F, 0.267F, 1.0F},
            {0.008F, 0.518F, 0.780F, 1.0F}, {0.486F, 0.227F, 0.929F, 0.20F},
            {0.918F, 0.345F, 0.047F, 0.75F}, {0.918F, 0.345F, 0.047F, 1.0F},
        };
    }
    if (theme == Theme::high_contrast) {
        return {
            {0.0F, 0.0F, 0.0F, 1.0F}, {0.04F, 0.04F, 0.04F, 1.0F},
            {0.06F, 0.06F, 0.06F, 1.0F}, {0.10F, 0.10F, 0.10F, 1.0F},
            {0.16F, 0.16F, 0.16F, 1.0F}, {0.28F, 0.28F, 0.28F, 1.0F},
            {1.0F, 1.0F, 1.0F, 1.0F}, {0.88F, 0.88F, 0.88F, 1.0F},
            {0.68F, 0.68F, 0.68F, 1.0F}, {0.60F, 0.60F, 0.60F, 1.0F},
            {0.95F, 0.95F, 0.20F, 1.0F}, {0.15F, 0.85F, 1.0F, 1.0F},
            {0.50F, 1.0F, 1.0F, 1.0F}, {0.20F, 1.0F, 0.30F, 1.0F},
            {1.0F, 0.80F, 0.10F, 1.0F}, {1.0F, 0.20F, 0.20F, 1.0F},
            {0.30F, 0.80F, 1.0F, 1.0F}, {0.80F, 0.30F, 1.0F, 0.65F},
            {1.0F, 0.70F, 0.10F, 0.90F}, {1.0F, 0.90F, 0.10F, 1.0F},
        };
    }
    return {
        // HAVEN-derived dark surfaces: navy depth, blue focus, and violet for
        // secondary instruments. The high-contrast branch above stays strict.
        {0.043F, 0.071F, 0.125F, 1.0F}, {0.055F, 0.102F, 0.180F, 1.0F},
        {0.067F, 0.102F, 0.173F, 1.0F}, {0.090F, 0.133F, 0.220F, 1.0F},
        {1.0F, 1.0F, 1.0F, 0.133F}, {0.231F, 0.510F, 0.965F, 0.20F},
        {0.953F, 0.965F, 0.988F, 1.0F}, {0.557F, 0.608F, 0.706F, 1.0F},
        {0.400F, 0.450F, 0.530F, 1.0F}, {0.153F, 0.208F, 0.310F, 1.0F},
        {0.231F, 0.510F, 0.965F, 1.0F}, {0.231F, 0.510F, 0.965F, 1.0F},
        {0.545F, 0.361F, 0.965F, 1.0F}, {0.133F, 0.773F, 0.369F, 1.0F},
        {0.961F, 0.620F, 0.043F, 1.0F}, {0.937F, 0.267F, 0.267F, 1.0F},
        {0.008F, 0.518F, 0.780F, 1.0F}, {0.545F, 0.361F, 0.965F, 0.20F},
        {1.0F, 0.541F, 0.239F, 0.75F}, {0.961F, 0.620F, 0.043F, 1.0F},
    };
}

const char* theme_name(Theme theme) noexcept {
    switch (theme) {
    case Theme::dark: return "dark";
    case Theme::light: return "light";
    case Theme::high_contrast: return "high-contrast";
    }
    return "unknown";
}

const char* density_name(Density density) noexcept {
    switch (density) {
    case Density::compact: return "compact";
    case Density::standard: return "standard";
    case Density::touch: return "touch";
    }
    return "unknown";
}

const char* operator_mode_name(OperatorMode mode) noexcept {
    switch (mode) {
    case OperatorMode::person_first: return "person-first";
    case OperatorMode::ai_first: return "ai-first";
    }
    return "unknown";
}

const char* workspace_name(Workspace workspace) noexcept {
    switch (workspace) {
    case Workspace::model: return "model";
    case Workspace::sculpt: return "sculpt";
    case Workspace::cad: return "cad";
    case Workspace::build: return "build";
    case Workspace::material: return "material";
    case Workspace::animate: return "animate";
    case Workspace::review: return "review";
    case Workspace::ai: return "ai";
    }
    return "unknown";
}

const std::vector<WorkspaceDefinition>& workspace_registry() noexcept {
    static const std::vector<WorkspaceDefinition> definitions = make_workspace_registry();
    return definitions;
}

core::Result<WorkspaceDefinition> workspace_definition(Workspace workspace) {
    const auto found = std::find_if(
        workspace_registry().begin(), workspace_registry().end(),
        [workspace](const WorkspaceDefinition& definition) {
            return definition.workspace == workspace;
        });
    if (found == workspace_registry().end()) {
        return core::Result<WorkspaceDefinition>::failure(invalid("workspace is invalid"));
    }
    if (auto result = found->validate(); !result) {
        return core::Result<WorkspaceDefinition>::failure(result.error());
    }
    return core::Result<WorkspaceDefinition>::success(*found);
}

const char* panel_name(Panel panel) noexcept {
    switch (panel) {
    case Panel::scene: return "scene";
    case Panel::viewport: return "viewport";
    case Panel::inspector: return "inspector";
    case Panel::operations: return "operations";
    case Panel::graph: return "graph";
    case Panel::assets: return "assets";
    case Panel::timeline: return "timeline";
    case Panel::problems: return "problems";
    case Panel::console: return "console";
    }
    return "unknown";
}

const char* bottom_panel_name(BottomPanel panel) noexcept {
    switch (panel) {
    case BottomPanel::operations: return "operations";
    case BottomPanel::graph: return "graph";
    case BottomPanel::assets: return "assets";
    case BottomPanel::timeline: return "timeline";
    case BottomPanel::problems: return "problems";
    case BottomPanel::console: return "console";
    }
    return "unknown";
}

const char* workbench_instrument_name(WorkbenchInstrument instrument) noexcept {
    switch (instrument) {
    case WorkbenchInstrument::scene: return "scene";
    case WorkbenchInstrument::assets: return "assets";
    case WorkbenchInstrument::layers: return "layers";
    case WorkbenchInstrument::tools: return "tools";
    case WorkbenchInstrument::references: return "references";
    case WorkbenchInstrument::draft: return "draft";
    case WorkbenchInstrument::inspector: return "inspector";
    case WorkbenchInstrument::material: return "material";
    case WorkbenchInstrument::constraint: return "constraint";
    case WorkbenchInstrument::modify: return "modify";
    case WorkbenchInstrument::measure: return "measure";
    case WorkbenchInstrument::transform: return "transform";
    case WorkbenchInstrument::extrude: return "extrude";
    }
    return "unknown";
}

std::optional<Shortcut> shortcut_for_native_key(const NativeKeyEvent& event) noexcept {
    if (!event.pressed) return std::nullopt;

    const bool control = (event.modifiers & kNativeModifierControl) != 0U;
    const bool shift = (event.modifiers & kNativeModifierShift) != 0U;
    const bool alt = (event.modifiers & kNativeModifierAlt) != 0U;
    const std::uint8_t known_modifiers =
        kNativeModifierControl | kNativeModifierShift | kNativeModifierAlt;
    if ((event.modifiers & static_cast<std::uint8_t>(~known_modifiers)) != 0U) {
        return std::nullopt;
    }

    if (control && !shift && !alt) {
        switch (event.key) {
        case NativeKey::s: return Shortcut::save;
        case NativeKey::z: return Shortcut::undo;
        case NativeKey::y: return Shortcut::redo;
        case NativeKey::r: return Shortcut::repeat_last_tool;
        case NativeKey::k: return Shortcut::toggle_command_palette;
        default: break;
        }
    }

    if (!control && !shift && !alt) {
        switch (event.key) {
        case NativeKey::digit_1: return Shortcut::object_mode;
        case NativeKey::digit_2: return Shortcut::vertex_mode;
        case NativeKey::digit_3: return Shortcut::edge_mode;
        case NativeKey::digit_4: return Shortcut::face_mode;
        case NativeKey::escape: return Shortcut::close_overlay;
        default: break;
        }
    }
    return std::nullopt;
}

std::optional<editor::SelectionOperation> selection_operation_for_native_pointer(
    const NativePointerEvent& event) noexcept {
    if (!event.pressed || event.button != NativePointerButton::primary) {
        return std::nullopt;
    }

    const std::uint8_t known_modifiers =
        kNativeModifierControl | kNativeModifierShift | kNativeModifierAlt;
    if ((event.modifiers & static_cast<std::uint8_t>(~known_modifiers)) != 0U) {
        return std::nullopt;
    }
    const bool control = (event.modifiers & kNativeModifierControl) != 0U;
    const bool shift = (event.modifiers & kNativeModifierShift) != 0U;
    const bool alt = (event.modifiers & kNativeModifierAlt) != 0U;
    if (alt || (control && shift)) return std::nullopt;
    if (control) return editor::SelectionOperation::toggle;
    if (shift) return editor::SelectionOperation::add;
    return editor::SelectionOperation::replace;
}

core::Result<void> save_workbench_preferences(
    const std::filesystem::path& path,
    const WorkbenchPreferences& preferences) {
    std::lock_guard lock(g_preferences_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid("workbench preference path must name a file"));
    }
    if (auto result = preferences.validate(); !result) return result;
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return core::Result<void>::failure(io_error(
            "workbench preference parent is unavailable: " + error.message()));
    }
    const auto temporary = preference_temporary_path(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return core::Result<void>::failure(
            io_error("unable to create workbench preferences"));
        stream << serialize_workbench_preferences(preferences);
        stream.flush();
        if (!stream) {
            std::error_code cleanup_error;
            std::filesystem::remove(temporary, cleanup_error);
            return core::Result<void>::failure(
                io_error("unable to write workbench preferences"));
        }
    }
    const auto replaced = atomic_replace(temporary, path);
    if (!replaced) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
    }
    return replaced;
}

core::Result<WorkbenchPreferences> load_workbench_preferences(
    const std::filesystem::path& path) {
    std::lock_guard lock(g_preferences_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<WorkbenchPreferences>::failure(
            invalid("workbench preference path must name a file"));
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) return core::Result<WorkbenchPreferences>::failure(
            io_error("unable to inspect workbench preferences: " + error.message()));
        return core::Result<WorkbenchPreferences>::failure(
            core::Diagnostic(core::ErrorCode::not_found, "workbench preferences do not exist"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) return core::Result<WorkbenchPreferences>::failure(
        io_error("unable to inspect workbench preference size: " + error.message()));
    if (size > kMaxPreferenceBytes) return core::Result<WorkbenchPreferences>::failure(
        validation("workbench preferences are too large"));
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return core::Result<WorkbenchPreferences>::failure(
        io_error("unable to open workbench preferences"));
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream && !stream.eof()) return core::Result<WorkbenchPreferences>::failure(
        io_error("unable to read workbench preferences"));
    return parse_workbench_preferences(text);
}

UiController::UiController(application::ApplicationSession& session)
    : session_(session) {}

core::Result<application::DispatchReceipt> UiController::dispatch(
    const application::ApplicationAction& action) {
    return dispatch_with_category(action, OperationCategory::committed_command);
}

core::Result<editor::AuthoringPreview> UiController::begin_preview(
    editor::PreviewKind kind) const {
    return session_.begin_preview(kind);
}

core::Result<application::DispatchReceipt> UiController::commit_preview(
    editor::AuthoringPreview& preview) {
    const auto result = application::HumanApplicationAccess::commit_preview(session_, preview);
    if (result) record_operation(result.value(), OperationCategory::committed_command);
    return result;
}

core::Result<ai::Proposal> UiController::propose_ai(std::string_view intent) {
    const auto authoring = session_.authoring_context();
    if (!authoring) {
        record_ui_problem(authoring.error().with_context("native AI context"));
        return core::Result<ai::Proposal>::failure(authoring.error());
    }
    const auto context = make_ai_context(session_.snapshot(), authoring.value());
    if (auto result = context.validate(); !result) {
        record_ui_problem(result.error().with_context("native AI context"));
        return core::Result<ai::Proposal>::failure(result.error());
    }
    auto proposal = native_planner_.propose(intent, context);
    if (!proposal) record_ui_problem(proposal.error().with_context("native AI proposal"));
    return proposal;
}

core::Result<editor::AuthoringPreview> UiController::begin_ai_preview(
    const ai::Proposal& proposal) const {
    const auto authoring = session_.authoring_context();
    if (!authoring) return core::Result<editor::AuthoringPreview>::failure(authoring.error());
    const auto context = make_ai_context(session_.snapshot(), authoring.value());
    if (auto result = proposal.validate(context); !result) {
        return core::Result<editor::AuthoringPreview>::failure(result.error());
    }
    auto preview = session_.begin_preview(proposal.preview_kind);
    if (!preview) return preview;
    auto updated = preview.value().update(proposal.parameters);
    if (!updated) {
        return core::Result<editor::AuthoringPreview>::failure(updated.error());
    }
    return preview;
}

core::Result<application::DispatchReceipt> UiController::apply_ai_intent(
    std::string_view intent) {
    const auto proposal = propose_ai(intent);
    if (!proposal) {
        return core::Result<application::DispatchReceipt>::failure(proposal.error());
    }
    const auto authoring = session_.authoring_context();
    if (!authoring) {
        record_ui_problem(authoring.error().with_context("native AI auto-approval context"));
        return core::Result<application::DispatchReceipt>::failure(authoring.error());
    }
    const auto context = make_ai_context(session_.snapshot(), authoring.value());
    if (auto result = proposal.value().validate_auto_approval(context); !result) {
        record_ui_problem(result.error().with_context("native AI auto-approval"));
        return core::Result<application::DispatchReceipt>::failure(result.error());
    }
    auto preview = begin_ai_preview(proposal.value());
    if (!preview) {
        record_ui_problem(preview.error().with_context("native AI auto-approval preview"));
        return core::Result<application::DispatchReceipt>::failure(preview.error());
    }
    return commit_ai_preview(preview.value(), proposal.value());
}

core::Result<application::DispatchReceipt> UiController::commit_ai_preview(
    editor::AuthoringPreview& preview,
    const ai::Proposal& proposal) {
    const auto authoring = session_.authoring_context();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(authoring.error());
    const auto context = make_ai_context(session_.snapshot(), authoring.value());
    if (auto result = proposal.validate(context); !result) {
        record_ui_problem(result.error().with_context("native AI commit"));
        return core::Result<application::DispatchReceipt>::failure(result.error());
    }
    if (!same_ai_preview(proposal, preview)) {
        const auto diagnostic = stale("native AI preview no longer matches its proposal");
        record_ui_problem(diagnostic);
        return core::Result<application::DispatchReceipt>::failure(diagnostic);
    }
    const auto result = application::HumanApplicationAccess::commit_preview(
        session_, preview, application::OperationSource::ai_proposal, proposal.request_id);
    if (result) record_operation(result.value(), OperationCategory::provider);
    return result;
}

core::Result<application::DispatchReceipt> UiController::submit_ai_proposal(
    const application::ApplicationAction& action) {
    static_cast<void>(action);
    const auto diagnostic = invalid_state(
        "raw AI application actions are rejected; submit a typed proposal and preview it first");
    record_ui_problem(diagnostic);
    return core::Result<application::DispatchReceipt>::failure(diagnostic);
}

core::Result<application::DispatchReceipt> UiController::dispatch_with_category(
    const application::ApplicationAction& action,
    OperationCategory category) {
    const auto result = application::HumanApplicationAccess::dispatch(session_, action);
    if (!result) return result;
    if (is_operation_action(action)) record_operation(result.value(), category);
    return result;
}

core::Result<void> UiController::set_operator_mode(OperatorMode mode) {
    if (!valid(mode)) return core::Result<void>::failure(invalid("operator mode is invalid"));
    preferences_.operator_mode = mode;
    return core::Result<void>::success();
}

core::Result<void> UiController::set_workspace(Workspace workspace) {
    const auto definition = workspace_definition(workspace);
    if (!definition) return core::Result<void>::failure(definition.error());
    if (!definition.value().available) {
        return core::Result<void>::failure(invalid_state(
            definition.value().unavailable_reason));
    }
    UiPreferences next = preferences_;
    next.workspace = workspace;
    next.visible_panels = definition.value().default_panels;
    next.bottom_panel = definition.value().default_bottom_panel;
    return apply_preferences(next);
}

core::Result<void> UiController::set_density(Density density) {
    if (!valid(density)) return core::Result<void>::failure(invalid("density is invalid"));
    preferences_.density = density;
    return core::Result<void>::success();
}

core::Result<void> UiController::set_theme(Theme theme) {
    if (!valid(theme)) return core::Result<void>::failure(invalid("theme is invalid"));
    preferences_.theme = theme;
    return core::Result<void>::success();
}

core::Result<void> UiController::set_bottom_panel(BottomPanel panel) {
    if (!valid(panel)) return core::Result<void>::failure(invalid("bottom panel is invalid"));
    preferences_.bottom_panel = panel;
    return core::Result<void>::success();
}

core::Result<void> UiController::set_ai_auto_approve(bool enabled) {
    preferences_.ai_auto_approve = enabled;
    return core::Result<void>::success();
}

core::Result<void> UiController::set_panel_visible(Panel panel, bool visible) {
    if (!valid(panel)) return core::Result<void>::failure(invalid("panel is invalid"));
    if (panel == Panel::viewport && !visible) {
        return core::Result<void>::failure(
            invalid_state("the viewport is required by the Cartographer shell"));
    }
    UiPreferences next = preferences_;
    const auto found = std::find(next.visible_panels.begin(), next.visible_panels.end(), panel);
    if (visible && found == next.visible_panels.end()) next.visible_panels.push_back(panel);
    if (!visible && found != next.visible_panels.end()) next.visible_panels.erase(found);
    return apply_preferences(next);
}

core::Result<void> UiController::reset_preferences() {
    return apply_preferences(UiPreferences{});
}

core::Result<void> UiController::handle_shortcut(Shortcut shortcut) {
    switch (shortcut) {
    case Shortcut::save: {
        const auto result = dispatch_with_category(
            application::SaveProjectAction{std::nullopt}, OperationCategory::project);
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::undo: {
        const auto result = dispatch_with_category(
            application::UndoAction{}, OperationCategory::undo);
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::redo: {
        const auto result = dispatch_with_category(
            application::RedoAction{}, OperationCategory::redo);
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::repeat_last_tool: {
        const auto result = dispatch_with_category(
            application::RepeatLastToolAction{}, OperationCategory::committed_command);
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::object_mode: {
        const auto result = dispatch(application::SetSelectionModeAction{editor::SelectionMode::object});
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::vertex_mode: {
        const auto result = dispatch(application::SetSelectionModeAction{editor::SelectionMode::vertex});
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::face_mode: {
        const auto result = dispatch(application::SetSelectionModeAction{editor::SelectionMode::face});
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::edge_mode: {
        const auto result = dispatch(application::SetSelectionModeAction{editor::SelectionMode::edge});
        return result ? core::Result<void>::success() : core::Result<void>::failure(result.error());
    }
    case Shortcut::toggle_command_palette:
        command_palette_open_ = !command_palette_open_;
        return core::Result<void>::success();
    case Shortcut::close_overlay:
        command_palette_open_ = false;
        return core::Result<void>::success();
    case Shortcut::person_first:
        return set_operator_mode(OperatorMode::person_first);
    case Shortcut::ai_first:
        return set_operator_mode(OperatorMode::ai_first);
    }
    return core::Result<void>::failure(invalid("shortcut is invalid"));
}

core::Result<void> UiController::set_command_palette_query(std::string query) {
    if (query.size() > 1024U || query.find('\0') != std::string::npos) {
        return core::Result<void>::failure(invalid("command palette query is invalid or too long"));
    }
    command_palette_query_ = std::move(query);
    return core::Result<void>::success();
}

core::Result<void> UiController::can_close(bool discard_dirty) const {
    return session_.can_close(discard_dirty);
}

UiPreferences UiController::preferences() const {
    return preferences_;
}

core::Result<void> UiController::apply_preferences(const UiPreferences& preferences) {
    if (auto result = preferences.validate(); !result) return result;
    if (auto result = sync_application_workspace(preferences.visible_panels); !result) return result;
    preferences_ = preferences;
    return core::Result<void>::success();
}

core::Result<void> UiController::sync_application_workspace(
    const std::vector<Panel>& panels) {
    if (panels.empty()) return core::Result<void>::failure(invalid("panel set is empty"));
    const auto result = dispatch_with_category(
        application::SetWorkspaceAction{application::WorkspaceState{application_panes(panels)}},
        OperationCategory::project);
    if (!result) return core::Result<void>::failure(result.error());
    return core::Result<void>::success();
}

std::vector<CommandView> UiController::command_views() const {
    return {
        {"project.save", "Save Project", "Ctrl+S", true, {}},
        {"edit.undo", "Undo", "Ctrl+Z", true, {}},
        {"edit.redo", "Redo", "Ctrl+Y", true, {}},
        {"edit.repeat-last", "Repeat Last Tool", "Ctrl+R", true, {}},
        {"mode.object", "Object Mode", "1", true, {}},
        {"mode.vertex", "Vertex Mode", "2", true, {}},
        {"mode.edge", "Edge Mode", "3", true, {}},
        {"mode.face", "Face Mode", "4", true, {}},
        {"create.box", "Create Box", {}, true, {}},
        {"create.plane", "Create Plane", {}, true, {}},
        {"operator.person", "Person-First Mode", {}, true, {}},
        {"operator.ai", "AI-First Mode", {}, true, {}},
        {"workspace.reset", "Reset Workspace", {}, true, {}},
    };
}

void UiController::record_operation(
    const application::DispatchReceipt& receipt,
    OperationCategory category) {
    if (operations_.size() >= kMaxOperations) operations_.erase(operations_.begin());
    operations_.push_back(OperationView{
        next_operation_id_++, category, receipt.action, receipt.revision_before,
        receipt.revision_after, receipt.document_changed});
}

void UiController::record_ui_problem(core::Diagnostic diagnostic) {
    if (ui_problems_.size() >= kMaxUiProblems) ui_problems_.erase(ui_problems_.begin());
    ui_problems_.push_back(std::move(diagnostic));
}

core::Result<void> UiController::save_preferences(
    const std::filesystem::path& path) const {
    std::lock_guard lock(g_preferences_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid("workspace preference path must name a file"));
    }
    if (auto result = preferences_.validate(); !result) return result;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(parent, error);
        if (error || !exists || !std::filesystem::is_directory(parent, error) || error) {
            return core::Result<void>::failure(io_error("workspace preference parent is unavailable"));
        }
    }
    const auto temporary = preference_temporary_path(path);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return core::Result<void>::failure(io_error("unable to create workspace preferences"));
    const std::string text = serialize_preferences(preferences_);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error("unable to write workspace preferences"));
    }
    const auto replaced = atomic_replace(temporary, path);
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return replaced;
    }
    return core::Result<void>::success();
}

core::Result<void> UiController::load_preferences(
    const std::filesystem::path& path) {
    std::lock_guard lock(g_preferences_mutex);
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid("workspace preference path must name a file"));
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        if (error) return core::Result<void>::failure(io_error("unable to inspect workspace preferences"));
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "workspace preferences do not exist"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) return core::Result<void>::failure(io_error("unable to inspect workspace preference size"));
    if (size > kMaxPreferenceBytes || size > std::numeric_limits<std::size_t>::max()) {
        const auto diagnostic = validation("workspace preferences are too large");
        const auto reset = reset_preferences();
        if (!reset) {
            preferences_ = UiPreferences{};
            record_ui_problem(reset.error().with_context("default workspace reset failed"));
        }
        record_ui_problem(diagnostic.with_context("workspace preferences reset to defaults"));
        return core::Result<void>::failure(diagnostic);
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return core::Result<void>::failure(io_error("unable to open workspace preferences"));
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream && !stream.eof()) return core::Result<void>::failure(io_error("unable to read workspace preferences"));
    const auto parsed = parse_preferences(text);
    if (!parsed) {
        const auto diagnostic = parsed.error().with_context("workspace preferences reset to defaults");
        const auto reset = reset_preferences();
        if (!reset) {
            preferences_ = UiPreferences{};
            record_ui_problem(reset.error().with_context("default workspace reset failed"));
        }
        record_ui_problem(diagnostic);
        return core::Result<void>::failure(parsed.error());
    }
    const auto applied = apply_preferences(parsed.value());
    if (!applied) {
        const auto reset = reset_preferences();
        if (!reset) {
            preferences_ = UiPreferences{};
            record_ui_problem(reset.error().with_context("default workspace reset failed"));
        }
        record_ui_problem(applied.error().with_context("workspace preferences reset to defaults"));
        return applied;
    }
    return core::Result<void>::success();
}

UiSnapshot UiController::snapshot() const {
    UiSnapshot result;
    static_cast<application::ApplicationSnapshot&>(result) = session_.snapshot();
    result.operator_mode = preferences_.operator_mode;
    result.workspace_kind = preferences_.workspace;
    result.density = preferences_.density;
    result.theme = preferences_.theme;
    result.bottom_panel = preferences_.bottom_panel;
    result.command_palette_open = command_palette_open_;
    result.command_palette_query = command_palette_query_;
    result.visible_panels = preferences_.visible_panels;
    result.operations = operations_;
    result.commands = command_views();
    result.ui_problems = ui_problems_;
    result.colors = color_tokens(preferences_.theme);
    result.ai_available = true;
    result.ai_auto_approve = preferences_.ai_auto_approve;
    result.ai_status = preferences_.ai_auto_approve
        ? "Native planner ready. Safe proposals auto-apply through the revision-bound command path."
        : "Native planner ready. Proposals require explicit Apply through the revision-bound command path.";
    if (!result.ui_problems.empty()) {
        result.project_status = ProjectStatus::ui_error;
        result.project_status_text = "UI attention required";
    } else if (result.viewport.error.has_value()) {
        result.project_status = ProjectStatus::render_error;
        result.project_status_text = "Render unavailable";
    } else if (!result.project_path.has_value()) {
        result.project_status = ProjectStatus::unsaved;
        result.project_status_text = "Not saved";
    } else if (result.dirty) {
        result.project_status = ProjectStatus::modified;
        result.project_status_text = "Modified";
    } else {
        result.project_status = ProjectStatus::saved;
        result.project_status_text = "Saved";
    }
    return result;
}

} // namespace carto::ui
