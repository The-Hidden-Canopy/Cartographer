#include <carto/ui/ui.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/obj.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/providers/registry.hpp>
#include <carto/condition/wave_import.hpp>

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
#include <span>
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

core::Result<std::vector<agent::HardConstraint>> compile_objective_constraints(
    std::string_view goal,
    const std::vector<agent::HardConstraint>& authored_constraints) {
    auto compiled = agent::compile_explicit_hard_constraints(goal);
    if (!compiled) {
        return core::Result<std::vector<agent::HardConstraint>>::failure(compiled.error());
    }

    auto constraints = authored_constraints;
    for (const auto& generated : compiled.value()) {
        const auto equivalent = std::find_if(
            constraints.begin(), constraints.end(), [&generated](const auto& existing) {
                return existing.kind == generated.kind &&
                    existing.object == generated.object &&
                    existing.maximum_displacement == generated.maximum_displacement;
            });
        if (equivalent != constraints.end()) continue;

        const bool id_collision = std::any_of(
            constraints.begin(), constraints.end(), [&generated](const auto& existing) {
                return existing.id == generated.id;
            });
        if (id_collision) {
            return core::Result<std::vector<agent::HardConstraint>>::failure(invalid(
                "compiled objective constraint identity collides with an authored constraint"));
        }
        constraints.push_back(generated);
    }
    return core::Result<std::vector<agent::HardConstraint>>::success(std::move(constraints));
}

template <typename Record>
core::Result<void> append_condition_record(
    std::vector<Record>& records, Record record, std::string_view label) {
    if (record.id == 0U) {
        const auto maximum = std::max_element(records.begin(), records.end(),
            [](const Record& left, const Record& right) { return left.id < right.id; });
        if (maximum != records.end() && maximum->id == std::numeric_limits<condition::Id>::max()) {
            return core::Result<void>::failure(invalid_state(
                std::string(label) + " identity space is exhausted"));
        }
        record.id = maximum == records.end() ? 1U : maximum->id + 1U;
    }
    const auto insertion = std::lower_bound(records.begin(), records.end(), record.id,
        [](const Record& existing, condition::Id id) { return existing.id < id; });
    if (insertion != records.end() && insertion->id == record.id) {
        return core::Result<void>::failure(invalid(
            std::string(label) + " identity already exists"));
    }
    if (auto valid_record = record.validate(); !valid_record) return valid_record;
    records.insert(insertion, std::move(record));
    return core::Result<void>::success();
}

std::optional<scene::ObjectId> selected_surface_object(
    const application::ApplicationSnapshot& snapshot) {
    if (snapshot.selection.component_object.has_value()) {
        return snapshot.selection.component_object;
    }
    if (!snapshot.selection.objects.empty()) return snapshot.selection.objects.front();
    return std::nullopt;
}

constexpr std::size_t kMaxAgentDemonstrationTargetSets = 16U;
constexpr std::size_t kMaxExpandedDemonstrationPlanSteps = 256U;

std::vector<AgentDemonstrationTargetBindingView> demonstration_binding_requirements(
    const agent::AgentDemonstrationTrace& trace,
    std::size_t target_set_count) {
    std::vector<AgentDemonstrationTargetBindingView> requirements;
    for (std::size_t target_set_index = 0U;
         target_set_index < target_set_count; ++target_set_index) {
        for (const auto& step : trace.steps) {
            const auto existing = std::find_if(requirements.begin(), requirements.end(),
                [target_set_index, &step](const auto& requirement) {
                    return requirement.target_set_index == target_set_index &&
                        requirement.target_alias == step.target_alias &&
                        requirement.component_alias == step.component_alias;
                });
            if (existing == requirements.end()) {
                requirements.push_back(AgentDemonstrationTargetBindingView{
                    target_set_index, step.target_alias, step.component_alias,
                    step.target_kind, false, {}});
            }
        }
    }
    return requirements;
}

bool supports_demonstration_structural_matching(
    const agent::AgentDemonstrationTrace& trace) {
    if (trace.steps.empty()) return false;
    const auto& first = trace.steps.front();
    if (!first.structure_signature.has_value() ||
        !first.result_object_structure_signature.has_value()) {
        return false;
    }
    if (trace.steps.size() == 1U) return true;
    if (first.target_kind == agent::AgentDemonstrationTargetKind::vertex &&
        first.tool_id == "mesh.set-vertex-position" && first.target_alias != 0U &&
        first.component_alias != 0U) {
        return std::all_of(trace.steps.begin(), trace.steps.end(), [&first](const auto& step) {
            return step.target_kind == agent::AgentDemonstrationTargetKind::vertex &&
                step.tool_id == "mesh.set-vertex-position" &&
                step.target_alias == first.target_alias &&
                step.component_alias == first.component_alias &&
                step.structure_signature == first.structure_signature &&
                step.result_object_structure_signature == first.result_object_structure_signature;
        });
    }
    if (first.target_kind != agent::AgentDemonstrationTargetKind::object ||
        first.tool_id != "object.translate" || first.target_alias == 0U ||
        first.component_alias != 0U ||
        first.structure_signature != first.result_object_structure_signature) {
        return false;
    }
    return std::all_of(trace.steps.begin(), trace.steps.end(), [&first](const auto& step) {
        return step.target_kind == agent::AgentDemonstrationTargetKind::object &&
            step.tool_id == "object.translate" &&
            step.target_alias == first.target_alias &&
            step.component_alias == first.component_alias &&
            step.structure_signature == first.structure_signature &&
            step.result_object_structure_signature == first.result_object_structure_signature;
    });
}

core::Result<std::string> demonstration_number_text(double value) {
    if (!std::isfinite(value)) {
        return core::Result<std::string>::failure(invalid(
            "demonstration parameter is not finite"));
    }
    std::array<char, 64U> buffer{};
    const auto converted = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
    if (converted.ec != std::errc{}) {
        return core::Result<std::string>::failure(invalid(
            "demonstration parameter could not be formatted"));
    }
    return core::Result<std::string>::success(
        std::string(buffer.data(), converted.ptr));
}

core::Result<std::string> demonstration_step_intent(
    const agent::AgentDemonstrationTraceStep& step,
    const agent::TargetRef& target,
    const application::ApplicationSnapshot& snapshot,
    std::optional<core::Vec3d> vertex_base_position = std::nullopt) {
    if (auto result = step.parameters.validate(step.tool_id); !result) {
        return core::Result<std::string>::failure(result.error());
    }
    if (step.target_kind == agent::AgentDemonstrationTargetKind::object) {
        if (!std::holds_alternative<std::monostate>(target.component) ||
            step.tool_id != "object.translate") {
            return core::Result<std::string>::failure(invalid(
                "object demonstration binding does not match its native operation"));
        }
        const auto x = demonstration_number_text(step.parameters.relative_displacement.x);
        const auto y = demonstration_number_text(step.parameters.relative_displacement.y);
        const auto z = demonstration_number_text(step.parameters.relative_displacement.z);
        if (!x) return core::Result<std::string>::failure(x.error());
        if (!y) return core::Result<std::string>::failure(y.error());
        if (!z) return core::Result<std::string>::failure(z.error());
        return core::Result<std::string>::success(
            "translate object by " + x.value() + " " + y.value() + " " + z.value());
    }

    const auto* instance = snapshot.viewport.scene.find(target.object);
    if (instance == nullptr || instance->mesh == nullptr) {
        return core::Result<std::string>::failure(stale(
            "demonstration target has no current mesh in the viewport snapshot"));
    }
    if (step.target_kind == agent::AgentDemonstrationTargetKind::face) {
        const auto* face_id = std::get_if<geometry::FaceId>(&target.component);
        if (face_id == nullptr ||
            (step.tool_id != "mesh.inset-face" && step.tool_id != "mesh.extrude-face")) {
            return core::Result<std::string>::failure(invalid(
                "face demonstration binding does not match its native operation"));
        }
        double longest_edge = 0.0;
        std::size_t boundary_edge_count = 0U;
        for (std::size_t triangle = 0U;
             triangle < instance->mesh->triangle_faces.size(); ++triangle) {
            if (instance->mesh->triangle_faces[triangle] != *face_id) continue;
            for (std::size_t edge = 0U; edge < 3U; ++edge) {
                if (!instance->mesh->triangle_edges[triangle][edge].has_value()) continue;
                const std::size_t first_index = static_cast<std::size_t>(
                    instance->mesh->indices[triangle * 3U + edge]);
                const std::size_t second_index = static_cast<std::size_t>(
                    instance->mesh->indices[triangle * 3U + (edge + 1U) % 3U]);
                if (first_index >= instance->mesh->positions.size() ||
                    second_index >= instance->mesh->positions.size()) {
                    return core::Result<std::string>::failure(stale(
                        "bound demonstration face has invalid compiled vertex data"));
                }
                ++boundary_edge_count;
                longest_edge = std::max(longest_edge,
                    (instance->mesh->positions[second_index] -
                        instance->mesh->positions[first_index]).length());
            }
        }
        if (boundary_edge_count < 3U) {
            return core::Result<std::string>::failure(stale(
                "bound demonstration face is missing or degenerate"));
        }
        const double distance = step.parameters.normalized_distance * longest_edge;
        if (!std::isfinite(distance) || distance <= 0.0) {
            return core::Result<std::string>::failure(invalid(
                "normalized demonstration face distance is invalid for the selected target"));
        }
        const auto formatted = demonstration_number_text(distance);
        if (!formatted) return core::Result<std::string>::failure(formatted.error());
        const std::string operation = step.tool_id == "mesh.inset-face"
            ? "inset face " : "extrude face ";
        return core::Result<std::string>::success(operation + formatted.value());
    }
    if (step.target_kind == agent::AgentDemonstrationTargetKind::vertex) {
        const auto* vertex_id = std::get_if<geometry::VertexId>(&target.component);
        if (vertex_id == nullptr || step.tool_id != "mesh.set-vertex-position") {
            return core::Result<std::string>::failure(invalid(
                "vertex demonstration binding does not match its native operation"));
        }
        const auto vertex = std::find(
            instance->mesh->vertex_ids.begin(), instance->mesh->vertex_ids.end(), *vertex_id);
        if (vertex == instance->mesh->vertex_ids.end()) {
            return core::Result<std::string>::failure(stale(
                "bound demonstration vertex is missing"));
        }
        const auto vertex_index = static_cast<std::size_t>(std::distance(
            instance->mesh->vertex_ids.begin(), vertex));
        const auto position = vertex_base_position.value_or(
            instance->mesh->positions[vertex_index]) +
            step.parameters.relative_displacement;
        const auto x = demonstration_number_text(position.x);
        const auto y = demonstration_number_text(position.y);
        const auto z = demonstration_number_text(position.z);
        if (!x) return core::Result<std::string>::failure(x.error());
        if (!y) return core::Result<std::string>::failure(y.error());
        if (!z) return core::Result<std::string>::failure(z.error());
        return core::Result<std::string>::success(
            "move vertex to " + x.value() + " " + y.value() + " " + z.value());
    }
    return core::Result<std::string>::failure(invalid(
        "demonstration trace target kind is unsupported"));
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
        {Workspace::material, "material", "Material", asset_panels,
         BottomPanel::assets, true, {}},
        future(Workspace::animate, "animate", "Animate", timeline_panels, BottomPanel::timeline,
               "animation tools are not available in this build"),
        {Workspace::review, "review", "Review", model_panels, BottomPanel::problems,
         true, {}},
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
            std::is_same_v<Action, application::CreateMaterialAction> ||
            std::is_same_v<Action, application::UpdateMaterialAction> ||
            std::is_same_v<Action, application::SetMaterialRegionAssignmentAction> ||
            std::is_same_v<Action, application::SetMaterialGraphAction> ||
            std::is_same_v<Action, application::SetMaterialGraphBindingAction> ||
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
            std::is_same_v<Action, application::BindConditionAssetObjectAction> ||
            std::is_same_v<Action, application::InvalidateConditionSourceAction> ||
            std::is_same_v<Action, application::CreateSpatialFieldAction> ||
            std::is_same_v<Action, application::PaintSpatialFieldAction>;
    }, action);
}

ai::Context make_ai_context(
    const application::ApplicationSnapshot& snapshot,
    const editor::AuthoringContext& authoring,
    std::uint32_t permitted_effects =
        ai::effect_mask(ai::CapabilityEffect::geometry) |
        ai::effect_mask(ai::CapabilityEffect::topology)) {
    const auto ontology = ai::build_operation_ontology(snapshot.tools);
    return ai::Context{
        snapshot.project_revision,
        authoring,
        ai::project_capabilities(ontology, permitted_effects)};
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

struct PendingMeshSource {
    std::filesystem::path project_root;
    std::filesystem::path staged_path;
    std::string identity;
    std::string bytes;
    assets::Sha256Digest digest;
};

struct StagedMeshSource {
    std::string relative_path;
    assets::Sha256Digest digest;
};

bool is_within_directory(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(directory);
    if (relative.empty() || relative.is_absolute()) return false;
    for (const auto& component : relative) {
        if (component == "..") return false;
    }
    return true;
}

core::Result<void> validate_managed_directory_chain(
    const std::filesystem::path& project_root,
    const std::filesystem::path& directory) {
    if (!is_within_directory(project_root, directory)) {
        return core::Result<void>::failure(validation(
            "managed mesh directory escaped the saved project directory"));
    }
    const auto relative = directory.lexically_relative(project_root);
    auto cursor = project_root;
    std::error_code error;
    for (const auto& component : relative) {
        cursor /= component;
        const auto status = std::filesystem::symlink_status(cursor, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            return core::Result<void>::failure(io_error(
                "unable to inspect managed mesh directory"));
        }
        error.clear();
        if (std::filesystem::is_symlink(status) ||
            (std::filesystem::exists(status) && !std::filesystem::is_directory(status))) {
            return core::Result<void>::failure(validation(
                "managed mesh directory contains a link or non-directory component"));
        }
    }
    return core::Result<void>::success();
}

core::Result<PendingMeshSource> stage_mesh_source(
    const std::filesystem::path& source_path,
    const std::filesystem::path& project_path) {
    constexpr std::size_t kMaxMeshSourceBytes = 128U * 1024U * 1024U;
    std::error_code error;
    const auto canonical_source = std::filesystem::canonical(source_path, error);
    if (error || !std::filesystem::is_regular_file(canonical_source, error) || error) {
        return core::Result<PendingMeshSource>::failure(io_error(
            "mesh source is not a readable regular file"));
    }
    auto bytes = read_bounded_file(
        canonical_source, kMaxMeshSourceBytes, "mesh source");
    if (!bytes) return core::Result<PendingMeshSource>::failure(bytes.error());

    const std::filesystem::path requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return core::Result<PendingMeshSource>::failure(io_error(
        "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) return core::Result<PendingMeshSource>::failure(io_error(
        "unable to resolve the saved project directory"));

    const auto* source_data = reinterpret_cast<const std::uint8_t*>(bytes.value().data());
    const std::span<const std::uint8_t> source_span(source_data, bytes.value().size());
    const auto digest = assets::sha256(source_span);
    const std::string canonical_locator = canonical_source.generic_string();
    const auto* locator_data = reinterpret_cast<const std::uint8_t*>(canonical_locator.data());
    const auto identity_digest = assets::sha256(std::span<const std::uint8_t>(
        locator_data, canonical_locator.size()));
    const std::string identity = "mesh.source." + identity_digest.hex().substr(0U, 32U);

    const auto staging_root = project_root / ".carto-assets" / "import-staging";
    if (auto checked = validate_managed_directory_chain(project_root, staging_root); !checked) {
        return core::Result<PendingMeshSource>::failure(checked.error());
    }
    std::filesystem::create_directories(staging_root, error);
    if (error) return core::Result<PendingMeshSource>::failure(io_error(
        "unable to create the private mesh import staging directory"));
    const auto staged_path = preference_temporary_path(staging_root / (digest.hex() + ".snapshot"));
    std::ofstream staged_file(staged_path, std::ios::binary | std::ios::trunc);
    if (!staged_file) return core::Result<PendingMeshSource>::failure(io_error(
        "unable to create a private mesh import snapshot"));
    staged_file.write(bytes.value().data(), static_cast<std::streamsize>(bytes.value().size()));
    staged_file.flush();
    staged_file.close();
    if (!staged_file) {
        std::filesystem::remove(staged_path, error);
        return core::Result<PendingMeshSource>::failure(io_error(
            "unable to write the private mesh import snapshot"));
    }
    return core::Result<PendingMeshSource>::success(PendingMeshSource{
        project_root, staged_path, identity, std::move(bytes.value()), digest});
}

void remove_pending_mesh_source(const PendingMeshSource& pending) {
    std::error_code error;
    const auto parent = std::filesystem::weakly_canonical(
        pending.staged_path.parent_path(), error);
    if (error || !is_within_directory(pending.project_root, parent)) return;
    std::filesystem::remove(pending.staged_path, error);
}

core::Result<StagedMeshSource> persist_mesh_source(
    const PendingMeshSource& pending,
    MeshImportFormat format) {
    constexpr std::size_t kMaxMeshSourceBytes = 128U * 1024U * 1024U;
    const auto staged_bytes = read_bounded_file(
        pending.staged_path, kMaxMeshSourceBytes, "staged mesh source");
    if (!staged_bytes) return core::Result<StagedMeshSource>::failure(staged_bytes.error());
    const auto* staged_data = reinterpret_cast<const std::uint8_t*>(staged_bytes.value().data());
    if (assets::sha256(std::span<const std::uint8_t>(
            staged_data, staged_bytes.value().size())) != pending.digest) {
        return core::Result<StagedMeshSource>::failure(stale(
            "staged mesh source changed while the importer was reading it"));
    }
    const auto* source_data = reinterpret_cast<const std::uint8_t*>(pending.bytes.data());
    const std::span<const std::uint8_t> source_span(source_data, pending.bytes.size());
    const auto store_root = pending.project_root / ".carto-assets" / "source-blobs";
    assets::BlobStore store(store_root);
    const auto destination = store.path_for(pending.digest);
    if (auto checked = validate_managed_directory_chain(
            pending.project_root, destination.parent_path()); !checked) {
        return core::Result<StagedMeshSource>::failure(checked.error());
    }
    std::error_code error;
    const auto destination_status = std::filesystem::symlink_status(destination, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return core::Result<StagedMeshSource>::failure(io_error(
            "unable to inspect managed mesh source file"));
    }
    error.clear();
    if (std::filesystem::is_symlink(destination_status)) {
        return core::Result<StagedMeshSource>::failure(validation(
            "managed mesh source destination cannot be a symbolic link"));
    }
    std::string media_type;
    switch (format) {
    case MeshImportFormat::obj: media_type = "model/obj"; break;
    case MeshImportFormat::ply: media_type = "model/ply"; break;
    case MeshImportFormat::stl: media_type = "model/stl"; break;
    }
    const auto stored = store.put(source_span, std::move(media_type));
    if (!stored) return core::Result<StagedMeshSource>::failure(stored.error());
    if (stored.value().digest != pending.digest) {
        return core::Result<StagedMeshSource>::failure(validation(
            "managed mesh source digest changed during persistence"));
    }
    const auto canonical_destination = std::filesystem::canonical(destination, error);
    if (error || !is_within_directory(pending.project_root, canonical_destination)) {
        return core::Result<StagedMeshSource>::failure(validation(
            "managed mesh source resolved outside the saved project directory"));
    }
    const auto relative =
        canonical_destination.lexically_relative(pending.project_root).generic_string();
    if (relative.empty() || relative.starts_with("../")) {
        return core::Result<StagedMeshSource>::failure(validation(
            "managed mesh source has no safe project-relative path"));
    }
    return core::Result<StagedMeshSource>::success(StagedMeshSource{
        relative, pending.digest});
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

core::Result<project::MaterialGraph> UiController::material_graph_source(
    std::string_view identity) const {
    return session_.material_graph_source(identity);
}

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

core::Result<void> UiController::queue_agent_step(
    std::string intent,
    std::vector<std::string> dependencies) {
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "a loaded checkpoint plan cannot be edited; resume or clear it first"));
    }
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status != agent::TaskStatus::accepted && status != agent::TaskStatus::rejected) {
            return core::Result<void>::failure(invalid_state(
                "accept or reject the current speculative task before editing its plan"));
        }
        agent_task_.reset();
        agent_condition_inventory_.reset();
        agent_plan_draft_.clear();
        agent_hard_constraints_draft_.clear();
        agent_clarifications_draft_.clear();
        agent_plan_base_revision_.reset();
        agent_goal_.clear();
        agent_plan_id_.clear();
        agent_objective_id_.clear();
        agent_repair_mode_ = false;
    }
    if (std::any_of(agent_clarifications_draft_.begin(), agent_clarifications_draft_.end(),
            [](const auto& question) { return question.human_receipt.has_value(); })) {
        return core::Result<void>::failure(invalid_state(
            "clear the objective and re-capture clarification after changing its targets"));
    }
    if (agent_plan_draft_.size() >= 16U) {
        return core::Result<void>::failure(invalid_state(
            "agent plan reached the local 16-command work budget"));
    }
    for (const auto& dependency : dependencies) {
        if (std::none_of(agent_plan_draft_.begin(), agent_plan_draft_.end(),
                [&dependency](const agent::AgentPlanStep& candidate) {
                    return candidate.id == dependency;
                })) {
            return core::Result<void>::failure(invalid_state(
                "plan step dependency must name an already captured step"));
        }
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
    if (authoring.value().selection_mode == editor::SelectionMode::object &&
        authoring.value().objects.size() == 1U) {
        target.object = authoring.value().objects.front();
        target.component = std::monostate{};
    } else if (authoring.value().selection_mode == editor::SelectionMode::face &&
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
            "capture one selected object, face, or vertex for each agent plan step"));
    }

    agent::AgentPlanStep step{
        "step-" + std::to_string(agent_plan_draft_.size() + 1U),
        std::move(target),
        std::move(intent),
        std::move(dependencies)};
    if (auto result = step.validate(); !result) {
        return core::Result<void>::failure(result.error());
    }
    if (!agent_plan_base_revision_.has_value()) {
        agent_plan_base_revision_ = snapshot.project_revision;
    }
    agent_plan_draft_.push_back(std::move(step));
    return core::Result<void>::success();
}

core::Result<void> UiController::set_agent_recipe(std::string recipe_id) {
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "a loaded checkpoint recipe cannot be edited; resume or clear it first"));
    }
    if (!recipe_id.empty() && agent::find_agent_recipe(recipe_id) == nullptr) {
        return core::Result<void>::failure(invalid_state(
            "selected agent recipe is not registered"));
    }
    if (recipe_id == agent_recipe_id_) return core::Result<void>::success();
    if (std::any_of(agent_clarifications_draft_.begin(),
            agent_clarifications_draft_.end(), [](const auto& question) {
                return question.human_receipt.has_value();
            })) {
        return core::Result<void>::failure(invalid_state(
            "clear and re-answer clarifications before changing the objective recipe"));
    }
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status != agent::TaskStatus::accepted && status != agent::TaskStatus::rejected) {
            return core::Result<void>::failure(invalid_state(
                "the recipe is locked while a speculative task is active"));
        }
        agent_task_.reset();
        agent_condition_inventory_.reset();
        agent_plan_draft_.clear();
        agent_hard_constraints_draft_.clear();
        agent_clarifications_draft_.clear();
        agent_plan_base_revision_.reset();
        agent_goal_.clear();
        agent_plan_id_.clear();
        agent_objective_id_.clear();
        agent_repair_mode_ = false;
    }
    agent_recipe_id_ = std::move(recipe_id);
    return core::Result<void>::success();
}

core::Result<void> UiController::set_agent_hard_constraints(
    std::vector<agent::HardConstraint> constraints) {
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "a loaded checkpoint objective cannot be edited; resume or clear it first"));
    }
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status != agent::TaskStatus::accepted && status != agent::TaskStatus::rejected) {
            return core::Result<void>::failure(invalid_state(
                "objective constraints are locked while a speculative task is active"));
        }
        agent_task_.reset();
        agent_condition_inventory_.reset();
        agent_plan_draft_.clear();
        agent_hard_constraints_draft_.clear();
        agent_clarifications_draft_.clear();
        agent_plan_base_revision_.reset();
        agent_goal_.clear();
        agent_plan_id_.clear();
        agent_objective_id_.clear();
        agent_repair_mode_ = false;
    }
    if (agent_plan_draft_.empty() || !agent_plan_base_revision_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "capture at least one plan target before setting objective constraints"));
    }
    if (session_.snapshot().project_revision != *agent_plan_base_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed after the first agent plan target was captured"));
    }
    if (std::any_of(agent_clarifications_draft_.begin(),
            agent_clarifications_draft_.end(), [](const auto& question) {
                return question.human_receipt.has_value();
            })) {
        return core::Result<void>::failure(invalid_state(
            "clear and re-answer clarifications before changing objective constraints"));
    }
    if (constraints.size() > 256U) {
        return core::Result<void>::failure(invalid_state(
            "objective exceeds the 256-constraint safety limit"));
    }

    std::vector<scene::ObjectId> scope;
    for (const auto& step : agent_plan_draft_) {
        if (std::find(scope.begin(), scope.end(), step.target.object) == scope.end()) {
            scope.push_back(step.target.object);
        }
    }
    std::set<std::string> constraint_ids;
    for (const auto& constraint : constraints) {
        if (auto result = constraint.validate(); !result) {
            return result;
        }
        if (!constraint_ids.insert(constraint.id).second) {
            return core::Result<void>::failure(invalid(
                "objective constraints contain a duplicate identity"));
        }
        if (constraint.object.has_value() &&
            std::find(scope.begin(), scope.end(), *constraint.object) == scope.end()) {
            return core::Result<void>::failure(invalid_state(
                "objective constraint target is outside the captured plan scope"));
        }
    }
    for (const auto& question : agent_clarifications_draft_) {
        if (auto result = question.validate(scope, constraints); !result) return result;
    }
    agent_hard_constraints_draft_ = std::move(constraints);
    return core::Result<void>::success();
}

core::Result<void> UiController::queue_agent_clarification(
    std::string prompt,
    agent::ClarificationImpact impact) {
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "a loaded checkpoint objective cannot be edited; resume or clear it first"));
    }
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status != agent::TaskStatus::accepted && status != agent::TaskStatus::rejected) {
            return core::Result<void>::failure(invalid_state(
                "accept or reject the current speculative task before editing its objective"));
        }
        agent_task_.reset();
        agent_condition_inventory_.reset();
        agent_plan_draft_.clear();
        agent_hard_constraints_draft_.clear();
        agent_clarifications_draft_.clear();
        agent_plan_base_revision_.reset();
        agent_goal_.clear();
        agent_plan_id_.clear();
        agent_objective_id_.clear();
        agent_repair_mode_ = false;
    }
    if (agent_plan_draft_.empty() || !agent_plan_base_revision_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "capture at least one plan target before adding an objective clarification"));
    }
    const auto snapshot = session_.snapshot();
    if (snapshot.project_revision != *agent_plan_base_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed after the first agent plan target was captured"));
    }
    if (!agent_goal_.empty() || std::any_of(
            agent_clarifications_draft_.begin(), agent_clarifications_draft_.end(),
            [](const auto& question) { return question.human_receipt.has_value(); })) {
        return core::Result<void>::failure(invalid_state(
            "clarifications are fixed after the first human answer; clear and rebuild the objective"));
    }
    if (agent_clarifications_draft_.size() >= 128U) {
        return core::Result<void>::failure(invalid_state(
            "objective reached the 128-question safety limit"));
    }

    std::vector<scene::ObjectId> scope;
    for (const auto& step : agent_plan_draft_) {
        if (std::find(scope.begin(), scope.end(), step.target.object) == scope.end()) {
            scope.push_back(step.target.object);
        }
    }
    agent::ClarificationQuestion question;
    question.id = "question-" + std::to_string(agent_clarifications_draft_.size() + 1U);
    question.prompt = std::move(prompt);
    question.rationale = "The operator requires an explicit answer before this plan may run.";
    question.impacts = {impact};
    question.affected_objects = std::move(scope);
    question.downstream_plan_branches = agent_plan_draft_.size();
    question.requires_explicit_answer = true;
    if (auto result = question.validate(question.affected_objects, {}); !result) {
        return result;
    }
    agent_clarifications_draft_.push_back(std::move(question));
    return core::Result<void>::success();
}

core::Result<void> UiController::answer_agent_clarification(
    std::string goal,
    std::string_view question_id,
    std::string answer) {
    if (agent_task_.has_value() &&
        (!agent_repair_mode_ ||
         agent_task_->snapshot().status != agent::TaskStatus::blocked)) {
        return core::Result<void>::failure(invalid_state(
            "clarification answers can be changed only while reviewing a blocked-plan repair"));
    }
    if (agent_plan_draft_.empty() || !agent_plan_base_revision_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "capture at least one plan target before answering an objective clarification"));
    }
    if (session_.snapshot().project_revision != *agent_plan_base_revision_) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "project changed after the first agent plan target was captured"));
    }
    if (!agent_goal_.empty() && agent_goal_ != goal) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "goal changed after a human clarification was recorded; clear and answer again"));
    }
    auto constraints = compile_objective_constraints(goal, agent_hard_constraints_draft_);
    if (!constraints) {
        return core::Result<void>::failure(constraints.error());
    }
    std::vector<scene::ObjectId> scope;
    for (const auto& step : agent_plan_draft_) {
        if (std::find(scope.begin(), scope.end(), step.target.object) == scope.end()) {
            scope.push_back(step.target.object);
        }
    }
    const std::uint64_t task_sequence = next_agent_task_id_ ==
        std::numeric_limits<std::uint64_t>::max() ? 1U : next_agent_task_id_;
    const std::string task_id = agent_plan_id_.empty()
        ? "agent-task-" + std::to_string(task_sequence) : agent_plan_id_;
    const std::string objective_id = agent_objective_id_.empty()
        ? task_id : agent_objective_id_;
    agent::AuthoringObjective objective{
        objective_id, std::move(goal),
        *agent_plan_base_revision_, std::move(scope), {},
        std::move(constraints.value()), agent_clarifications_draft_, agent_recipe_id_};
    agent::AgentPlan plan{task_id, std::move(objective), agent_plan_draft_};
    const auto answered = application::HumanApplicationAccess::answer_agent_clarification(
        plan, question_id, std::nullopt, std::move(answer));
    if (!answered) return answered;
    agent_goal_ = plan.objective.goal;
    agent_clarifications_draft_ = std::move(plan.objective.clarifications);
    return core::Result<void>::success();
}

core::Result<agent::AgentTaskSnapshot> UiController::prepare_agent_plan(
    std::string goal,
    std::optional<agent::AgentWorkEnvelope> requested_envelope) {
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "resume the loaded checkpoint through its revalidation boundary"));
    }
    if (agent_plan_draft_.empty() || !agent_plan_base_revision_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "capture at least one selected object, face, or vertex before running an agent plan"));
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
    if (!agent_goal_.empty() && agent_goal_ != goal) {
        return core::Result<agent::AgentTaskSnapshot>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "goal changed after a human clarification was recorded; clear and answer again"));
    }
    auto constraints = compile_objective_constraints(goal, agent_hard_constraints_draft_);
    if (!constraints) {
        return core::Result<agent::AgentTaskSnapshot>::failure(constraints.error());
    }
    const std::uint64_t task_sequence = next_agent_task_id_ ==
        std::numeric_limits<std::uint64_t>::max() ? 1U : next_agent_task_id_;
    const std::string task_id = "agent-task-" + std::to_string(task_sequence);
    std::vector<scene::ObjectId> scope;
    for (const auto& step : agent_plan_draft_) {
        if (std::find(scope.begin(), scope.end(), step.target.object) == scope.end()) {
            scope.push_back(step.target.object);
        }
    }
    const bool permits_transform = requested_envelope.has_value() &&
        (requested_envelope->permitted_effects &
            agent::effect_mask(agent::Effect::transform)) != 0U;
    if (permits_transform) {
        std::vector<scene::ObjectId> transform_roots;
        for (const auto& step : agent_plan_draft_) {
            if (std::holds_alternative<std::monostate>(step.target.component) &&
                std::find(transform_roots.begin(), transform_roots.end(),
                    step.target.object) == transform_roots.end()) {
                transform_roots.push_back(step.target.object);
            }
        }
        if (transform_roots.empty()) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "transform permission requires at least one captured object target"));
        }
        for (std::size_t cursor = 0U; cursor < transform_roots.size(); ++cursor) {
            const scene::ObjectId parent = transform_roots[cursor];
            for (const auto& object : snapshot.objects) {
                if (object.object.parent.has_value() && *object.object.parent == parent &&
                    std::find(scope.begin(), scope.end(), object.object.id) == scope.end()) {
                    scope.push_back(object.object.id);
                    transform_roots.push_back(object.object.id);
                }
            }
        }
    }
    if (requested_envelope.has_value() &&
        !requested_envelope->scope_objects.empty()) {
        auto requested_scope = requested_envelope->scope_objects;
        auto captured_scope = scope;
        std::sort(requested_scope.begin(), requested_scope.end());
        std::sort(captured_scope.begin(), captured_scope.end());
        if (requested_scope != captured_scope) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "requested work envelope scope must match the captured target subtree"));
        }
    }
    agent::AgentPlan plan{
        task_id,
        agent::AuthoringObjective{
            agent_objective_id_.empty() ? task_id : agent_objective_id_,
            std::move(goal), *agent_plan_base_revision_, scope, {},
            std::move(constraints.value()),
            agent_clarifications_draft_, agent_recipe_id_},
        agent_plan_draft_};
    const std::string task_goal = plan.objective.goal;
    agent::AgentWorkEnvelope envelope = requested_envelope.value_or(
        agent::AgentWorkEnvelope{});
    envelope.scope_objects = scope;
    envelope.max_commands = std::min<std::size_t>(envelope.max_commands, 16U);
    if (!requested_envelope.has_value()) envelope.max_objects_touched = scope.size();
    auto task = agent::AgentTask::begin(session_, std::move(plan), std::move(envelope));
    if (!task) {
        record_ui_problem(task.error().with_context("agent workbench plan"));
        return core::Result<agent::AgentTaskSnapshot>::failure(task.error());
    }
    next_agent_task_id_ = task_sequence == std::numeric_limits<std::uint64_t>::max()
        ? 1U : task_sequence + 1U;
    agent_goal_ = task_goal;
    agent_plan_id_ = task_id;
    if (agent_objective_id_.empty()) agent_objective_id_ = task_id;
    agent_repair_mode_ = false;
    agent_condition_inventory_.reset();
    agent_task_.emplace(std::move(task.value()));
    return core::Result<agent::AgentTaskSnapshot>::success(agent_task_->snapshot());
}

core::Result<agent::AgentTaskSnapshot> UiController::run_agent_plan(
    std::string goal,
    std::optional<agent::AgentWorkEnvelope> requested_envelope) {
    if (agent_task_.has_value()) {
        const auto task = agent_task_->snapshot();
        if (task.status != agent::TaskStatus::prepared) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "a speculative task is already active"));
        }
        if (goal != agent_goal_) {
            return core::Result<agent::AgentTaskSnapshot>::failure(stale(
                "prepared task goal changed; clear and prepare the objective again"));
        }
        if (requested_envelope.has_value()) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "the work envelope is locked after task preparation"));
        }
        return run_prepared_agent_plan();
    }
    auto prepared = prepare_agent_plan(std::move(goal), std::move(requested_envelope));
    if (!prepared) return prepared;
    return run_prepared_agent_plan();
}

core::Result<agent::AgentTaskSnapshot> UiController::run_prepared_agent_plan() {
    if (!agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "there is no prepared speculative agent task to run"));
    }
    const auto task = agent_task_->snapshot();
    if (task.status != agent::TaskStatus::prepared) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "only a prepared agent task can start execution"));
    }
    if (session_.snapshot().project_revision != task.base_revision) {
        return core::Result<agent::AgentTaskSnapshot>::failure(stale(
            "project changed after task preparation; discard and prepare against the current revision"));
    }
    auto result = agent_task_->run();
    if (!result) {
        record_ui_problem(result.error().with_context("agent workbench execution"));
        return result;
    }
    return result;
}

core::Result<agent::SceneQueryResult> UiController::query_agent_condition_inventory() {
    core::Result<agent::SceneQueryResult> queried =
        core::Result<agent::SceneQueryResult>::failure(invalid_state(
            "CONDITION inventory query was not initialized"));
    const std::optional<agent::AgentTaskSnapshot> current_task =
        agent_task_.has_value()
            ? std::optional<agent::AgentTaskSnapshot>(agent_task_->snapshot())
            : std::nullopt;
    const bool active_task = current_task.has_value() &&
        current_task->status != agent::TaskStatus::accepted &&
        current_task->status != agent::TaskStatus::rejected;
    if (active_task) {
        agent_condition_inventory_scope_ = current_task->scope_objects;
        queried = agent::SceneQuery::execute(*agent_task_, {
            agent::ConditionAssetInventoryQuery{std::nullopt}, 512U});
    } else {
        const auto source_snapshot = session_.snapshot();
        agent_condition_inventory_scope_ = source_snapshot.selection.objects;
        if (source_snapshot.selection.component_object.has_value() &&
            std::find(agent_condition_inventory_scope_.begin(),
                agent_condition_inventory_scope_.end(),
                *source_snapshot.selection.component_object) ==
                agent_condition_inventory_scope_.end()) {
            agent_condition_inventory_scope_.push_back(
                *source_snapshot.selection.component_object);
        }
        std::sort(agent_condition_inventory_scope_.begin(),
            agent_condition_inventory_scope_.end());
        agent_condition_inventory_scope_.erase(std::unique(
            agent_condition_inventory_scope_.begin(),
            agent_condition_inventory_scope_.end()),
            agent_condition_inventory_scope_.end());
        if (agent_condition_inventory_scope_.empty()) {
            return core::Result<agent::SceneQueryResult>::failure(invalid_state(
                "select one or more objects before inspecting CONDITION inventory"));
        }
        if (!agent_condition_inventory_query_revision_.has_value() ||
            *agent_condition_inventory_query_revision_ !=
                source_snapshot.project_revision) {
            agent_condition_inventory_query_budget_ = {};
            agent_condition_inventory_query_revision_ =
                source_snapshot.project_revision;
        }
        queried = agent::SceneQuery::execute(session_, {
            agent::ConditionAssetInventoryQuery{std::nullopt}, 512U},
            agent_condition_inventory_query_budget_,
            agent_condition_inventory_scope_, source_snapshot.project_revision);
    }
    if (!queried) {
        record_ui_problem(queried.error().with_context(
            "agent CONDITION inventory query"));
        return core::Result<agent::SceneQueryResult>::failure(queried.error());
    }
    agent_condition_inventory_ = queried.value();
    return queried;
}

core::Result<void> UiController::revise_agent_plan_step(
    agent::AgentPlanStep revised_step) {
    if (!agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no speculative task to revise for repair"));
    }
    const auto task_snapshot = agent_task_->snapshot();
    const bool failed_required_gate = std::any_of(
        task_snapshot.gates.begin(), task_snapshot.gates.end(),
        [](const auto& gate) { return gate.required && !gate.passed; });
    if (task_snapshot.status != agent::TaskStatus::blocked &&
        !(task_snapshot.status == agent::TaskStatus::review && failed_required_gate)) {
        return core::Result<void>::failure(invalid_state(
            "repair requires a blocked task or a review whose required gate failed"));
    }
    if (task_snapshot.repair_attempts_remaining == 0U) {
        return core::Result<void>::failure(invalid_state(
            "the task repair-attempt budget is exhausted"));
    }
    if (auto valid = revised_step.validate(); !valid) return valid;
    const auto existing = std::find_if(agent_plan_draft_.begin(), agent_plan_draft_.end(),
        [&revised_step](const agent::AgentPlanStep& step) {
            return step.id == revised_step.id;
        });
    if (existing == agent_plan_draft_.end()) {
        return core::Result<void>::failure(invalid(
            "repair can revise only an existing plan step"));
    }
    if (existing->target.object != revised_step.target.object) {
        return core::Result<void>::failure(invalid_state(
            "repair cannot expand the target object scope"));
    }
    if (existing->intent == revised_step.intent &&
        existing->target.component == revised_step.target.component &&
        existing->dependencies == revised_step.dependencies) {
        return core::Result<void>::failure(invalid(
            "repair revision must change the step intent, component, or dependencies"));
    }
    *existing = std::move(revised_step);
    agent_repair_mode_ = true;
    const std::uint64_t task_sequence = next_agent_task_id_ ==
        std::numeric_limits<std::uint64_t>::max() ? 1U : next_agent_task_id_;
    agent_plan_id_ = "agent-task-" + std::to_string(task_sequence);
    for (auto& question : agent_clarifications_draft_) {
        question.selected_candidate_id.reset();
        question.freeform_answer.reset();
        question.human_receipt.reset();
    }
    return core::Result<void>::success();
}

core::Result<agent::AgentTaskSnapshot> UiController::repair_agent_plan(
    std::string justification) {
    if (!agent_task_.has_value() || !agent_repair_mode_) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "revise a failed plan step before requesting bounded repair"));
    }
    const auto task_snapshot = agent_task_->snapshot();
    const bool failed_required_gate = std::any_of(
        task_snapshot.gates.begin(), task_snapshot.gates.end(),
        [](const auto& gate) { return gate.required && !gate.passed; });
    if (task_snapshot.status != agent::TaskStatus::blocked &&
        !(task_snapshot.status == agent::TaskStatus::review && failed_required_gate)) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "repair requires a blocked task or a review whose required gate failed"));
    }
    if (!agent_plan_base_revision_.has_value() ||
        session_.snapshot().project_revision != *agent_plan_base_revision_) {
        return core::Result<agent::AgentTaskSnapshot>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "repair cannot use a source project that changed after the failed task"));
    }
    const std::uint64_t task_sequence = next_agent_task_id_ ==
        std::numeric_limits<std::uint64_t>::max() ? 1U : next_agent_task_id_;
    const std::string task_id = "agent-task-" + std::to_string(task_sequence);
    if (agent_plan_id_ != task_id) {
        return core::Result<agent::AgentTaskSnapshot>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "repair plan identity changed while its operator review was open"));
    }
    std::vector<scene::ObjectId> scope;
    for (const auto& step : agent_plan_draft_) {
        if (std::find(scope.begin(), scope.end(), step.target.object) == scope.end()) {
            scope.push_back(step.target.object);
        }
    }
    const auto previous = agent_task_->snapshot();
    agent::AgentPlan revised_plan{
        task_id,
        agent::AuthoringObjective{
            agent_objective_id_, agent_goal_, *agent_plan_base_revision_, scope, {},
            previous.hard_constraints, agent_clarifications_draft_, agent_recipe_id_},
        agent_plan_draft_};
    agent::AgentWorkEnvelope envelope;
    envelope.scope_objects = scope;
    envelope.max_commands = 16U;
    envelope.max_objects_touched = std::max<std::size_t>(scope.size(), 1U);
    auto repaired = agent::AgentTask::repair(
        session_, *agent_task_, std::move(revised_plan), std::move(envelope),
        justification);
    if (!repaired) {
        record_ui_problem(repaired.error().with_context("agent plan repair"));
        return core::Result<agent::AgentTaskSnapshot>::failure(repaired.error());
    }
    agent_task_.reset();
    agent_condition_inventory_.reset();
    agent_task_.emplace(std::move(repaired.value()));
    next_agent_task_id_ = task_sequence == std::numeric_limits<std::uint64_t>::max()
        ? 1U : task_sequence + 1U;
    agent_plan_id_ = task_id;
    agent_repair_mode_ = false;
    auto result = agent_task_->run();
    if (!result) {
        record_ui_problem(result.error().with_context("agent repair execution"));
        return result;
    }
    return result;
}

core::Result<void> UiController::save_agent_checkpoint(
    const std::filesystem::path& path) const {
    if (!agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no agent task to checkpoint"));
    }
    return agent_task_->save_checkpoint(path);
}


core::Result<void> UiController::save_agent_demonstration(
    const std::filesystem::path& path) const {
    if (!agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no agent task to record as a demonstration"));
    }
    const auto demonstration = agent_task_->create_demonstration();
    if (!demonstration) {
        return core::Result<void>::failure(demonstration.error());
    }
    return demonstration.value().save(path);
}

core::Result<void> UiController::load_agent_demonstration(
    const std::filesystem::path& path) {
    auto demonstration = agent::AgentDemonstration::load(path);
    if (!demonstration) {
        record_ui_problem(demonstration.error().with_context("agent demonstration"));
        return core::Result<void>::failure(demonstration.error());
    }
    agent_demonstration_.emplace(std::move(demonstration.value()));
    return core::Result<void>::success();
}

core::Result<void> UiController::clear_agent_demonstration() {
    agent_demonstration_.reset();
    return core::Result<void>::success();
}

core::Result<void> UiController::capture_agent_demonstration_trace(
    application::OperationId first_operation,
    application::OperationId last_operation) {
    if (first_operation == 0U || last_operation < first_operation) {
        return core::Result<void>::failure(invalid_state(
            "demonstration trace range identities are invalid"));
    }
    const auto snapshot = session_.snapshot();
    const auto& available = snapshot.native_operation_trace;
    const auto first = std::find_if(available.begin(), available.end(),
        [first_operation](const auto& operation) {
            return operation.operation_id == first_operation;
        });
    const auto last = std::find_if(available.begin(), available.end(),
        [last_operation](const auto& operation) {
            return operation.operation_id == last_operation;
        });
    if (first == available.end() || last == available.end() || last < first) {
        return core::Result<void>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "selected demonstration trace range is no longer in recent native history"));
    }
    const auto first_index = static_cast<std::size_t>(std::distance(available.begin(), first));
    const auto count = static_cast<std::size_t>(std::distance(first, last)) + 1U;
    const auto selected = std::span<const application::NativeOperationTraceEntry>(
        available.data() + first_index, count);
    auto trace = agent::AgentDemonstrationTrace::capture(selected);
    if (!trace) {
        record_ui_problem(trace.error().with_context("capture native demonstration range"));
        return core::Result<void>::failure(trace.error());
    }
    agent_demonstration_trace_.emplace(std::move(trace.value()));
    agent_demonstration_recipe_name_.clear();
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_.reset();
    agent_demonstration_target_set_count_ = 1U;
    return core::Result<void>::success();
}

core::Result<void> UiController::set_agent_demonstration_target_set_count(
    std::size_t target_set_count) {
    if (!agent_demonstration_trace_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "load or capture a demonstration trace before configuring target sets"));
    }
    if (target_set_count == 0U ||
        target_set_count > kMaxAgentDemonstrationTargetSets) {
        return core::Result<void>::failure(invalid(
            "demonstration target set count must be between 1 and 16"));
    }
    if (agent_demonstration_trace_->steps.size() >
        kMaxExpandedDemonstrationPlanSteps / target_set_count) {
        return core::Result<void>::failure(invalid(
            "expanded demonstration plan exceeds the 256-step limit"));
    }
    if (target_set_count == agent_demonstration_target_set_count_) {
        return core::Result<void>::success();
    }
    if (agent_task_.has_value() || agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "clear the current task or checkpoint before changing demonstration target sets"));
    }
    agent_demonstration_target_set_count_ = target_set_count;
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_.reset();
    return core::Result<void>::success();
}

core::Result<void> UiController::bind_agent_demonstration_target(
    std::uint32_t target_alias,
    std::uint32_t component_alias,
    std::size_t target_set_index) {
    if (!agent_demonstration_trace_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "load or capture a demonstration trace before binding targets"));
    }
    if (auto valid_trace = agent_demonstration_trace_->validate(); !valid_trace) {
        return valid_trace;
    }
    if (target_set_index >= agent_demonstration_target_set_count_) {
        return core::Result<void>::failure(invalid(
            "demonstration target set index is outside the configured selection"));
    }
    const auto snapshot = session_.snapshot();
    if (agent_demonstration_binding_revision_.has_value() &&
        *agent_demonstration_binding_revision_ != snapshot.project_revision) {
        agent_demonstration_bindings_.clear();
    }
    agent_demonstration_binding_revision_ = snapshot.project_revision;

    const auto requirements = demonstration_binding_requirements(
        *agent_demonstration_trace_, agent_demonstration_target_set_count_);
    const auto requirement = std::find_if(requirements.begin(), requirements.end(),
        [target_set_index, target_alias, component_alias](const auto& value) {
            return value.target_set_index == target_set_index &&
                value.target_alias == target_alias &&
                value.component_alias == component_alias;
        });
    if (requirement == requirements.end()) {
        return core::Result<void>::failure(invalid(
            "selected target alias is not required by this demonstration trace"));
    }

    agent::TargetRef target;
    if (requirement->target_kind == agent::AgentDemonstrationTargetKind::object) {
        if (snapshot.selection.mode != editor::SelectionMode::object ||
            snapshot.selection.objects.size() != 1U ||
            snapshot.selection.component_object.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "bind an object trace target by selecting exactly one scene object"));
        }
        target.object = snapshot.selection.objects.front();
        target.component = std::monostate{};
    } else if (requirement->target_kind == agent::AgentDemonstrationTargetKind::face) {
        if (snapshot.selection.mode != editor::SelectionMode::face ||
            !snapshot.selection.component_object.has_value() ||
            snapshot.selection.faces.size() != 1U ||
            !snapshot.selection.vertices.empty()) {
            return core::Result<void>::failure(invalid_state(
                "bind a face trace target by selecting exactly one face"));
        }
        target.object = *snapshot.selection.component_object;
        target.component = snapshot.selection.faces.front();
        const auto* instance = snapshot.viewport.scene.find(target.object);
        if (instance == nullptr || instance->mesh == nullptr ||
            std::find(instance->mesh->triangle_faces.begin(),
                instance->mesh->triangle_faces.end(), snapshot.selection.faces.front()) ==
                instance->mesh->triangle_faces.end()) {
            return core::Result<void>::failure(stale(
                "selected demonstration face is absent from the current viewport snapshot"));
        }
    } else if (requirement->target_kind == agent::AgentDemonstrationTargetKind::vertex) {
        if (snapshot.selection.mode != editor::SelectionMode::vertex ||
            !snapshot.selection.component_object.has_value() ||
            snapshot.selection.vertices.size() != 1U ||
            !snapshot.selection.faces.empty()) {
            return core::Result<void>::failure(invalid_state(
                "bind a vertex trace target by selecting exactly one vertex"));
        }
        target.object = *snapshot.selection.component_object;
        target.component = snapshot.selection.vertices.front();
        const auto* instance = snapshot.viewport.scene.find(target.object);
        if (instance == nullptr || instance->mesh == nullptr ||
            std::find(instance->mesh->vertex_ids.begin(), instance->mesh->vertex_ids.end(),
                snapshot.selection.vertices.front()) == instance->mesh->vertex_ids.end()) {
            return core::Result<void>::failure(stale(
                "selected demonstration vertex is absent from the current viewport snapshot"));
        }
    } else {
        return core::Result<void>::failure(invalid(
            "demonstration target kind is unknown"));
    }

    for (const auto& existing : agent_demonstration_bindings_) {
        if (existing.target_set_index == target_set_index) {
            if (existing.target_alias == target_alias &&
                existing.target.object != target.object) {
                return core::Result<void>::failure(invalid_state(
                    "all components for one demonstration object alias must map to the same object within a target set"));
            }
            if (existing.target_alias != target_alias &&
                existing.target.object == target.object) {
                return core::Result<void>::failure(invalid_state(
                    "different demonstration object aliases must map to different scene objects within a target set"));
            }
        } else if (existing.target.object == target.object) {
            return core::Result<void>::failure(invalid_state(
                "one scene object cannot be reused across demonstration target sets"));
        }
    }

    const auto existing = std::find_if(
        agent_demonstration_bindings_.begin(), agent_demonstration_bindings_.end(),
        [target_set_index, target_alias, component_alias](const auto& value) {
            return value.target_set_index == target_set_index &&
                value.target_alias == target_alias &&
                value.component_alias == component_alias;
        });
    if (existing == agent_demonstration_bindings_.end()) {
        agent_demonstration_bindings_.push_back({
            target_set_index, target_alias, component_alias, target});
    } else {
        existing->target = target;
    }
    return core::Result<void>::success();
}

core::Result<std::size_t> UiController::apply_agent_demonstration_to_similar() {
    if (!agent_demonstration_trace_.has_value()) {
        return core::Result<std::size_t>::failure(invalid_state(
            "load or capture a demonstration trace before finding similar targets"));
    }
    if (agent_task_.has_value() || agent_checkpoint_pending_.has_value()) {
        return core::Result<std::size_t>::failure(invalid_state(
            "clear the current task or checkpoint before finding similar targets"));
    }
    if (auto valid_trace = agent_demonstration_trace_->validate(); !valid_trace) {
        return core::Result<std::size_t>::failure(valid_trace.error());
    }
    const auto& trace_step = agent_demonstration_trace_->steps.front();
    if (!trace_step.structure_signature.has_value()) {
        return core::Result<std::size_t>::failure(unsupported(
            "this legacy or unsupported demonstration has no structural correspondence evidence"));
    }
    if (!trace_step.result_object_structure_signature.has_value()) {
        return core::Result<std::size_t>::failure(unsupported(
            "this demonstration has no post-command exemplar evidence"));
    }
    if (!supports_demonstration_structural_matching(*agent_demonstration_trace_)) {
        return core::Result<std::size_t>::failure(unsupported(
            "multi-command correspondence requires a single alias chain of unchanged transforms or topology-stable edits to one vertex"));
    }

    const auto snapshot = session_.snapshot();
    if (snapshot.selection.mode != editor::SelectionMode::object ||
        snapshot.selection.objects.size() != 1U ||
        snapshot.selection.component_object.has_value()) {
        return core::Result<std::size_t>::failure(invalid_state(
            "select exactly one completed demonstration object as the exemplar"));
    }
    const scene::ObjectId exemplar_object = snapshot.selection.objects.front();
    const auto exemplar_view = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(), [exemplar_object](const auto& value) {
            return value.object.id == exemplar_object;
        });
    if (exemplar_view == snapshot.objects.end() || !exemplar_view->object.visible ||
        exemplar_view->object.locked ||
        (trace_step.target_kind == agent::AgentDemonstrationTargetKind::object &&
         exemplar_view->object.parent.has_value())) {
        return core::Result<std::size_t>::failure(invalid_state(
            "the exemplar must be a visible, unlocked target of the recorded kind"));
    }

    const auto exemplar_signature = session_.native_structure_signature(
        exemplar_object, std::monostate{});
    if (!exemplar_signature ||
        exemplar_signature.value() != *trace_step.result_object_structure_signature) {
        return core::Result<std::size_t>::failure(stale(
            "selected object no longer matches the captured post-command exemplar evidence"));
    }

    const auto target_kind = trace_step.target_kind ==
            agent::AgentDemonstrationTargetKind::object
        ? application::NativeTraceTargetKind::object
        : (trace_step.target_kind == agent::AgentDemonstrationTargetKind::face
            ? application::NativeTraceTargetKind::face
            : application::NativeTraceTargetKind::vertex);
    const auto matches = session_.find_native_structure_matches(
        target_kind, *trace_step.structure_signature, exemplar_object);
    if (!matches) {
        return core::Result<std::size_t>::failure(matches.error());
    }

    std::map<scene::ObjectId, std::optional<
        std::variant<std::monostate, geometry::FaceId, geometry::VertexId>>> candidates;
    std::set<scene::ObjectId> ambiguous_objects;
    for (const auto& match : matches.value()) {
        if (ambiguous_objects.contains(match.object)) continue;
        const auto [candidate, inserted] = candidates.emplace(
            match.object, std::optional{match.component});
        if (!inserted) {
            candidates.erase(candidate);
            ambiguous_objects.insert(match.object);
        }
    }
    if (trace_step.target_kind != agent::AgentDemonstrationTargetKind::object) {
        if (!exemplar_view->object.mesh_asset.has_value()) {
            return core::Result<std::size_t>::failure(stale(
                "selected exemplar has no current mesh asset"));
        }
        std::map<std::uint64_t, std::size_t> asset_owners;
        for (const auto& object : snapshot.objects) {
            if (object.object.mesh_asset.has_value()) {
                ++asset_owners[*object.object.mesh_asset];
            }
        }
        for (const auto& [object, component] : candidates) {
            static_cast<void>(component);
            const auto candidate_view = std::find_if(
                snapshot.objects.begin(), snapshot.objects.end(), [&object](const auto& value) {
                    return value.object.id == object;
                });
            if (candidate_view == snapshot.objects.end() ||
                !candidate_view->object.mesh_asset.has_value()) {
                return core::Result<std::size_t>::failure(stale(
                    "structural match is no longer an authored mesh object"));
            }
        }
        for (auto candidate = candidates.begin(); candidate != candidates.end();) {
            const auto candidate_view = std::find_if(
                snapshot.objects.begin(), snapshot.objects.end(), [&candidate](const auto& value) {
                    return value.object.id == candidate->first;
                });
            const auto candidate_asset = *candidate_view->object.mesh_asset;
            if (candidate_asset == *exemplar_view->object.mesh_asset ||
                asset_owners[candidate_asset] > 1U) {
                candidate = candidates.erase(candidate);
            } else {
                ++candidate;
            }
        }
    }
    if (candidates.empty()) {
        return core::Result<std::size_t>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "no unique matching structural signatures were found for the selected exemplar"));
    }
    if (candidates.size() > kMaxAgentDemonstrationTargetSets) {
        return core::Result<std::size_t>::failure(unsupported(
            "more than 16 matching structural signatures were found; narrow the target scope"));
    }

    agent_demonstration_target_set_count_ = candidates.size();
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_ = snapshot.project_revision;
    std::size_t target_set_index = 0U;
    for (const auto& [object, component] : candidates) {
        if (!component.has_value()) {
            return core::Result<std::size_t>::failure(invalid_state(
                "structural match did not resolve a unique target component"));
        }
        agent::TargetRef target;
        target.object = object;
        if (std::holds_alternative<geometry::FaceId>(*component)) {
            target.component = std::get<geometry::FaceId>(*component);
        } else if (std::holds_alternative<geometry::VertexId>(*component)) {
            target.component = std::get<geometry::VertexId>(*component);
        } else {
            target.component = std::monostate{};
        }
        agent_demonstration_bindings_.push_back({
            target_set_index++, trace_step.target_alias,
            trace_step.component_alias, target});
    }
    return core::Result<std::size_t>::success(candidates.size());
}

core::Result<void> UiController::clear_agent_demonstration_target_bindings() {
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_.reset();
    return core::Result<void>::success();
}

core::Result<agent::AgentTaskSnapshot> UiController::preview_agent_demonstration_trace() {
    if (!agent_demonstration_trace_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "load or capture a demonstration trace before previewing it"));
    }
    if (agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "clear the current AgentTask before previewing a demonstration trace"));
    }
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "resume or clear the loaded checkpoint before previewing a demonstration trace"));
    }
    if (auto valid_trace = agent_demonstration_trace_->validate(); !valid_trace) {
        return core::Result<agent::AgentTaskSnapshot>::failure(valid_trace.error());
    }
    const auto snapshot = session_.snapshot();
    if (!agent_demonstration_binding_revision_.has_value() ||
        *agent_demonstration_binding_revision_ != snapshot.project_revision) {
        agent_demonstration_bindings_.clear();
        agent_demonstration_binding_revision_.reset();
        return core::Result<agent::AgentTaskSnapshot>::failure(stale(
            "project changed after target binding; bind the demonstration targets again"));
    }

    if (agent_demonstration_target_set_count_ == 0U ||
        agent_demonstration_target_set_count_ > kMaxAgentDemonstrationTargetSets ||
        agent_demonstration_trace_->steps.size() >
            kMaxExpandedDemonstrationPlanSteps / agent_demonstration_target_set_count_) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid(
            "expanded demonstration target selection exceeds the supported bound"));
    }
    const auto requirements = demonstration_binding_requirements(
        *agent_demonstration_trace_, agent_demonstration_target_set_count_);
    const std::size_t expanded_step_count =
        agent_demonstration_trace_->steps.size() * agent_demonstration_target_set_count_;
    std::vector<scene::ObjectId> scope;
    std::vector<std::vector<scene::ObjectId>> target_set_scopes(
        agent_demonstration_target_set_count_);
    std::vector<std::vector<scene::ObjectId>> transform_roots(
        agent_demonstration_target_set_count_);
    std::vector<agent::AgentPlanStep> steps;
    steps.reserve(expanded_step_count);
    std::map<std::uint64_t, std::pair<std::size_t, scene::ObjectId>> changed_mesh_assets;
    std::map<std::pair<scene::ObjectId, geometry::VertexId>, core::Vec3d>
        planned_vertex_positions;
    const bool sequential_vertex_trace = agent_demonstration_trace_->steps.size() > 1U &&
        agent_demonstration_trace_->steps.front().target_kind ==
            agent::AgentDemonstrationTargetKind::vertex &&
        supports_demonstration_structural_matching(*agent_demonstration_trace_);
    std::uint32_t permitted_effects = 0U;
    double transform_distance_budget = 0.0;
    bool has_topology_operation = false;

    for (const auto& requirement : requirements) {
        const auto binding = std::find_if(
            agent_demonstration_bindings_.begin(), agent_demonstration_bindings_.end(),
            [&requirement](const auto& value) {
                return value.target_set_index == requirement.target_set_index &&
                    value.target_alias == requirement.target_alias &&
                    value.component_alias == requirement.component_alias;
            });
        if (binding == agent_demonstration_bindings_.end()) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "bind every object and component alias before previewing the trace"));
        }
        auto& target_scope = target_set_scopes[requirement.target_set_index];
        if (std::find(target_scope.begin(), target_scope.end(), binding->target.object) ==
            target_scope.end()) {
            target_scope.push_back(binding->target.object);
        }
        if (requirement.target_kind == agent::AgentDemonstrationTargetKind::object &&
            std::find(transform_roots[requirement.target_set_index].begin(),
                transform_roots[requirement.target_set_index].end(),
                binding->target.object) ==
                transform_roots[requirement.target_set_index].end()) {
            transform_roots[requirement.target_set_index].push_back(binding->target.object);
        }
    }

    for (std::size_t target_set_index = 0U;
         target_set_index < agent_demonstration_target_set_count_; ++target_set_index) {
        auto& target_scope = target_set_scopes[target_set_index];
        auto& roots = transform_roots[target_set_index];
        for (std::size_t cursor = 0U; cursor < roots.size(); ++cursor) {
            const scene::ObjectId parent = roots[cursor];
            const auto parent_view = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                [parent](const auto& value) { return value.object.id == parent; });
            if (parent_view == snapshot.objects.end() || parent_view->object.parent.has_value()) {
                return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                    "object demonstration targets must bind to existing root objects"));
            }
            for (const auto& object : snapshot.objects) {
                if (!object.object.parent.has_value() || *object.object.parent != parent) continue;
                if (std::find(target_scope.begin(), target_scope.end(), object.object.id) ==
                    target_scope.end()) {
                    target_scope.push_back(object.object.id);
                    roots.push_back(object.object.id);
                }
            }
        }
        for (std::size_t prior_set = 0U; prior_set < target_set_index; ++prior_set) {
            for (const auto object : target_scope) {
                if (std::find(target_set_scopes[prior_set].begin(),
                        target_set_scopes[prior_set].end(), object) !=
                    target_set_scopes[prior_set].end()) {
                    return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                        "demonstration target sets cannot overlap through object hierarchy"));
                }
            }
        }
        for (const auto object : target_scope) {
            if (std::find(scope.begin(), scope.end(), object) == scope.end()) {
                scope.push_back(object);
            }
        }
    }

    for (std::size_t target_set_index = 0U;
         target_set_index < agent_demonstration_target_set_count_; ++target_set_index) {
      for (std::size_t index = 0U; index < agent_demonstration_trace_->steps.size(); ++index) {
        const auto& trace_step = agent_demonstration_trace_->steps[index];
        const auto binding = std::find_if(
            agent_demonstration_bindings_.begin(), agent_demonstration_bindings_.end(),
            [target_set_index, &trace_step](const auto& value) {
                return value.target_set_index == target_set_index &&
                    value.target_alias == trace_step.target_alias &&
                    value.component_alias == trace_step.component_alias;
            });
        if (binding == agent_demonstration_bindings_.end()) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "demonstration trace target binding is missing"));
        }

        if (trace_step.target_kind == agent::AgentDemonstrationTargetKind::object) {
            permitted_effects |= agent::effect_mask(agent::Effect::transform);
            transform_distance_budget +=
                trace_step.parameters.relative_displacement.length();
        } else {
            const auto object = std::find_if(snapshot.objects.begin(), snapshot.objects.end(),
                [&binding](const auto& value) {
                    return value.object.id == binding->target.object;
                });
            if (object == snapshot.objects.end() || !object->object.mesh_asset.has_value()) {
                return core::Result<agent::AgentTaskSnapshot>::failure(stale(
                    "bound mesh target has no current authored mesh asset"));
            }
            const auto [asset_entry, inserted] = changed_mesh_assets.emplace(
                *object->object.mesh_asset,
                std::pair{target_set_index, binding->target.object});
            if (!inserted && (!sequential_vertex_trace ||
                    asset_entry->second != std::pair{target_set_index, binding->target.object})) {
                return core::Result<agent::AgentTaskSnapshot>::failure(unsupported(
                    "trace preview requires separate mesh assets across targets; repeated edits are supported only for one topology-stable vertex target"));
            }
            if (trace_step.target_kind == agent::AgentDemonstrationTargetKind::face) {
                permitted_effects |= agent::effect_mask(agent::Effect::geometry) |
                    agent::effect_mask(agent::Effect::topology);
                has_topology_operation = true;
            } else {
                permitted_effects |= agent::effect_mask(agent::Effect::geometry);
            }
        }

        std::optional<core::Vec3d> vertex_base_position;
        const auto* vertex_id = std::get_if<geometry::VertexId>(&binding->target.component);
        if (vertex_id != nullptr) {
            const auto key = std::pair{binding->target.object, *vertex_id};
            const auto planned_position = planned_vertex_positions.find(key);
            if (planned_position != planned_vertex_positions.end()) {
                vertex_base_position = planned_position->second;
            } else {
                const auto* instance = snapshot.viewport.scene.find(binding->target.object);
                if (instance == nullptr || instance->mesh == nullptr) {
                    return core::Result<agent::AgentTaskSnapshot>::failure(stale(
                        "bound demonstration vertex has no current source mesh"));
                }
                const auto vertex = std::find(instance->mesh->vertex_ids.begin(),
                    instance->mesh->vertex_ids.end(), *vertex_id);
                if (vertex == instance->mesh->vertex_ids.end()) {
                    return core::Result<agent::AgentTaskSnapshot>::failure(stale(
                        "bound demonstration vertex is absent from the source mesh"));
                }
                const auto vertex_index = static_cast<std::size_t>(std::distance(
                    instance->mesh->vertex_ids.begin(), vertex));
                vertex_base_position = instance->mesh->positions[vertex_index];
            }
        }
        auto intent = demonstration_step_intent(
            trace_step, binding->target, snapshot, vertex_base_position);
        if (!intent) {
            return core::Result<agent::AgentTaskSnapshot>::failure(intent.error());
        }
        if (vertex_id != nullptr && vertex_base_position.has_value()) {
            planned_vertex_positions[{binding->target.object, *vertex_id}] =
                *vertex_base_position + trace_step.parameters.relative_displacement;
        }
        std::vector<std::string> dependencies;
        const std::string step_id = "demonstration-set-" +
            std::to_string(target_set_index + 1U) + "-step-" +
            std::to_string(index + 1U);
        if (index > 0U) {
            dependencies.push_back("demonstration-set-" +
                std::to_string(target_set_index + 1U) + "-step-" +
                std::to_string(index));
        }
        steps.push_back(agent::AgentPlanStep{
            step_id,
            binding->target,
            std::move(intent.value()),
            std::move(dependencies)});
      }
    }

    if (!std::isfinite(transform_distance_budget) ||
        transform_distance_budget > 1'000'000.0) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid(
            "demonstration transform budget exceeds the supported limit"));
    }
    const std::uint64_t task_sequence = next_agent_task_id_ ==
        std::numeric_limits<std::uint64_t>::max() ? 1U : next_agent_task_id_;
    const std::string task_id = "agent-demonstration-" + std::to_string(task_sequence);
    constexpr std::string_view goal =
        "Preview a captured demonstration on explicitly bound targets";
    agent::AgentPlan plan{
        task_id,
        agent::AuthoringObjective{
            task_id, std::string(goal), snapshot.project_revision, scope, {}, {}, {}, {}},
        std::move(steps)};
    agent::AgentWorkEnvelope envelope;
    envelope.scope_objects = scope;
    envelope.permitted_effects = permitted_effects;
    envelope.max_commands = expanded_step_count;
    envelope.max_objects_touched = scope.size();
    envelope.max_topology_operations = has_topology_operation
        ? 512U * agent_demonstration_target_set_count_ : 0U;
    envelope.max_repair_attempts = 0U;
    envelope.max_additional_operations = 0U;
    envelope.max_transform_distance_meters = transform_distance_budget;

    auto plan_draft = plan.steps;
    auto task = agent::AgentTask::begin(session_, std::move(plan), std::move(envelope));
    if (!task) {
        record_ui_problem(task.error().with_context("demonstration speculative preview"));
        return core::Result<agent::AgentTaskSnapshot>::failure(task.error());
    }
    next_agent_task_id_ = task_sequence == std::numeric_limits<std::uint64_t>::max()
        ? 1U : task_sequence + 1U;
    agent_goal_ = std::string(goal);
    agent_plan_id_ = task_id;
    agent_objective_id_ = task_id;
    agent_plan_base_revision_ = snapshot.project_revision;
    agent_plan_draft_ = std::move(plan_draft);
    agent_hard_constraints_draft_.clear();
    agent_clarifications_draft_.clear();
    agent_recipe_id_.clear();
    agent_repair_mode_ = false;
    agent_condition_inventory_.reset();
    agent_task_.emplace(std::move(task.value()));
    return run_prepared_agent_plan();
}

core::Result<void> UiController::save_agent_demonstration_trace(
    const std::filesystem::path& path) const {
    if (!agent_demonstration_trace_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no captured native command range to save"));
    }
    return agent_demonstration_trace_->save(path);
}

core::Result<void> UiController::load_agent_demonstration_trace(
    const std::filesystem::path& path) {
    auto trace = agent::AgentDemonstrationTrace::load(path);
    if (!trace) {
        record_ui_problem(trace.error().with_context("agent demonstration trace"));
        return core::Result<void>::failure(trace.error());
    }
    agent_demonstration_trace_.emplace(std::move(trace.value()));
    agent_demonstration_recipe_name_.clear();
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_.reset();
    agent_demonstration_target_set_count_ = 1U;
    return core::Result<void>::success();
}

core::Result<void> UiController::save_agent_demonstration_recipe(
    const std::filesystem::path& path) {
    if (!agent_demonstration_trace_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no captured native command range to save as a recipe"));
    }
    const auto utf8_name = path.stem().u8string();
    std::string name;
    name.reserve(utf8_name.size());
    for (const char8_t character : utf8_name) {
        name.push_back(static_cast<char>(character));
    }
    agent::AgentDemonstrationRecipe recipe{
        std::move(name), *agent_demonstration_trace_};
    auto saved = recipe.save(path);
    if (saved) agent_demonstration_recipe_name_ = recipe.name;
    return saved;
}

core::Result<void> UiController::load_agent_demonstration_recipe(
    const std::filesystem::path& path) {
    auto recipe = agent::AgentDemonstrationRecipe::load(path);
    if (!recipe) {
        record_ui_problem(recipe.error().with_context("agent demonstration recipe"));
        return core::Result<void>::failure(recipe.error());
    }
    agent_demonstration_recipe_name_ = recipe.value().name;
    agent_demonstration_trace_.emplace(std::move(recipe.value().trace));
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_.reset();
    agent_demonstration_target_set_count_ = 1U;
    return core::Result<void>::success();
}

core::Result<void> UiController::clear_agent_demonstration_trace() {
    agent_demonstration_trace_.reset();
    agent_demonstration_recipe_name_.clear();
    agent_demonstration_bindings_.clear();
    agent_demonstration_binding_revision_.reset();
    agent_demonstration_target_set_count_ = 1U;
    return core::Result<void>::success();
}

core::Result<void> UiController::load_agent_checkpoint(
    const std::filesystem::path& path) {
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "clear the currently loaded checkpoint before opening another"));
    }
    if (agent_task_.has_value()) {
        const auto status = agent_task_->snapshot().status;
        if (status != agent::TaskStatus::accepted && status != agent::TaskStatus::rejected) {
            return core::Result<void>::failure(invalid_state(
                "accept, reject, or clear the active speculative task before opening a checkpoint"));
        }
    }
    auto checkpoint = agent::AgentTask::load_checkpoint(path);
    if (!checkpoint) {
        record_ui_problem(checkpoint.error().with_context("agent task checkpoint"));
        return core::Result<void>::failure(checkpoint.error());
    }
    if (checkpoint.value().status == agent::TaskStatus::accepted ||
        checkpoint.value().status == agent::TaskStatus::rejected) {
        return core::Result<void>::failure(invalid_state(
            "accepted and rejected checkpoints are history records and cannot resume"));
    }
    agent_task_.reset();
    agent_condition_inventory_.reset();
    agent_plan_draft_ = checkpoint.value().plan.steps;
    agent_hard_constraints_draft_ = checkpoint.value().plan.objective.hard_constraints;
    agent_clarifications_draft_ = checkpoint.value().plan.objective.clarifications;
    agent_plan_base_revision_ = checkpoint.value().source_revision;
    agent_goal_ = checkpoint.value().plan.objective.goal;
    agent_recipe_id_ = checkpoint.value().plan.objective.recipe_id;
    agent_plan_id_ = checkpoint.value().plan.id;
    agent_objective_id_ = checkpoint.value().plan.objective.id;
    agent_repair_mode_ = false;
    agent_checkpoint_rebased_ = false;
    agent_checkpoint_pending_.emplace(std::move(checkpoint.value()));
    return core::Result<void>::success();
}

core::Result<void> UiController::rebase_agent_checkpoint() {
    if (!agent_checkpoint_pending_.has_value() || agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "load one saved checkpoint before requesting a rebase"));
    }
    const auto analysis = agent::AgentTask::analyze_checkpoint_rebase(
        session_, *agent_checkpoint_pending_);
    if (!analysis) {
        record_ui_problem(analysis.error().with_context("agent task rebase analysis"));
        return core::Result<void>::failure(analysis.error());
    }
    if (!analysis.value().can_rebase()) {
        std::string message = "checkpoint rebase is blocked";
        for (std::size_t index = 0U;
             index < analysis.value().blockers.size() && index < 3U; ++index) {
            message += index == 0U ? ": " : "; ";
            message += analysis.value().blockers[index];
        }
        const auto diagnostic = core::Diagnostic(
            core::ErrorCode::stale_data, std::move(message));
        record_ui_problem(diagnostic.with_context("agent task rebase analysis"));
        return core::Result<void>::failure(diagnostic);
    }
    if (analysis.value().current_revision ==
            analysis.value().checkpoint_revision &&
        analysis.value().current_source_digest == agent_checkpoint_pending_->source_digest) {
        agent_checkpoint_rebased_ = false;
        agent_plan_base_revision_ = analysis.value().current_revision;
        return core::Result<void>::success();
    }
    if (!agent_plan_base_revision_.has_value() ||
        *agent_plan_base_revision_ != analysis.value().current_revision) {
        for (auto& question : agent_clarifications_draft_) {
            question.selected_candidate_id.reset();
            question.freeform_answer.reset();
            question.human_receipt.reset();
        }
    }
    agent_checkpoint_rebased_ = true;
    agent_plan_base_revision_ = analysis.value().current_revision;
    return core::Result<void>::success();
}

core::Result<agent::AgentTaskSnapshot> UiController::resume_agent_checkpoint() {
    if (!agent_checkpoint_pending_.has_value() || agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "load one saved checkpoint before requesting resume"));
    }
    auto plan = agent_checkpoint_pending_->plan;
    plan.steps = agent_plan_draft_;
    plan.objective.goal = agent_goal_;
    plan.objective.hard_constraints = agent_hard_constraints_draft_;
    plan.objective.clarifications = agent_clarifications_draft_;
    if (agent_checkpoint_rebased_ && agent_plan_base_revision_.has_value()) {
        plan.objective.base_revision = *agent_plan_base_revision_;
    }
    auto resumed = agent_checkpoint_rebased_
        ? agent::AgentTask::rebase_checkpoint(
            session_, *agent_checkpoint_pending_, std::move(plan))
        : agent::AgentTask::resume_checkpoint(
            session_, *agent_checkpoint_pending_, std::move(plan));
    if (!resumed) {
        record_ui_problem(resumed.error().with_context("agent task checkpoint resume"));
        return core::Result<agent::AgentTaskSnapshot>::failure(resumed.error());
    }
    agent_condition_inventory_.reset();
    agent_task_.emplace(std::move(resumed.value()));
    agent_checkpoint_pending_.reset();
    agent_checkpoint_rebased_ = false;
    agent_repair_mode_ = false;
    const auto result = agent_task_->snapshot();
    return core::Result<agent::AgentTaskSnapshot>::success(result);
}

core::Result<agent::AgentTaskSnapshot> UiController::correct_agent_ghost_transform(
    scene::ObjectId object,
    core::Transform transform,
    std::string justification) {
    if (!agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "there is no speculative ghost to correct"));
    }
    const auto corrected = application::HumanApplicationAccess::correct_agent_ghost_transform(
        *agent_task_, object, transform, std::move(justification));
    if (!corrected) {
        record_ui_problem(corrected.error().with_context("agent ghost correction"));
        return core::Result<agent::AgentTaskSnapshot>::failure(corrected.error());
    }
    return core::Result<agent::AgentTaskSnapshot>::success(agent_task_->snapshot());
}

core::Result<agent::AgentTaskSnapshot> UiController::correct_agent_ghost_vertex_position(
    scene::ObjectId object,
    geometry::VertexId vertex,
    core::Vec3d position,
    std::string justification) {
    if (!agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "there is no speculative ghost to correct"));
    }
    const auto corrected =
        application::HumanApplicationAccess::correct_agent_ghost_vertex_position(
            *agent_task_, object, vertex, position, std::move(justification));
    if (!corrected) {
        record_ui_problem(corrected.error().with_context("agent ghost vertex correction"));
        return core::Result<agent::AgentTaskSnapshot>::failure(corrected.error());
    }
    return core::Result<agent::AgentTaskSnapshot>::success(agent_task_->snapshot());
}

core::Result<agent::AgentTaskSnapshot> UiController::correct_agent_ghost_face(
    scene::ObjectId object,
    geometry::FaceId face,
    agent::GhostCorrectionKind kind,
    double distance,
    std::string justification) {
    if (!agent_task_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "there is no speculative ghost to correct"));
    }
    const auto corrected = application::HumanApplicationAccess::correct_agent_ghost_face(
        *agent_task_, object, face, kind, distance, std::move(justification));
    if (!corrected) {
        record_ui_problem(corrected.error().with_context("agent ghost face correction"));
        return core::Result<agent::AgentTaskSnapshot>::failure(corrected.error());
    }
    return core::Result<agent::AgentTaskSnapshot>::success(agent_task_->snapshot());
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
        agent_condition_inventory_.reset();
    }
    agent_condition_inventory_.reset();
    agent_plan_draft_.clear();
    agent_hard_constraints_draft_.clear();
    agent_clarifications_draft_.clear();
    agent_checkpoint_pending_.reset();
    agent_checkpoint_rebased_ = false;
    agent_plan_base_revision_.reset();
    agent_goal_.clear();
    agent_plan_id_.clear();
    agent_objective_id_.clear();
    agent_recipe_id_.clear();
    agent_repair_mode_ = false;
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

    const auto application_snapshot = session_.snapshot();
    std::optional<PendingMeshSource> pending_source;
    std::filesystem::path importer_path = path;
    if (application_snapshot.project_path.has_value()) {
        auto staged = stage_mesh_source(
            path, *application_snapshot.project_path);
        if (!staged) return fail(staged.error());
        importer_path = staged.value().staged_path;
        pending_source = std::move(staged.value());
    }
    const auto cleanup_pending_source = [&pending_source]() {
        if (pending_source.has_value()) remove_pending_mesh_source(*pending_source);
    };

    geometry::EditableMesh mesh;
    io::IoReport report;
    switch (format) {
    case MeshImportFormat::obj: {
        auto imported = io::import_obj(importer_path);
        if (!imported) {
            cleanup_pending_source();
            return fail(imported.error());
        }
        mesh = std::move(imported.value().mesh);
        report = std::move(imported.value().report);
        break;
    }
    case MeshImportFormat::ply: {
        auto imported = io::import_ply(importer_path);
        if (!imported) {
            cleanup_pending_source();
            return fail(imported.error());
        }
        mesh = std::move(imported.value().mesh);
        report = std::move(imported.value().report);
        break;
    }
    case MeshImportFormat::stl: {
        auto imported = io::import_stl(importer_path);
        if (!imported) {
            cleanup_pending_source();
            return fail(imported.error());
        }
        mesh = std::move(imported.value().mesh);
        report = std::move(imported.value().report);
        break;
    }
    default:
        cleanup_pending_source();
        return fail(unsupported("mesh import format is unsupported"));
    }

    std::optional<StagedMeshSource> staged_source;
    if (pending_source.has_value()) {
        auto persisted = persist_mesh_source(*pending_source, format);
        cleanup_pending_source();
        if (!persisted) return fail(persisted.error());
        staged_source = std::move(persisted.value());
    }

    std::string object_name = path.stem().string();
    if (object_name.empty()) object_name = "Imported Mesh";
    std::optional<assets::SourceImportRecord> source_record;
    if (staged_source.has_value()) {
        assets::SourceImportRecord record;
        record.identity = pending_source->identity;
        record.relative_source_path = staged_source->relative_path;
        record.source_generation = core::Revision{1U};
        record.kind = assets::SourceAssetKind::mesh;
        record.source_digest = staged_source->digest;
        switch (format) {
        case MeshImportFormat::obj: record.importer_identity = "carto.import.obj"; break;
        case MeshImportFormat::ply: record.importer_identity = "carto.import.ply"; break;
        case MeshImportFormat::stl: record.importer_identity = "carto.import.stl"; break;
        }
        record.importer_version = 1U;
        record.options.import_hierarchy = false;
        record.options.import_materials = false;
        record.options.import_normals = false;
        record.artifact_identity = "pending.mesh";
        record.artifact_digest = application::canonical_mesh_geometry_digest(mesh);
        record.domain_digests.push_back(assets::SourceDomainDigest{
            assets::SourceAssetDomain::geometry, record.artifact_digest});
        const auto active = std::find_if(
            application_snapshot.source_assets.begin(),
            application_snapshot.source_assets.end(),
            [&record](const application::SourceAssetView& item) {
                return item.identity == record.identity;
            });
        if (active != application_snapshot.source_assets.end()) {
            record.artifact_identity = active->artifact_identity;
        }
        if (auto result = record.validate(); !result) {
            return fail(result.error());
        }
        report.warnings.push_back(
            "Source bytes were verified and stored as a project-relative content-addressed snapshot.");
        source_record = std::move(record);
    } else {
        report.warnings.push_back(
            "This import is not source-linked because the workspace has not been saved; "
            "save the project and import again to bind it.");
    }

    auto admitted = dispatch(application::CreateMeshObjectAction{
        object_name, std::move(mesh), std::move(source_record)});
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
        staged_source.has_value(),
        staged_source.has_value() ? pending_source->identity : std::string{},
        staged_source.has_value() ? staged_source->relative_path : std::string{},
    });
}

core::Result<assets::SourceReimportPlan> UiController::preview_source_reimport(
    std::vector<assets::SourceImportRecord> incoming) const {
    return session_.preview_source_reimport(std::move(incoming));
}

core::Result<application::DispatchReceipt> UiController::apply_source_reimport_plan(
    assets::SourceReimportPlan plan) {
    return dispatch(application::ApplySourceReimportPlanAction{std::move(plan)});
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

core::Result<application::DispatchReceipt> UiController::import_condition_audio_clip(
    const std::filesystem::path& path) {
    auto bytes = read_bounded_file(path, condition::kMaxWaveClipBytes, "WAVE audio clip");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    const std::string& encoded = bytes.value();
    const auto byte_span = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()};
    const auto inspection = condition::inspect_wave_clip(byte_span);
    if (!inspection) return core::Result<application::DispatchReceipt>::failure(
        inspection.error().with_context("WAVE audio clip inspection"));

    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION audio clip import"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (authoring.value().sources.size() >=
        condition::AuthoringDocument::kMaxRecordsPerKind) {
        return core::Result<application::DispatchReceipt>::failure(validation(
            "CONDITION source evidence count exceeds its limit"));
    }
    const auto maximum = std::max_element(authoring.value().sources.begin(),
        authoring.value().sources.end(), [](const auto& left, const auto& right) {
            return left.id < right.id;
        });
    if (maximum != authoring.value().sources.end() &&
        maximum->id == std::numeric_limits<condition::Id>::max()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION source identity space is exhausted"));
    }

    const assets::Sha256Digest digest = assets::sha256(byte_span);
    constexpr std::string_view import_settings =
        "Cartographer.WaveImport/v1;RIFF-WAVE;PCM8-16-24-32-or-IEEE-F32-F64;metadata-only";
    const auto settings_bytes = std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(import_settings.data()), import_settings.size()};
    condition::SourceEvidence source;
    source.id = maximum == authoring.value().sources.end() ? 1U : maximum->id + 1U;
    source.kind = condition::SourceKind::imported_asset;
    source.label = "WAVE clip " + digest.hex().substr(0U, 12U);
    source.logical_locator = "audio/sha256/" + digest.hex() + ".wav";
    source.revision = "sha256:" + digest.hex();
    source.digest = digest;
    source.producer_identity = "Cartographer.WaveImport";
    source.producer_version = "1";
    source.import_settings_digest = assets::sha256(settings_bytes);
    source.reviewed = false;
    source.current = true;
    source.diagnostics.push_back("WAVE inspected: " +
        std::string(inspection.value().encoding_name()) + ", " +
        std::to_string(inspection.value().channels) + " channels, " +
        std::to_string(inspection.value().sample_rate) + " Hz, " +
        std::to_string(inspection.value().bits_per_sample) + " bit, " +
        std::to_string(inspection.value().frame_count) + " frames");
    authoring.value().sources.push_back(std::move(source));
    std::sort(authoring.value().sources.begin(), authoring.value().sources.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::bind_condition_asset_to_object(
    condition::Id asset_id,
    scene::ObjectId object_id,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(application::BindConditionAssetObjectAction{
        expected_project_revision,
        expected_authoring_revision,
        asset_id,
        object_id}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::set_condition_source_reviewed(
    condition::Id source_id,
    bool reviewed) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION source review"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    const auto source = std::find_if(authoring.value().sources.begin(),
        authoring.value().sources.end(), [source_id](const auto& value) {
            return value.id == source_id;
        });
    if (source == authoring.value().sources.end()) {
        return core::Result<application::DispatchReceipt>::failure(
            core::Diagnostic(core::ErrorCode::not_found,
                "CONDITION source evidence does not exist"));
    }
    if (source->kind != condition::SourceKind::imported_asset) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "only imported asset evidence can be reviewed through the clip workflow"));
    }
    if (reviewed && !source->current) {
        return core::Result<application::DispatchReceipt>::failure(stale(
            "stale CONDITION source evidence must be reimported before review"));
    }
    if (source->reviewed == reviewed) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION source review state already matches the requested state"));
    }
    source->reviewed = reviewed;
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
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

core::Result<application::DispatchReceipt> UiController::add_condition_audio_source(
    condition::AuthoredAudioSource source) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION audio source authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().audio_sources, std::move(source), "audio source"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_acoustic_zone(
    condition::AuthoredAcousticZone zone) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION acoustic-zone authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().acoustic_zones, std::move(zone), "acoustic zone"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::update_condition_audio_source_distances(
    condition::Id source_id,
    core::Revision expected_authoring_revision,
    double minimum_distance,
    double maximum_distance) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION audio distance edit"));
    if (authoring.value().project_revision != expected_authoring_revision) {
        return core::Result<application::DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION audio distance edit was based on a stale authoring revision"));
    }
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    const auto found = std::find_if(authoring.value().audio_sources.begin(),
        authoring.value().audio_sources.end(), [source_id](const auto& source) {
            return source.id == source_id;
        });
    if (found == authoring.value().audio_sources.end()) {
        return core::Result<application::DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "CONDITION audio source does not exist"));
    }
    found->minimum_distance = minimum_distance;
    found->maximum_distance = maximum_distance;
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::set_condition_audio_source_enabled(
    condition::Id source_id,
    core::Revision expected_authoring_revision,
    bool enabled) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION audio source state"));
    if (authoring.value().project_revision != expected_authoring_revision) {
        return core::Result<application::DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION audio state edit was based on a stale authoring revision"));
    }
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    const auto found = std::find_if(authoring.value().audio_sources.begin(),
        authoring.value().audio_sources.end(), [source_id](const auto& source) {
            return source.id == source_id;
        });
    if (found == authoring.value().audio_sources.end()) {
        return core::Result<application::DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "CONDITION audio source does not exist"));
    }
    found->enabled = enabled;
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::update_condition_acoustic_zone_volume(
    condition::Id zone_id,
    core::Revision expected_authoring_revision,
    core::Vec3d half_extents,
    double radius) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION acoustic-zone volume edit"));
    if (authoring.value().project_revision != expected_authoring_revision) {
        return core::Result<application::DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION acoustic-zone edit was based on a stale authoring revision"));
    }
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    const auto found = std::find_if(authoring.value().acoustic_zones.begin(),
        authoring.value().acoustic_zones.end(), [zone_id](const auto& zone) {
            return zone.id == zone_id;
        });
    if (found == authoring.value().acoustic_zones.end()) {
        return core::Result<application::DispatchReceipt>::failure(core::Diagnostic(
            core::ErrorCode::not_found, "CONDITION acoustic zone does not exist"));
    }
    found->half_extents = half_extents;
    found->radius = radius;
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_effect_source(
    condition::EffectSource source) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION effect source authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().effect_sources, std::move(source), "effect source"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_particle_emitter(
    condition::ParticleEmitterSource emitter) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION particle emitter authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().particle_emitters, std::move(emitter), "particle emitter"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_field_influence(
    condition::FieldInfluenceBinding influence) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION field influence authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().field_influences, std::move(influence), "field influence"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_vegetation_scatter(
    condition::VegetationScatterRecipe recipe) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION vegetation scatter authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().vegetation_scatters, std::move(recipe),
            "vegetation scatter recipe"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_state_branch(
    condition::AuthoredStateBranch branch) {
    auto authoring = session_.condition_authoring();
    if (!authoring) return core::Result<application::DispatchReceipt>::failure(
        authoring.error().with_context("CONDITION state branch authoring"));
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    if (auto appended = append_condition_record(
            authoring.value().state_branches, std::move(branch), "state branch"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<condition::EffectPreviewResult> UiController::preview_condition_effect(
    condition::Id effect_source_id,
    std::string_view operating_state,
    double elapsed_seconds) const {
    return session_.condition_effect_preview(
        effect_source_id, operating_state, elapsed_seconds);
}

core::Result<condition::VegetationPreviewResult> UiController::preview_condition_vegetation(
    condition::Id recipe_id) const {
    return session_.condition_vegetation_preview(recipe_id);
}

core::Result<condition::StateComparisonResult>
UiController::compare_condition_state_branches(
    condition::Id left_branch_id,
    condition::Id right_branch_id) const {
    return session_.condition_state_comparison(left_branch_id, right_branch_id);
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
    result.agent_recipe_id = agent_recipe_id_;
    result.agent_plan_draft = agent_plan_draft_;
    result.agent_hard_constraints_draft = agent_hard_constraints_draft_;
    result.agent_clarifications_draft = agent_clarifications_draft_;
    result.agent_repair_ready = agent_repair_mode_;
    result.agent_checkpoint_loaded = agent_checkpoint_pending_.has_value();
    result.agent_checkpoint_rebased = agent_checkpoint_rebased_;
    if (agent_checkpoint_pending_.has_value()) {
        result.agent_checkpoint_task_id = agent_checkpoint_pending_->plan.id;
        result.agent_checkpoint_base_revision = agent_checkpoint_pending_->source_revision;
        result.agent_checkpoint_rebase_revision = agent_plan_base_revision_.value_or(
            agent_checkpoint_pending_->source_revision);
    }
    if (agent_task_.has_value()) {
        const auto task = agent_task_->snapshot();
        result.agent_task = task;
        if (preferences_.workspace != Workspace::ai &&
            task.status != agent::TaskStatus::accepted &&
            task.status != agent::TaskStatus::rejected) {
            AgentTaskStripView strip;
            strip.task_id = task.task_id;
            strip.status = task.status;
            strip.total_steps = task.plan_steps.size();
            strip.blocker = task.diagnostic;
            for (const auto& step : task.plan_steps) {
                if (step.status == agent::StepStatus::passed) ++strip.passed_steps;
            }
            for (const auto& gate : task.gates) {
                if (!gate.required) continue;
                ++strip.required_gates;
                if (gate.passed) ++strip.passed_required_gates;
            }
            result.agent_task_strip = std::move(strip);
        }
    }
    if (agent_condition_inventory_.has_value()) {
        const bool active_task = result.agent_task.has_value() &&
            result.agent_task->status != agent::TaskStatus::accepted &&
            result.agent_task->status != agent::TaskStatus::rejected;
        if (active_task) {
            if (agent_condition_inventory_->source_revision ==
                    result.agent_task->speculative_revision &&
                agent_condition_inventory_scope_ == result.agent_task->scope_objects) {
                result.agent_condition_inventory = agent_condition_inventory_;
            }
        } else {
            std::vector<scene::ObjectId> current_scope = result.selection.objects;
            if (result.selection.component_object.has_value() &&
                std::find(current_scope.begin(), current_scope.end(),
                    *result.selection.component_object) == current_scope.end()) {
                current_scope.push_back(*result.selection.component_object);
            }
            std::sort(current_scope.begin(), current_scope.end());
            current_scope.erase(std::unique(current_scope.begin(), current_scope.end()),
                current_scope.end());
            if (agent_condition_inventory_->source_revision == result.project_revision &&
                agent_condition_inventory_scope_ == current_scope) {
                result.agent_condition_inventory = agent_condition_inventory_;
            }
        }
    }
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
    result.agent_demonstration = agent_demonstration_;
    result.agent_demonstration_trace = agent_demonstration_trace_;
    result.agent_demonstration_recipe_name = agent_demonstration_recipe_name_;
    result.agent_demonstration_target_set_count =
        agent_demonstration_target_set_count_;
    if (agent_demonstration_trace_.has_value()) {
        result.agent_demonstration_similar_targets_supported =
            supports_demonstration_structural_matching(*agent_demonstration_trace_);
        const bool bindings_current = agent_demonstration_binding_revision_.has_value() &&
            *agent_demonstration_binding_revision_ == result.project_revision;
        auto requirements = demonstration_binding_requirements(
            *agent_demonstration_trace_, agent_demonstration_target_set_count_);
        result.agent_demonstration_ready_to_preview = !requirements.empty() &&
            bindings_current && !agent_task_.has_value() &&
            !agent_checkpoint_pending_.has_value();
        for (auto& requirement : requirements) {
            const auto binding = std::find_if(
                agent_demonstration_bindings_.begin(),
                agent_demonstration_bindings_.end(), [&requirement](const auto& binding) {
                    return binding.target_set_index == requirement.target_set_index &&
                        binding.target_alias == requirement.target_alias &&
                        binding.component_alias == requirement.component_alias;
                });
            const auto object = binding == agent_demonstration_bindings_.end()
                ? result.objects.end()
                : std::find_if(result.objects.begin(), result.objects.end(),
                    [&binding](const auto& candidate) {
                        return candidate.object.id == binding->target.object;
                    });
            requirement.bound = bindings_current &&
                binding != agent_demonstration_bindings_.end() &&
                object != result.objects.end();
            if (requirement.bound) {
                requirement.target_label = object->object.name.empty()
                    ? "(unnamed object)"
                    : object->object.name;
                requirement.target_label += " (#" +
                    std::to_string(object->object.id.value) + ")";
                if (const auto* face = std::get_if<geometry::FaceId>(
                        &binding->target.component)) {
                    requirement.target_label += ", face #" +
                        std::to_string(face->value);
                } else if (const auto* vertex = std::get_if<geometry::VertexId>(
                        &binding->target.component)) {
                    requirement.target_label += ", vertex #" +
                        std::to_string(vertex->value);
                }
            }
            result.agent_demonstration_ready_to_preview =
                result.agent_demonstration_ready_to_preview && requirement.bound;
            result.agent_demonstration_bindings.push_back(requirement);
        }
    }
    return result;
}

} // namespace carto::ui
