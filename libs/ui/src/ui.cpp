#include <carto/ui/ui.hpp>

#include <carto/io/builtin_provider.hpp>
#include <carto/io/obj.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/providers/registry.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <tuple>
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

core::Diagnostic unsupported(std::string message) {
    return core::Diagnostic(core::ErrorCode::unsupported, std::move(message));
}

std::optional<scene::ObjectId> selected_surface_object(
    const application::ApplicationSnapshot& snapshot) {
    if (snapshot.selection.component_object.has_value()) {
        return snapshot.selection.component_object;
    }
    if (!snapshot.selection.objects.empty()) return snapshot.selection.objects.front();
    return std::nullopt;
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
        value == Workspace::engineering ||
        value == Workspace::condition ||
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
    const std::vector<Panel> engineering_panels{
        Panel::scene, Panel::viewport, Panel::assets, Panel::inspector,
        Panel::operations, Panel::problems};
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
        {Workspace::sculpt, "sculpt", "Sculpt", model_panels, BottomPanel::operations,
         true, {}},
        {Workspace::engineering, "engineering", "Engineering", engineering_panels,
         BottomPanel::problems, true, {}},
        {Workspace::condition, "condition", "CONDITION", engineering_panels,
         BottomPanel::problems, true, {}},
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
            std::is_same_v<Action, application::AppendSculptStrokeAction> ||
            std::is_same_v<Action, application::AppendSculptSurfaceStrokeAction> ||
            std::is_same_v<Action, application::SetSculptLayerVisibilityAction> ||
            std::is_same_v<Action, application::SetObjectTransformAction> ||
            std::is_same_v<Action, application::ImportEngineeringFixtureAction> ||
            std::is_same_v<Action, application::ImportEngineeringAuthoringAction> ||
            std::is_same_v<Action, application::InvalidateEngineeringSourceAction> ||
            std::is_same_v<Action, application::ImportConditionFixtureAction> ||
            std::is_same_v<Action, application::ImportConditionAuthoringAction> ||
            std::is_same_v<Action, application::InvalidateConditionSourceAction> ||
            std::is_same_v<Action, application::CreateSpatialFieldAction> ||
            std::is_same_v<Action, application::PaintSpatialFieldAction>;
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
        {"model", Workspace::model}, {"sculpt", Workspace::sculpt},
        {"engineering", Workspace::engineering}, {"condition", Workspace::condition},
        {"cad", Workspace::cad},
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
        return core::Result<void>::failure(io_error("unable to publish file atomically"));
    }
    return core::Result<void>::success();
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return core::Result<void>::failure(io_error(
            "unable to publish file atomically: " + error.message()));
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

core::Result<std::string> read_bounded_file(
    const std::filesystem::path& path,
    std::size_t maximum_bytes,
    std::string_view description) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<std::string>::failure(invalid(
            std::string(description) + " path must name a file"));
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return core::Result<std::string>::failure(error
            ? io_error(std::string(description) + " file is unavailable")
            : core::Diagnostic(core::ErrorCode::not_found,
                std::string(description) + " file does not exist"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) return core::Result<std::string>::failure(io_error(
        "unable to inspect " + std::string(description) + " size"));
    if (size > maximum_bytes || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return core::Result<std::string>::failure(validation(
            std::string(description) + " file exceeds the supported size"));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return core::Result<std::string>::failure(io_error(
        "unable to open " + std::string(description) + " file"));
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size()) || stream.bad()) {
        return core::Result<std::string>::failure(io_error(
            "unable to read complete " + std::string(description) + " file"));
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        return core::Result<std::string>::failure(stale(
            std::string(description) + " file changed while it was being read"));
    }
    return core::Result<std::string>::success(std::move(bytes));
}

core::Result<void> write_atomic_file(
    const std::filesystem::path& path,
    std::string_view bytes,
    std::string_view description) {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            std::string(description) + " path must name a file"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        if (!std::filesystem::is_directory(parent, error) || error) {
            return core::Result<void>::failure(io_error(
                std::string(description) + " parent directory is unavailable"));
        }
    }
    const auto temporary = preference_temporary_path(path);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return core::Result<void>::failure(io_error(
        "unable to create " + std::string(description) + " file"));
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error(
            "unable to write " + std::string(description) + " file"));
    }
    const auto replaced = atomic_replace(temporary, path);
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    }
    return replaced;
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

core::Result<void> SurfaceFidelityView::validate() const {
    if (status_text.size() > 4096U ||
        (object.has_value() && !*object) ||
        ((status == SurfaceFidelityStatus::ready ||
          status == SurfaceFidelityStatus::rejected) &&
            (!object.has_value() || source_triangles == 0U || target_triangles == 0U ||
             source_triangles > kMaxSurfacePreviewTriangles ||
             target_triangles > kMaxSurfacePreviewTriangles ||
             target_triangles > source_triangles || compiled == nullptr))) {
        return core::Result<void>::failure(invalid("surface fidelity view is invalid"));
    }
    if (status == SurfaceFidelityStatus::ready ||
        status == SurfaceFidelityStatus::rejected) {
        if (auto result = error.validate(); !result) return result;
    }
    if (admitted && status != SurfaceFidelityStatus::ready) {
        return core::Result<void>::failure(
            invalid("admitted surface fidelity view is not ready"));
    }
    return core::Result<void>::success();
}

UiColorTokens color_tokens(Theme theme) noexcept {
    if (theme == Theme::light) {
        return {
            // Cartographer light: ice canvas, white instruments, slate
            // structure, and the registered blue/bronze identity accents.
            {0.941F, 0.953F, 0.965F, 1.0F}, {0.965F, 0.973F, 0.980F, 1.0F},
            {0.980F, 0.984F, 0.988F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F},
            {0.890F, 0.914F, 0.941F, 1.0F}, {0.184F, 0.510F, 0.965F, 0.16F},
            {0.031F, 0.059F, 0.078F, 1.0F}, {0.322F, 0.376F, 0.435F, 1.0F},
            {0.486F, 0.537F, 0.600F, 1.0F}, {0.796F, 0.831F, 0.867F, 1.0F},
            {0.184F, 0.510F, 0.965F, 1.0F}, {0.184F, 0.510F, 0.965F, 1.0F},
            {0.831F, 0.651F, 0.471F, 1.0F}, {0.133F, 0.773F, 0.369F, 1.0F},
            {0.961F, 0.620F, 0.043F, 1.0F}, {0.937F, 0.267F, 0.267F, 1.0F},
            {0.008F, 0.518F, 0.780F, 1.0F}, {0.486F, 0.227F, 0.929F, 0.20F},
            {0.831F, 0.651F, 0.471F, 0.75F}, {0.831F, 0.651F, 0.471F, 1.0F},
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
        // Cartographer dark: deep earth canvas, slate instruments, ice text,
        // Carto blue interaction, and bronze navigation detail.
        {0.031F, 0.059F, 0.078F, 1.0F}, {0.043F, 0.090F, 0.129F, 1.0F},
        {0.075F, 0.106F, 0.149F, 1.0F}, {0.118F, 0.161F, 0.220F, 1.0F},
        {0.153F, 0.208F, 0.282F, 1.0F}, {0.184F, 0.510F, 0.965F, 0.20F},
        {0.898F, 0.906F, 0.922F, 1.0F}, {0.612F, 0.639F, 0.686F, 1.0F},
        {0.420F, 0.463F, 0.525F, 1.0F}, {0.118F, 0.161F, 0.220F, 1.0F},
        {0.184F, 0.510F, 0.965F, 1.0F}, {0.184F, 0.510F, 0.965F, 1.0F},
        {0.831F, 0.651F, 0.471F, 1.0F}, {0.133F, 0.773F, 0.369F, 1.0F},
        {0.961F, 0.620F, 0.043F, 1.0F}, {0.937F, 0.267F, 0.267F, 1.0F},
        {0.008F, 0.518F, 0.780F, 1.0F}, {0.545F, 0.361F, 0.965F, 0.20F},
        {0.831F, 0.651F, 0.471F, 0.75F}, {0.831F, 0.651F, 0.471F, 1.0F},
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
    case Workspace::engineering: return "engineering";
    case Workspace::condition: return "condition";
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

const char* mesh_import_format_name(MeshImportFormat format) noexcept {
    switch (format) {
    case MeshImportFormat::obj: return "OBJ";
    case MeshImportFormat::ply: return "PLY";
    case MeshImportFormat::stl: return "STL";
    }
    return "unknown";
}

const char* market_status_name(MarketStatus status) noexcept {
    switch (status) {
    case MarketStatus::unconfigured: return "unconfigured";
    case MarketStatus::empty: return "empty";
    case MarketStatus::ready: return "ready";
    case MarketStatus::error: return "error";
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

const char* surface_fidelity_status_name(SurfaceFidelityStatus status) noexcept {
    switch (status) {
    case SurfaceFidelityStatus::idle: return "idle";
    case SurfaceFidelityStatus::ready: return "ready";
    case SurfaceFidelityStatus::stale: return "stale";
    case SurfaceFidelityStatus::unavailable: return "unavailable";
    case SurfaceFidelityStatus::rejected: return "rejected";
    }
    return "unknown";
}

const char* surface_fidelity_mode_name(surface::RepresentationMode mode) noexcept {
    switch (mode) {
    case surface::RepresentationMode::geometry_only: return "geometry-only";
    case surface::RepresentationMode::fidelity_closure: return "fidelity closure";
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

core::Result<production::SculptStrokeSample> sample_sculpt_viewport_surface(
    std::span<const SculptViewportTriangle> triangles,
    scene::ObjectId object,
    core::Vec2d screen_position) {
    if (object.value == 0U || !screen_position.finite()) {
        return core::Result<production::SculptStrokeSample>::failure(invalid(
            "Sculpt viewport sample requires a mesh identity and finite screen point"));
    }

    std::optional<production::SculptStrokeSample> best_sample;
    double best_depth = -std::numeric_limits<double>::infinity();
    std::array<std::uint64_t, 4> best_identity{};
    for (const SculptViewportTriangle& triangle : triangles) {
        if (triangle.object != object) continue;
        if (triangle.face.value == 0U || triangle.vertices[0].value == 0U ||
            triangle.vertices[1].value == 0U || triangle.vertices[2].value == 0U ||
            triangle.vertices[0] == triangle.vertices[1] ||
            triangle.vertices[0] == triangle.vertices[2] ||
            triangle.vertices[1] == triangle.vertices[2]) {
            return core::Result<production::SculptStrokeSample>::failure(validation(
                "Sculpt viewport triangle has invalid stable source identity"));
        }
        for (std::size_t index = 0U; index < triangle.screen_positions.size(); ++index) {
            if (!triangle.screen_positions[index].finite() ||
                !triangle.local_positions[index].finite() ||
                !std::isfinite(triangle.camera_depth[index])) {
                return core::Result<production::SculptStrokeSample>::failure(validation(
                    "Sculpt viewport triangle contains non-finite projected source data"));
            }
        }

        const core::Vec2d& a = triangle.screen_positions[0];
        const core::Vec2d& b = triangle.screen_positions[1];
        const core::Vec2d& c = triangle.screen_positions[2];
        const double denominator = (b.y - c.y) * (a.x - c.x) +
            (c.x - b.x) * (a.y - c.y);
        if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-12) continue;
        const double first = ((b.y - c.y) * (screen_position.x - c.x) +
            (c.x - b.x) * (screen_position.y - c.y)) / denominator;
        const double second = ((c.y - a.y) * (screen_position.x - c.x) +
            (a.x - c.x) * (screen_position.y - c.y)) / denominator;
        const double third = 1.0 - first - second;
        constexpr double barycentric_tolerance = 1.0e-10;
        if (!std::isfinite(first) || !std::isfinite(second) || !std::isfinite(third) ||
            first < -barycentric_tolerance || second < -barycentric_tolerance ||
            third < -barycentric_tolerance) {
            continue;
        }

        std::array<double, 3> weights{
            std::clamp(first, 0.0, 1.0),
            std::clamp(second, 0.0, 1.0),
            std::clamp(third, 0.0, 1.0)};
        const double total = weights[0] + weights[1] + weights[2];
        if (!std::isfinite(total) || total <= 0.0) {
            return core::Result<production::SculptStrokeSample>::failure(validation(
                "Sculpt viewport hit produced invalid barycentric coordinates"));
        }
        for (double& weight : weights) weight /= total;
        const double depth = triangle.camera_depth[0] * weights[0] +
            triangle.camera_depth[1] * weights[1] +
            triangle.camera_depth[2] * weights[2];
        if (!std::isfinite(depth)) {
            return core::Result<production::SculptStrokeSample>::failure(validation(
                "Sculpt viewport hit produced a non-finite camera depth"));
        }
        const std::array<std::uint64_t, 4> identity{
            triangle.face.value,
            triangle.vertices[0].value,
            triangle.vertices[1].value,
            triangle.vertices[2].value};
        const double depth_tolerance = 1.0e-12 * std::max({
            1.0, std::abs(depth), best_sample.has_value() ? std::abs(best_depth) : 0.0});
        const bool nearer = !best_sample.has_value() || depth > best_depth + depth_tolerance;
        const bool stable_tie = best_sample.has_value() &&
            std::abs(depth - best_depth) <= depth_tolerance && identity < best_identity;
        if (!nearer && !stable_tie) continue;

        core::Vec3d position{};
        for (std::size_t index = 0U; index < weights.size(); ++index) {
            position = position + triangle.local_positions[index] * weights[index];
        }
        if (!position.finite() || !std::isfinite(depth)) {
            return core::Result<production::SculptStrokeSample>::failure(validation(
                "Sculpt viewport hit produced a non-finite source sample"));
        }
        production::SculptSurfaceAnchor anchor{
            triangle.face, triangle.vertices, weights};
        if (auto result = anchor.validate(); !result) {
            return core::Result<production::SculptStrokeSample>::failure(result.error());
        }
        best_sample = production::SculptStrokeSample{
            position, 1.0, std::move(anchor), std::nullopt};
        best_depth = depth;
        best_identity = identity;
    }
    if (!best_sample.has_value()) {
        return core::Result<production::SculptStrokeSample>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "Sculpt viewport cursor does not project onto the selected mesh surface"));
    }
    return core::Result<production::SculptStrokeSample>::success(std::move(*best_sample));
}

core::Result<std::vector<production::SculptStrokeSample>> sample_sculpt_viewport_stroke(
    std::span<const SculptViewportTriangle> triangles,
    scene::ObjectId object,
    std::span<const core::Vec2d> screen_path,
    double maximum_spacing_pixels) {
    constexpr std::size_t maximum_raw_points = 8192U;
    if (screen_path.empty() || screen_path.size() > maximum_raw_points ||
        !std::isfinite(maximum_spacing_pixels) || maximum_spacing_pixels <= 0.0 ||
        maximum_spacing_pixels > 512.0) {
        return core::Result<std::vector<production::SculptStrokeSample>>::failure(invalid(
            "Sculpt viewport path or resampling spacing is outside its bound"));
    }
    double total_path_length = 0.0;
    for (std::size_t index = 0U; index < screen_path.size(); ++index) {
        if (!screen_path[index].finite()) {
            return core::Result<std::vector<production::SculptStrokeSample>>::failure(invalid(
                "Sculpt viewport path contains a non-finite cursor point"));
        }
        if (index == 0U) continue;
        const double segment_length = std::hypot(
            screen_path[index].x - screen_path[index - 1U].x,
            screen_path[index].y - screen_path[index - 1U].y);
        if (!std::isfinite(segment_length) ||
            segment_length > std::numeric_limits<double>::max() - total_path_length) {
            return core::Result<std::vector<production::SculptStrokeSample>>::failure(validation(
                "Sculpt viewport path length exceeds its numeric bound"));
        }
        total_path_length += segment_length;
    }
    const double spacing_intervals = std::ceil(total_path_length / maximum_spacing_pixels);
    if (!std::isfinite(spacing_intervals) ||
        spacing_intervals + 1.0 >
            static_cast<double>(production::kMaxSculptSamplesPerStroke)) {
        return core::Result<std::vector<production::SculptStrokeSample>>::failure(invalid(
            "Sculpt viewport stroke exceeds its deterministic sample bound"));
    }

    std::vector<production::SculptStrokeSample> samples;
    samples.reserve(std::min(
        production::kMaxSculptSamplesPerStroke, screen_path.size() + 16U));
    const auto append_sample = [&triangles, object, &samples](core::Vec2d point)
        -> core::Result<void> {
        auto sample = sample_sculpt_viewport_surface(triangles, object, point);
        if (!sample) return core::Result<void>::failure(sample.error());
        if (samples.size() >= production::kMaxSculptSamplesPerStroke) {
            return core::Result<void>::failure(invalid(
                "Sculpt viewport stroke exceeds its deterministic sample bound"));
        }
        samples.push_back(std::move(sample.value()));
        return core::Result<void>::success();
    };

    if (auto result = append_sample(screen_path.front()); !result) {
        return core::Result<std::vector<production::SculptStrokeSample>>::failure(result.error());
    }
    core::Vec2d last_emitted = screen_path.front();
    double distance_until_sample = maximum_spacing_pixels;
    for (std::size_t segment = 1U; segment < screen_path.size(); ++segment) {
        const core::Vec2d start = screen_path[segment - 1U];
        const core::Vec2d end = screen_path[segment];
        if (!start.finite() || !end.finite()) {
            return core::Result<std::vector<production::SculptStrokeSample>>::failure(invalid(
                "Sculpt viewport path contains a non-finite cursor point"));
        }
        const core::Vec2d delta{end.x - start.x, end.y - start.y};
        const double length = std::hypot(delta.x, delta.y);
        if (!std::isfinite(length)) {
            return core::Result<std::vector<production::SculptStrokeSample>>::failure(validation(
                "Sculpt viewport path segment length is non-finite"));
        }
        if (length <= 1.0e-12) continue;

        double travelled = 0.0;
        while (length - travelled + 1.0e-12 >= distance_until_sample) {
            travelled += distance_until_sample;
            const double fraction = std::min(1.0, travelled / length);
            const core::Vec2d point{
                start.x + delta.x * fraction,
                start.y + delta.y * fraction};
            if (auto result = append_sample(point); !result) {
                return core::Result<std::vector<production::SculptStrokeSample>>::failure(
                    result.error());
            }
            last_emitted = point;
            distance_until_sample = maximum_spacing_pixels;
        }
        distance_until_sample -= length - travelled;
        distance_until_sample = std::clamp(
            distance_until_sample, 1.0e-12, maximum_spacing_pixels);
    }

    const core::Vec2d endpoint = screen_path.back();
    if (!endpoint.finite()) {
        return core::Result<std::vector<production::SculptStrokeSample>>::failure(invalid(
            "Sculpt viewport path contains a non-finite cursor point"));
    }
    if (std::hypot(endpoint.x - last_emitted.x, endpoint.y - last_emitted.y) > 1.0e-9) {
        if (auto result = append_sample(endpoint); !result) {
            return core::Result<std::vector<production::SculptStrokeSample>>::failure(
                result.error());
        }
    }
    return core::Result<std::vector<production::SculptStrokeSample>>::success(std::move(samples));
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
    if (!preferences_.ai_auto_approve) {
        const auto diagnostic = invalid_state(
            "native AI auto-approval is disabled; review the preview before committing");
        record_ui_problem(diagnostic);
        return core::Result<application::DispatchReceipt>::failure(diagnostic);
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

core::Result<void> UiController::queue_agent_step(std::string intent) {
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status != agent::TaskStatus::accepted && status != agent::TaskStatus::rejected) {
            return core::Result<void>::failure(invalid_state(
                "accept or reject the current speculative task before editing its plan"));
        }
        agent_task_.reset();
        agent_plan_draft_.clear();
        agent_plan_base_revision_.reset();
        agent_goal_.clear();
    }
    if (agent_plan_draft_.size() >= 16U) {
        return core::Result<void>::failure(invalid_state(
            "agent plan reached the local 16-command work budget"));
    }
    const auto snapshot = session_.snapshot();
    if (agent_plan_base_revision_.has_value() &&
        *agent_plan_base_revision_ != snapshot.project_revision) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed after the first agent plan target was captured"));
    }
    const auto authoring = session_.authoring_context();
    if (!authoring) return core::Result<void>::failure(authoring.error());

    agent::TargetRef target;
    if (authoring.value().selection_mode == editor::SelectionMode::face &&
        authoring.value().component_object.has_value() &&
        authoring.value().faces.size() == 1U) {
        target.object = *authoring.value().component_object;
        target.component = authoring.value().faces.front();
    } else if (authoring.value().selection_mode == editor::SelectionMode::vertex &&
        authoring.value().component_object.has_value() &&
        authoring.value().vertices.size() == 1U) {
        target.object = *authoring.value().component_object;
        target.component = authoring.value().vertices.front();
    } else {
        return core::Result<void>::failure(invalid_state(
            "capture one selected face or vertex for each agent plan step"));
    }

    agent::AgentPlanStep step{
        "step-" + std::to_string(agent_plan_draft_.size() + 1U),
        std::move(target),
        std::move(intent)};
    if (auto result = step.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (!agent_plan_base_revision_.has_value()) {
        agent_plan_base_revision_ = snapshot.project_revision;
    }
    agent_plan_draft_.push_back(std::move(step));
    return core::Result<void>::success();
}

core::Result<agent::AgentTaskSnapshot> UiController::run_agent_plan(std::string goal) {
    if (agent_plan_draft_.empty() || !agent_plan_base_revision_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "capture at least one selected face or vertex before running an agent plan"));
    }
    if (agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "a speculative task is already active"));
    }
    const auto snapshot = session_.snapshot();
    if (snapshot.project_revision != *agent_plan_base_revision_) {
        return core::Result<agent::AgentTaskSnapshot>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "agent plan targets were captured against an older project revision"));
    }
    if (next_agent_task_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_agent_task_id_ = 1U;
    }
    const std::string task_id = "agent-task-" + std::to_string(next_agent_task_id_++);
    std::vector<scene::ObjectId> scope;
    for (const auto& step : agent_plan_draft_) {
        if (std::find(scope.begin(), scope.end(), step.target.object) == scope.end()) {
            scope.push_back(step.target.object);
        }
    }
    agent::AgentPlan plan{
        task_id,
        agent::AuthoringObjective{
            task_id, std::move(goal), *agent_plan_base_revision_, scope, {}},
        agent_plan_draft_};
    const std::string task_goal = plan.objective.goal;
    agent::AgentWorkEnvelope envelope;
    envelope.scope_objects = scope;
    envelope.max_commands = 16U;
    envelope.max_objects_touched = scope.size();
    auto task = agent::AgentTask::begin(session_, std::move(plan), std::move(envelope));
    if (!task) {
        record_ui_problem(task.error().with_context("agent workbench plan"));
        return core::Result<agent::AgentTaskSnapshot>::failure(task.error());
    }
    agent_goal_ = task_goal;
    agent_task_.emplace(std::move(task.value()));
    auto result = agent_task_->run();
    if (!result) {
        record_ui_problem(result.error().with_context("agent workbench execution"));
        return result;
    }
    return result;
}

core::Result<application::DispatchReceipt> UiController::accept_agent_plan() {
    if (!agent_task_.has_value()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "there is no speculative agent result to accept"));
    }
    auto result = application::HumanApplicationAccess::accept_agent_task(*agent_task_);
    if (result) record_operation(result.value(), OperationCategory::provider);
    else record_ui_problem(result.error().with_context("agent result acceptance"));
    return result;
}

core::Result<void> UiController::reject_agent_plan() {
    if (!agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no speculative agent result to reject"));
    }
    auto result = agent_task_->reject();
    if (!result) record_ui_problem(result.error().with_context("agent result rejection"));
    return result;
}

core::Result<void> UiController::clear_agent_plan() {
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status == agent::TaskStatus::review || status == agent::TaskStatus::blocked ||
            status == agent::TaskStatus::prepared) {
            if (auto result = agent_task_->reject(); !result) return result;
        } else if (status == agent::TaskStatus::running) {
            return core::Result<void>::failure(invalid_state(
                "a running agent task cannot be cleared"));
        }
        agent_task_.reset();
    }
    agent_plan_draft_.clear();
    agent_plan_base_revision_.reset();
    agent_goal_.clear();
    return core::Result<void>::success();
}

core::Result<MeshImportReceipt> UiController::import_mesh(
    const std::filesystem::path& path,
    MeshImportFormat format) {
    const auto fail = [this](core::Diagnostic diagnostic) {
        auto surfaced = diagnostic.with_context("native mesh import");
        record_ui_problem(surfaced);
        return core::Result<MeshImportReceipt>::failure(std::move(surfaced));
    };
    if (path.empty() || path.filename().empty()) {
        return fail(invalid("mesh import path must name a file"));
    }

    providers::Registry providers;
    const auto registered = [&]() -> core::Result<void> {
        switch (format) {
        case MeshImportFormat::obj:
            return io::register_builtin_obj_providers(providers);
        case MeshImportFormat::ply:
            return io::register_builtin_ply_providers(providers);
        case MeshImportFormat::stl:
            return io::register_builtin_stl_providers(providers);
        }
        return core::Result<void>::failure(unsupported("mesh import format is unsupported"));
    }();
    if (!registered) return fail(registered.error());

    const std::string capability = [&]() -> std::string {
        switch (format) {
        case MeshImportFormat::obj: return "geometry.import.obj";
        case MeshImportFormat::ply: return "geometry.import.ply";
        case MeshImportFormat::stl: return "geometry.import.stl";
        }
        return {};
    }();
    if (capability.empty()) {
        return fail(unsupported("mesh import format has no provider capability"));
    }
    if (auto resolved = providers.resolve(capability); !resolved) {
        return fail(resolved.error());
    }

    geometry::EditableMesh mesh;
    io::IoReport report;
    switch (format) {
    case MeshImportFormat::obj: {
        auto imported = io::import_obj(path);
        if (!imported) return fail(imported.error());
        mesh = std::move(imported.value().mesh);
        report = std::move(imported.value().report);
        break;
    }
    case MeshImportFormat::ply: {
        auto imported = io::import_ply(path);
        if (!imported) return fail(imported.error());
        mesh = std::move(imported.value().mesh);
        report = std::move(imported.value().report);
        break;
    }
    case MeshImportFormat::stl: {
        auto imported = io::import_stl(path);
        if (!imported) return fail(imported.error());
        mesh = std::move(imported.value().mesh);
        report = std::move(imported.value().report);
        break;
    }
    default:
        return fail(unsupported("mesh import format is unsupported"));
    }

    std::string object_name = path.stem().string();
    if (object_name.empty()) object_name = "Imported Mesh";
    auto admitted = dispatch(application::CreateMeshObjectAction{object_name, std::move(mesh)});
    if (!admitted) {
        return core::Result<MeshImportReceipt>::failure(admitted.error());
    }
    return core::Result<MeshImportReceipt>::success(MeshImportReceipt{
        format,
        std::move(object_name),
        report.vertices,
        report.faces,
        report.triangles,
        std::move(report.warnings),
        std::move(admitted).value(),
    });
}

core::Result<application::DispatchReceipt> UiController::import_engineering_fixture() {
    return dispatch_with_category(
        application::ImportEngineeringFixtureAction{},
        OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::import_engineering_authoring(
    const std::filesystem::path& path) {
    auto bytes = read_bounded_file(
        path, engineering::AuthoringDocument::kMaxSerializedBytes,
        "engineering authoring document");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    return dispatch_with_category(
        application::ImportEngineeringAuthoringAction{std::move(bytes).value()},
        OperationCategory::provider);
}

core::Result<void> UiController::export_engineering_authoring(
    const std::filesystem::path& path) const {
    const auto authoring = session_.engineering_authoring();
    if (!authoring) return core::Result<void>::failure(authoring.error());
    return write_atomic_file(
        path, authoring.value().serialize(), "engineering authoring document");
}

core::Result<application::DispatchReceipt> UiController::invalidate_engineering_source(
    engineering::Id source_id,
    std::string new_revision,
    assets::Sha256Digest new_digest) {
    return dispatch_with_category(
        application::InvalidateEngineeringSourceAction{
            source_id, std::move(new_revision), new_digest},
        OperationCategory::provider);
}

core::Result<engineering::InspectionPackage> UiController::build_engineering_package() const {
    const auto authoring = session_.engineering_authoring();
    if (!authoring) {
        return core::Result<engineering::InspectionPackage>::failure(authoring.error().with_context(
            "engineering publication"));
    }
    const auto snapshot = session_.snapshot();
    return engineering::InspectionPackage::publish(
        authoring.value(), snapshot.project_revision, authoring.value().project_namespace);
}

core::Result<engineering::VantaAdmissionResult> UiController::admit_engineering_package() const {
    const auto package = build_engineering_package();
    if (!package) {
        return core::Result<engineering::VantaAdmissionResult>::failure(package.error());
    }
    return engineering::admit_for_vanta(package.value());
}

core::Result<void> UiController::publish_engineering_package(
    const std::filesystem::path& path) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            "engineering publication path must name a file"));
    }
    const auto package = build_engineering_package();
    if (!package) return core::Result<void>::failure(package.error());
    const auto admission = engineering::admit_for_vanta(package.value());
    if (!admission) return core::Result<void>::failure(admission.error().with_context(
        "engineering publication admission"));
    if (admission.value().status != engineering::VantaAdmissionStatus::admit) {
        return core::Result<void>::failure(validation(
            "engineering publication was refused at the runtime boundary"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(parent, error);
        if (error || !exists || !std::filesystem::is_directory(parent, error) || error) {
            return core::Result<void>::failure(io_error(
                "engineering publication parent is unavailable"));
        }
    }
    const auto temporary = preference_temporary_path(path);
    const std::string bytes = package.value().serialize();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return core::Result<void>::failure(io_error(
        "unable to create engineering publication package"));
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error(
            "unable to write engineering publication package"));
    }
    const auto replaced = atomic_replace(temporary, path);
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return replaced;
    }
    return core::Result<void>::success();
}

core::Result<application::DispatchReceipt> UiController::import_condition_fixture() {
    return dispatch_with_category(
        application::ImportConditionFixtureAction{}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::import_condition_authoring(
    const std::filesystem::path& path) {
    auto bytes = read_bounded_file(
        path, condition::AuthoringDocument::kMaxSerializedBytes,
        "CONDITION authoring document");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    return dispatch_with_category(
        application::ImportConditionAuthoringAction{std::move(bytes).value()},
        OperationCategory::provider);
}

core::Result<void> UiController::export_condition_authoring(
    const std::filesystem::path& path) const {
    const auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<void>::failure(authoring.error());
    return write_atomic_file(
        path, authoring.value().serialize(), "CONDITION authoring document");
}

core::Result<application::DispatchReceipt> UiController::invalidate_condition_source(
    condition::Id source_id,
    std::string new_revision,
    assets::Sha256Digest new_digest) {
    return dispatch_with_category(
        application::InvalidateConditionSourceAction{
            source_id, std::move(new_revision), new_digest},
        OperationCategory::provider);
}

core::Result<condition::Package> UiController::build_condition_package() const {
    const auto authoring = session_.condition_authoring();
    if (!authoring) {
        return core::Result<condition::Package>::failure(authoring.error().with_context(
            "CONDITION source publication"));
    }
    const auto snapshot = session_.snapshot();
    return condition::Package::publish(
        authoring.value(), snapshot.project_revision, authoring.value().project_namespace);
}

core::Result<condition::AdmissionResult> UiController::admit_condition_package() const {
    const auto package = build_condition_package();
    if (!package) return core::Result<condition::AdmissionResult>::failure(package.error());
    return condition::admit_for_consumer(package.value());
}

core::Result<void> UiController::publish_condition_package(
    const std::filesystem::path& path) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            "CONDITION publication path must name a file"));
    }
    const auto package = build_condition_package();
    if (!package) return core::Result<void>::failure(package.error());
    const auto admission = condition::admit_for_consumer(package.value());
    if (!admission) return core::Result<void>::failure(admission.error().with_context(
        "CONDITION publication admission"));
    if (admission.value().status != condition::PackageAdmissionStatus::admit) {
        return core::Result<void>::failure(validation(
            "CONDITION source publication was refused at the public consumer boundary"));
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        const bool exists = std::filesystem::exists(parent, error);
        if (error || !exists || !std::filesystem::is_directory(parent, error) || error) {
            return core::Result<void>::failure(io_error(
                "CONDITION publication parent is unavailable"));
        }
    }
    const auto temporary = preference_temporary_path(path);
    const std::string bytes = package.value().serialize();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return core::Result<void>::failure(io_error(
        "unable to create CONDITION source package"));
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return core::Result<void>::failure(io_error(
            "unable to write CONDITION source package"));
    }
    const auto replaced = atomic_replace(temporary, path);
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return replaced;
    }
    return core::Result<void>::success();
}

core::Result<void> UiController::configure_market_library(
    const std::filesystem::path& root) {
    if (root.empty()) {
        return core::Result<void>::failure(invalid("market library root must not be empty"));
    }
    market_library_ = std::make_unique<market::MarketLibrary>(root);
    market_view_ = MarketView{};
    market_view_.library_root = root;
    return refresh_market_library();
}

core::Result<void> UiController::refresh_market_library() {
    if (!market_library_) {
        market_view_ = MarketView{};
        const auto diagnostic = invalid_state("market library is not configured");
        record_market_failure(diagnostic);
        return core::Result<void>::failure(diagnostic);
    }

    const auto listed = market_library_->list();
    if (!listed) {
        const auto diagnostic = listed.error().with_context("local market library refresh");
        record_market_failure(diagnostic);
        return core::Result<void>::failure(diagnostic);
    }

    MarketView next;
    next.library_root = market_library_->root();
    next.status = listed.value().empty() ? MarketStatus::empty : MarketStatus::ready;
    next.status_text = listed.value().empty()
        ? "No validated data-only packages are installed."
        : std::to_string(listed.value().size()) + " validated data-only package(s) installed.";
    next.packages.reserve(listed.value().size());
    for (const auto& receipt : listed.value()) {
        const auto report = market_library_->inspect(
            receipt.package_id, receipt.listing_version_id);
        if (!report) {
            const auto diagnostic = report.error().with_context(
                "local market package refresh: " + receipt.package_id + "@" +
                receipt.listing_version_id);
            record_market_failure(diagnostic);
            return core::Result<void>::failure(diagnostic);
        }
        const auto& manifest = report.value().manifest;
        next.packages.push_back(MarketPackageView{
            manifest.package_id,
            manifest.listing_id,
            manifest.listing_version_id,
            manifest.publisher_id,
            manifest.license_id,
            manifest.source_revision,
            "sha256:" + manifest.content_digest.hex(),
            "sha256:" + report.value().computed_package_digest.hex(),
            report.value().payload_bytes,
            report.value().payload_files,
            receipt.publication_receipt_digest.has_value(),
        });
    }
    market_view_ = std::move(next);
    return core::Result<void>::success();
}

core::Result<market::MarketInstallReceipt> UiController::install_market_directory(
    const std::filesystem::path& package_root) {
    if (!market_library_) {
        const auto diagnostic = invalid_state("market library is not configured");
        record_market_failure(diagnostic);
        return core::Result<market::MarketInstallReceipt>::failure(diagnostic);
    }
    if (package_root.empty()) {
        const auto diagnostic = invalid("market package directory must not be empty");
        record_market_failure(diagnostic);
        return core::Result<market::MarketInstallReceipt>::failure(diagnostic);
    }
    const auto installed = market_library_->install_directory(package_root);
    if (!installed) {
        const auto diagnostic = installed.error().with_context("local market package install");
        record_market_failure(diagnostic);
        return core::Result<market::MarketInstallReceipt>::failure(diagnostic);
    }
    if (auto refreshed = refresh_market_library(); !refreshed) {
        return core::Result<market::MarketInstallReceipt>::failure(
            refreshed.error().with_context("market package installed but refresh failed"));
    }
    return installed;
}

core::Result<market::MarketInstallReceipt> UiController::install_market_archive(
    const std::filesystem::path& archive_path) {
    if (!market_library_) {
        const auto diagnostic = invalid_state("market library is not configured");
        record_market_failure(diagnostic);
        return core::Result<market::MarketInstallReceipt>::failure(diagnostic);
    }
    if (archive_path.empty()) {
        const auto diagnostic = invalid("market archive path must not be empty");
        record_market_failure(diagnostic);
        return core::Result<market::MarketInstallReceipt>::failure(diagnostic);
    }
    const auto installed = market_library_->install_archive(archive_path);
    if (!installed) {
        const auto diagnostic = installed.error().with_context("local market archive install");
        record_market_failure(diagnostic);
        return core::Result<market::MarketInstallReceipt>::failure(diagnostic);
    }
    if (auto refreshed = refresh_market_library(); !refreshed) {
        return core::Result<market::MarketInstallReceipt>::failure(
            refreshed.error().with_context("market archive installed but refresh failed"));
    }
    return installed;
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

core::Result<void> UiController::compile_surface_preview(
    std::uint32_t target_triangles,
    surface::RepresentationMode mode) {
    const auto current = session_.snapshot();
    SurfaceFidelityView view;
    view.project_generation = current.project_generation;
    view.project_revision = current.project_revision;
    view.target_triangles = target_triangles;
    view.mode = mode;

    const auto fail = [this, &view](
                           core::Diagnostic diagnostic,
                           SurfaceFidelityStatus status = SurfaceFidelityStatus::unavailable)
        -> core::Result<void> {
        view.status = status;
        view.status_text = diagnostic.message;
        view.diagnostic = diagnostic;
        surface_fidelity_ = view;
        return core::Result<void>::failure(std::move(diagnostic));
    };

    if (target_triangles == 0U) {
        return fail(invalid("surface fidelity preview requires at least one target triangle"));
    }
    if (target_triangles > kMaxSurfacePreviewTriangles) {
        return fail(unsupported(
            "surface fidelity preview exceeds the bounded native UI triangle budget"));
    }
    if (mode != surface::RepresentationMode::geometry_only &&
        mode != surface::RepresentationMode::fidelity_closure) {
        return fail(invalid("surface fidelity preview mode is invalid"));
    }

    const auto selected = selected_surface_object(current);
    if (!selected.has_value()) {
        return fail(invalid_state(
            "surface fidelity preview requires a selected object or mesh component"));
    }
    view.object = selected;
    const auto* instance = current.viewport.scene.find(*selected);
    if (instance == nullptr || instance->mesh == nullptr || !instance->mesh->valid()) {
        return fail(stale("selected object has no valid compiled viewport geometry"));
    }

    const auto source_triangles = instance->mesh->indices.size() / 3U;
    if (source_triangles == 0U || source_triangles > std::numeric_limits<std::uint32_t>::max()) {
        return fail(validation("selected object has no bounded source triangle set"));
    }
    if (source_triangles > kMaxSurfacePreviewTriangles) {
        return fail(unsupported(
            "selected object exceeds the bounded native UI source triangle budget"));
    }
    view.source_revision = instance->source_revision;
    view.source_triangles = static_cast<std::uint32_t>(source_triangles);
    if (target_triangles > view.source_triangles) {
        return fail(invalid(
            "surface fidelity target cannot exceed the selected source triangle count"));
    }

    const auto domain = surface::build_surface_domain(
        *instance->mesh,
        surface::SurfaceDomainId{selected->value},
        "scene-object",
        "unassigned");
    if (!domain) return fail(domain.error().with_context("surface fidelity domain"));

    surface::SurfaceClosureCompileOptions options;
    options.analysis.max_source_triangles = kMaxSurfacePreviewTriangles;
    options.target_triangles = target_triangles;
    const auto candidate = surface::compile_surface_representation(
        *instance->mesh, domain.value(), options, mode);
    if (!candidate) return fail(candidate.error().with_context("surface fidelity compile"));

    view.error = candidate.value().error;
    view.compiled = std::make_shared<const surface::SurfaceClosurePolygon>(candidate.value());
    view.domain_digest = domain.value().digest;
    view.representation_digest = candidate.value().digest;
    view.target_triangles = static_cast<std::uint32_t>(candidate.value().geometry.triangle_count());

    const auto admission = surface::admit_surface_representation(
        *instance->mesh, domain.value(), candidate.value());
    view.admitted = static_cast<bool>(admission);
    if (admission) {
        view.status = SurfaceFidelityStatus::ready;
        view.status_text = "Preview compiled and admitted by the appearance policy.";
        view.diagnostic.reset();
    } else {
        view.status = SurfaceFidelityStatus::rejected;
        view.status_text = "Preview compiled but was rejected by the appearance policy.";
        view.diagnostic = admission.error();
    }
    if (auto result = view.validate(); !result) return fail(result.error());
    surface_fidelity_ = std::move(view);
    return core::Result<void>::success();
}

core::Result<void> UiController::clear_surface_preview() {
    surface_fidelity_.reset();
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
        {"file.import-mesh", "Import Mesh", "Ctrl+I", true, {}},
        {"edit.duplicate", "Duplicate Selected Object", "Ctrl+D", true, {}},
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
        {"market.refresh", "Refresh Local Market Library", {},
         market_library_ != nullptr, market_library_ == nullptr
             ? "The local market library is not configured" : ""},
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

void UiController::record_market_failure(core::Diagnostic diagnostic) {
    market_view_.status = MarketStatus::error;
    market_view_.status_text = diagnostic.message;
    market_view_.packages.clear();
    if (ui_problems_.empty() || ui_problems_.back().code != diagnostic.code ||
        ui_problems_.back().message != diagnostic.message) {
        record_ui_problem(diagnostic);
    }
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
    result.agent_goal = agent_goal_;
    result.agent_plan_draft = agent_plan_draft_;
    if (agent_task_.has_value()) result.agent_task = agent_task_->snapshot();
    result.market = market_view_;
    if (surface_fidelity_.has_value()) {
        result.surface_fidelity = *surface_fidelity_;
        const auto& preview = *surface_fidelity_;
        if (preview.status == SurfaceFidelityStatus::ready ||
            preview.status == SurfaceFidelityStatus::rejected) {
            const auto selected = selected_surface_object(result);
            const auto* instance = selected.has_value()
                ? result.viewport.scene.find(*selected) : nullptr;
            const bool stale_preview = !selected.has_value() ||
                !preview.object.has_value() || *selected != *preview.object ||
                preview.project_generation != result.project_generation ||
                preview.project_revision != result.project_revision ||
                instance == nullptr || instance->mesh == nullptr ||
                instance->source_revision != preview.source_revision;
            if (stale_preview) {
                result.surface_fidelity.status = SurfaceFidelityStatus::stale;
                result.surface_fidelity.status_text =
                    "Preview is stale; selection or authored geometry changed.";
                result.surface_fidelity.admitted = false;
            }
        }
    }
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
