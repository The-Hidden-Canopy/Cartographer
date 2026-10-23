#include <carto/ui/ui.hpp>

#include <carto/assets/blob_store.hpp>
#include <carto/assets/texture_source.hpp>
#include <carto/io/builtin_provider.hpp>
#include <carto/io/obj.hpp>
#include <carto/io/ply.hpp>
#include <carto/io/stl.hpp>
#include <carto/providers/registry.hpp>
#include <carto/condition/wave_import.hpp>
#include <carto/engineering/cad_provider.hpp>
#include <carto/condition/state_branch.hpp>
#include <carto/kinematic/graph.hpp>
#include <carto/kinematic/limits.hpp>
#include <carto/kinematic/pose_preview.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
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
constexpr std::size_t kWorkbenchPreferenceV3FieldCount =
    kWorkbenchPreferenceV2FieldCount + kWorkbenchPaneCount;
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

core::Result<std::vector<std::string>> split_agent_goal_sequence(
    std::string_view goal) {
    constexpr std::size_t kMaxGoalBytes = 4096U;
    constexpr std::size_t kMaxSteps = 16U;
    if (goal.empty() || goal.size() > kMaxGoalBytes) {
        return core::Result<std::vector<std::string>>::failure(invalid(
            "agent goal must be non-empty and within the 4096-byte limit"));
    }
    const auto trim_command = [](std::string_view command) {
        std::size_t begin = 0U;
        while (begin < command.size() &&
            std::isspace(static_cast<unsigned char>(command[begin])) != 0) {
            ++begin;
        }
        std::size_t end = command.size();
        while (end > begin &&
            std::isspace(static_cast<unsigned char>(command[end - 1U])) != 0) {
            --end;
        }
        while (end > begin &&
            (command[end - 1U] == '.' || command[end - 1U] == '!' ||
             command[end - 1U] == '?')) {
            --end;
        }
        while (end > begin &&
            std::isspace(static_cast<unsigned char>(command[end - 1U])) != 0) {
            --end;
        }
        return std::string(command.substr(begin, end - begin));
    };
    const auto is_then_separator = [goal](std::size_t index) {
        if (index + 4U > goal.size() ||
            (index > 0U && std::isspace(static_cast<unsigned char>(goal[index - 1U])) == 0) ||
            (index + 4U < goal.size() &&
                std::isspace(static_cast<unsigned char>(goal[index + 4U])) == 0)) {
            return false;
        }
        constexpr std::string_view kThen = "then";
        for (std::size_t offset = 0U; offset < kThen.size(); ++offset) {
            const auto value = static_cast<unsigned char>(goal[index + offset]);
            if (static_cast<char>(std::tolower(value)) != kThen[offset]) return false;
        }
        return true;
    };

    std::vector<std::string> commands;
    std::size_t command_begin = 0U;
    const auto append_command = [&commands, &trim_command](std::string_view command)
        -> core::Result<void> {
        auto normalized = trim_command(command);
        if (normalized.empty()) {
            return core::Result<void>::failure(invalid(
                "agent goal sequence contains an empty command"));
        }
        if (commands.size() >= kMaxSteps) {
            return core::Result<void>::failure(invalid(
                "agent goal sequence exceeds the 16-command limit"));
        }
        commands.push_back(std::move(normalized));
        return core::Result<void>::success();
    };
    for (std::size_t index = 0U; index < goal.size();) {
        const std::size_t separator_size = goal[index] == ';' ? 1U :
            (is_then_separator(index) ? 4U : 0U);
        if (separator_size == 0U) {
            ++index;
            continue;
        }
        if (auto appended = append_command(goal.substr(command_begin, index - command_begin));
            !appended) {
            return core::Result<std::vector<std::string>>::failure(appended.error());
        }
        index += separator_size;
        command_begin = index;
    }
    if (auto appended = append_command(goal.substr(command_begin)); !appended) {
        return core::Result<std::vector<std::string>>::failure(appended.error());
    }
    return core::Result<std::vector<std::string>>::success(std::move(commands));
}

bool starts_with_agent_command_words(
    std::string_view command,
    std::string_view first,
    std::string_view second,
    std::string_view third) {
    std::size_t cursor = 0U;
    const auto matches = [&command, &cursor](std::string_view expected) {
        while (cursor < command.size() &&
            std::isspace(static_cast<unsigned char>(command[cursor])) != 0) {
            ++cursor;
        }
        const std::size_t begin = cursor;
        while (cursor < command.size() &&
            std::isspace(static_cast<unsigned char>(command[cursor])) == 0) {
            ++cursor;
        }
        if (cursor - begin != expected.size()) return false;
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            const auto value = static_cast<unsigned char>(command[begin + index]);
            if (static_cast<char>(std::tolower(value)) != expected[index]) return false;
        }
        return true;
    };
    return matches(first) && matches(second) && matches(third);
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

bool is_demonstration_face_topology_chain(const agent::AgentDemonstrationTrace& trace);

std::vector<AgentDemonstrationTargetBindingView> demonstration_binding_requirements(
    const agent::AgentDemonstrationTrace& trace,
    std::size_t target_set_count) {
    std::vector<AgentDemonstrationTargetBindingView> requirements;
    // A face-topology chain binds only its first face; later faces are
    // resolved by replaying the chain on the bound candidate.
    const bool face_chain = is_demonstration_face_topology_chain(trace);
    for (std::size_t target_set_index = 0U;
         target_set_index < target_set_count; ++target_set_index) {
        for (const auto& step : trace.steps) {
            if (face_chain && &step != &trace.steps.front()) break;
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

// A multi-command chain of native face-topology edits on one object alias.
// After the first step, each step targets either the same face as the
// previous step or a face the previous step created (a new component alias);
// every step carries both structural signatures so the chain can be replayed
// and verified per step on a candidate.
bool is_demonstration_face_topology_chain(const agent::AgentDemonstrationTrace& trace) {
    if (trace.steps.size() < 2U ||
        trace.steps.size() > application::kMaxNativeFaceChainSteps) {
        return false;
    }
    const auto& first = trace.steps.front();
    std::set<std::uint32_t> seen_components;
    for (std::size_t index = 0U; index < trace.steps.size(); ++index) {
        const auto& step = trace.steps[index];
        const bool supported_tool = step.tool_id == "mesh.inset-face" ||
            step.tool_id == "mesh.extrude-face" || step.tool_id == "mesh.poke-face";
        if (step.target_kind != agent::AgentDemonstrationTargetKind::face || !supported_tool ||
            step.target_alias == 0U || step.target_alias != first.target_alias ||
            step.component_alias == 0U || !step.structure_signature.has_value() ||
            !step.result_object_structure_signature.has_value()) {
            return false;
        }
        const bool same_as_previous = index > 0U &&
            step.component_alias == trace.steps[index - 1U].component_alias;
        if (!same_as_previous && !seen_components.insert(step.component_alias).second) {
            return false;
        }
    }
    return true;
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
    if (is_demonstration_face_topology_chain(trace)) return true;
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
        if (face_id == nullptr) {
            return core::Result<std::string>::failure(invalid(
                "face demonstration binding does not match its native operation"));
        }
        if (step.tool_id == "mesh.poke-face" &&
            step.parameters.parameterization ==
                agent::AgentDemonstrationParameterization::parameterless) {
            return core::Result<std::string>::success("poke face");
        }
        if ((step.tool_id != "mesh.inset-face" && step.tool_id != "mesh.extrude-face") ||
            step.parameters.parameterization !=
                agent::AgentDemonstrationParameterization::normalized_face_distance) {
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
        value == Workspace::uv ||
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
        {Workspace::uv, "uv", "UV", asset_panels, BottomPanel::assets, true, {}},
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
            std::is_same_v<Action, application::SetPresentationCatalogAction> ||
            std::is_same_v<Action, application::CreateMeshObjectAction> ||
            std::is_same_v<Action, application::ApplySourceReimportPlanAction> ||
            std::is_same_v<Action, application::AppendSculptStrokeAction> ||
            std::is_same_v<Action, application::AppendSculptSurfaceStrokeAction> ||
            std::is_same_v<Action, application::SetSculptLayerVisibilityAction> ||
            std::is_same_v<Action, application::CreatePlantScatterBatchAction> ||
            std::is_same_v<Action, application::SetObjectTransformAction> ||
            std::is_same_v<Action, application::ImportEngineeringFixtureAction> ||
            std::is_same_v<Action, application::ImportEngineeringAuthoringAction> ||
            std::is_same_v<Action, application::ImportEngineeringBomCsvAction> ||
            std::is_same_v<Action, application::ImportEngineeringRequirementCsvAction> ||
            std::is_same_v<Action, application::ImportEngineeringPathResultAction> ||
            std::is_same_v<Action, application::AttachEngineeringEvidenceAction> ||
            std::is_same_v<Action, application::CreateEngineeringRequirementAction> ||
            std::is_same_v<Action, application::ReviewEngineeringRequirementAction> ||
            std::is_same_v<Action, application::CreateEngineeringCharacteristicAction> ||
            std::is_same_v<Action, application::CreateEngineeringTargetAction> ||
            std::is_same_v<Action, application::UpdateEngineeringWorkcellAction> ||
            std::is_same_v<Action, application::CreateEngineeringKeepOutAction> ||
            std::is_same_v<Action, application::CreateEngineeringWorkcellFrameAction> ||
            std::is_same_v<Action, application::ReviewEngineeringTargetAction> ||
            std::is_same_v<Action, application::InvalidateEngineeringSourceAction> ||
            std::is_same_v<Action, application::ImportConditionFixtureAction> ||
            std::is_same_v<Action, application::ImportConditionAuthoringAction> ||
            std::is_same_v<Action, application::BindConditionAssetObjectAction> ||
            std::is_same_v<Action, application::AddConditionInteractionAnchorAction> ||
            std::is_same_v<Action, application::AddConditionPrefabAction> ||
            std::is_same_v<Action, application::InstantiateConditionPrefabAction> ||
            std::is_same_v<Action, application::AddConditionCollectionAction> ||
            std::is_same_v<Action, application::AddConditionRegionKitAction> ||
            std::is_same_v<Action, application::AddConditionKinematicAssemblyAction> ||
            std::is_same_v<Action, application::ReplaceConditionAuthoredRecordAction> ||
            std::is_same_v<Action, application::DeleteConditionAuthoredRecordAction> ||
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
        {"uv", Workspace::uv},
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

core::Result<std::string> read_file_prefix(
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
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return core::Result<std::string>::failure(io_error(
            "unable to open " + std::string(description) + " file"));
    }
    std::string prefix(maximum_bytes, '\0');
    stream.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
    const auto read = stream.gcount();
    if (stream.bad() || (stream.fail() && !stream.eof())) {
        return core::Result<std::string>::failure(io_error(
            "unable to read " + std::string(description) + " header"));
    }
    prefix.resize(static_cast<std::size_t>(read));
    return core::Result<std::string>::success(std::move(prefix));
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

core::Result<StagedMeshSource> persist_botanical_source_blob(
    const std::filesystem::path& project_path,
    std::span<const std::uint8_t> bytes,
    std::string media_type,
    std::size_t maximum_bytes,
    std::string_view source_description) {
    if (bytes.empty() || bytes.size() > maximum_bytes ||
        project_path.empty() || project_path.filename().empty()) {
        return core::Result<StagedMeshSource>::failure(validation(
            std::string(source_description) + " or saved project path is invalid"));
    }
    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error) : project_path.parent_path();
    if (error) return core::Result<StagedMeshSource>::failure(io_error(
        "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) {
        return core::Result<StagedMeshSource>::failure(io_error(
            "unable to resolve the saved project directory"));
    }

    const auto digest = assets::sha256(bytes);
    const auto store_root = project_root / ".carto-assets" / "source-blobs";
    assets::BlobStore store(store_root, assets::BlobStoreLimits{
        static_cast<std::uint64_t>(maximum_bytes), 256U, 1U});
    const auto destination = store.path_for(digest);
    if (auto checked = validate_managed_directory_chain(
            project_root, destination.parent_path()); !checked) {
        return core::Result<StagedMeshSource>::failure(checked.error());
    }
    const auto destination_status = std::filesystem::symlink_status(destination, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return core::Result<StagedMeshSource>::failure(io_error(
            "unable to inspect managed botanical source destination"));
    }
    error.clear();
    if (std::filesystem::is_symlink(destination_status)) {
        return core::Result<StagedMeshSource>::failure(validation(
            "managed botanical source destination cannot be a symbolic link"));
    }
    const auto stored = store.put(bytes, std::move(media_type));
    if (!stored || stored.value().digest != digest) {
        return core::Result<StagedMeshSource>::failure(stored
            ? validation("botanical source digest changed during persistence")
            : stored.error());
    }
    const auto canonical_destination = std::filesystem::canonical(destination, error);
    if (error || !is_within_directory(project_root, canonical_destination)) {
        return core::Result<StagedMeshSource>::failure(validation(
            "botanical source resolved outside the saved project directory"));
    }
    const auto relative =
        canonical_destination.lexically_relative(project_root).generic_string();
    if (relative.empty() || relative.starts_with("../")) {
        return core::Result<StagedMeshSource>::failure(validation(
            "botanical source has no safe project-relative path"));
    }
    return core::Result<StagedMeshSource>::success(StagedMeshSource{relative, digest});
}

core::Result<StagedMeshSource> persist_generated_botanical_source_blob(
    const std::filesystem::path& project_path,
    std::span<const std::uint8_t> bytes) {
    return persist_botanical_source_blob(project_path, bytes, "model/gltf+json",
        40U * 1024U * 1024U, "generated botanical artifact");
}

core::Result<std::string> persist_texture_source_blob(
    const std::filesystem::path& project_path,
    std::string_view bytes,
    const assets::TextureSourceAdmission& admission,
    const assets::TextureSourceLimits& limits) {
    if (auto checked = limits.validate(); !checked) {
        return core::Result<std::string>::failure(checked.error());
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::span<const std::uint8_t> source(data, bytes.size());
    if (bytes.size() > limits.max_source_bytes ||
        assets::sha256(source) != admission.source.digest) {
        return core::Result<std::string>::failure(stale(
            "material texture bytes changed after source inspection"));
    }
    std::error_code error;
    const auto requested_root = project_path.parent_path().empty()
        ? std::filesystem::current_path(error)
        : project_path.parent_path();
    if (error) return core::Result<std::string>::failure(io_error(
        "unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) return core::Result<std::string>::failure(io_error(
        "unable to resolve the saved project directory"));

    const auto store_root = project_root / ".carto-assets" / "source-blobs";
    assets::BlobStore store(store_root,
        assets::BlobStoreLimits{limits.max_source_bytes, 256U});
    const auto destination = store.path_for(admission.source.digest);
    if (auto checked = validate_managed_directory_chain(
            project_root, destination.parent_path()); !checked) {
        return core::Result<std::string>::failure(checked.error());
    }
    const auto destination_status = std::filesystem::symlink_status(destination, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return core::Result<std::string>::failure(io_error(
            "unable to inspect managed material texture source"));
    }
    error.clear();
    if (std::filesystem::is_symlink(destination_status)) {
        return core::Result<std::string>::failure(validation(
            "managed material texture destination cannot be a symbolic link"));
    }
    const auto stored = store.put(source, admission.source.media_type);
    if (!stored) return core::Result<std::string>::failure(stored.error());
    if (stored.value().digest != admission.source.digest) {
        return core::Result<std::string>::failure(validation(
            "managed material texture digest changed during persistence"));
    }
    const auto canonical_destination = std::filesystem::canonical(destination, error);
    if (error || canonical_destination.lexically_normal() != destination.lexically_normal() ||
        !is_within_directory(project_root, canonical_destination)) {
        return core::Result<std::string>::failure(validation(
            "managed material texture resolved outside its project-local content path"));
    }
    const auto relative =
        canonical_destination.lexically_relative(project_root).generic_string();
    if (relative.empty() || relative.starts_with("../")) {
        return core::Result<std::string>::failure(validation(
            "managed material texture has no safe project-relative locator"));
    }
    return core::Result<std::string>::success(relative);
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
    const bool v2_format = header != fields.end() &&
        header->second == "CARTOGRAPHER_WORKBENCH_PREFS_V2";
    const bool current_format = header != fields.end() &&
        header->second == "CARTOGRAPHER_WORKBENCH_PREFS_V3";
    if (!legacy_format && !v2_format && !current_format) {
        return core::Result<WorkbenchPreferences>::failure(
            validation("workbench preference header is invalid"));
    }
    const std::size_t expected_field_count = current_format
        ? kWorkbenchPreferenceV3FieldCount
        : v2_format ? kWorkbenchPreferenceV2FieldCount
                    : kWorkbenchPreferenceV1FieldCount;
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
    if (v2_format || current_format) {
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
    if (current_format) {
        for (std::size_t index = 0U; index < kWorkbenchPaneCount; ++index) {
            const auto pane = static_cast<WorkbenchPane>(index);
            const auto field = fields.find(workbench_pane_name(pane));
            if (field == fields.end()) {
                return core::Result<WorkbenchPreferences>::failure(validation(
                    "workbench preference field is missing: " +
                    std::string(workbench_pane_name(pane))));
            }
            const auto parsed = parse_workbench_layout(field->second);
            if (!parsed) return core::Result<WorkbenchPreferences>::failure(parsed.error());
            result.panes[index] = WorkbenchPaneLayout{
                parsed.value().visible, parsed.value().positioned,
                parsed.value().x, parsed.value().y,
                parsed.value().width, parsed.value().height};
        }
    }
    if (auto valid_result = result.validate(); !valid_result) {
        return core::Result<WorkbenchPreferences>::failure(valid_result.error());
    }
    return core::Result<WorkbenchPreferences>::success(std::move(result));
}

std::string serialize_workbench_preferences(const WorkbenchPreferences& preferences) {
    std::string result = "header=CARTOGRAPHER_WORKBENCH_PREFS_V3\n";
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
    for (std::size_t index = 0U; index < kWorkbenchPaneCount; ++index) {
        const auto& layout = preferences.panes[index];
        result += workbench_pane_name(static_cast<WorkbenchPane>(index));
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

std::string WorkspaceDefinition::availability_summary() const {
    return available ? "Available" : "Unavailable: " + unavailable_reason;
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

bool record_workbench_instrument_geometry(
    WorkbenchInstrumentLayout& layout,
    double x, double y, double width, double height) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
        !std::isfinite(height)) {
        return false;
    }
    const auto rounded_clamped = [](double value, std::int32_t minimum,
                                    std::int32_t maximum) noexcept {
        return static_cast<std::int32_t>(std::lround(std::clamp(
            value, static_cast<double>(minimum), static_cast<double>(maximum))));
    };
    layout.x = rounded_clamped(x, -65536, 65536);
    layout.y = rounded_clamped(y, -65536, 65536);
    layout.width = rounded_clamped(width, 120, 4096);
    layout.height = rounded_clamped(
        height, static_cast<std::int32_t>(kWorkbenchInstrumentMinimumHeight), 4096);
    layout.positioned = true;
    return true;
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
    const auto validate_layout = [](const auto& layout, const char* name) {
        if (!layout.positioned) {
            if (layout.x != 0 || layout.y != 0 || layout.width != 0 || layout.height != 0) {
                return core::Result<void>::failure(invalid(
                    std::string("unpositioned workbench ") + name +
                    " cannot have geometry"));
            }
            return core::Result<void>::success();
        }
        if (layout.width < 120 || layout.width > 4096 || layout.height < 80 ||
            layout.height > 4096 || layout.x < -65536 || layout.x > 65536 ||
            layout.y < -65536 || layout.y > 65536) {
            return core::Result<void>::failure(invalid(
                std::string("workbench ") + name + " layout is outside safe bounds"));
        }
        return core::Result<void>::success();
    };
    for (const auto& layout : instruments) {
        if (auto result = validate_layout(layout, "instrument"); !result) return result;
    }
    for (const auto& layout : panes) {
        if (auto result = validate_layout(layout, "pane"); !result) return result;
    }
    return core::Result<void>::success();
}

std::optional<core::Vec2d> nearest_workbench_snap_target(
    core::Vec2d position,
    std::span<const core::Vec2d> candidates,
    double snap_distance) noexcept {
    if (!position.finite() || !std::isfinite(snap_distance) ||
        snap_distance < 0.0 || snap_distance > 1.0e6) {
        return std::nullopt;
    }

    double best_distance_squared = std::numeric_limits<double>::infinity();
    std::optional<core::Vec2d> best_candidate;
    for (const core::Vec2d candidate : candidates) {
        if (!candidate.finite()) continue;
        const double dx = candidate.x - position.x;
        const double dy = candidate.y - position.y;
        // Docking is axis-aligned: a corner remains reachable when both axes
        // are within the magnet radius, even if diagonal distance is larger.
        if (std::abs(dx) > snap_distance || std::abs(dy) > snap_distance) continue;
        const double distance_squared = dx * dx + dy * dy;
        if (!std::isfinite(distance_squared)) {
            continue;
        }
        if (best_candidate.has_value()) {
            const bool closer = distance_squared < best_distance_squared;
            const bool tied = distance_squared == best_distance_squared;
            const bool stable_tie_break = tied &&
                (candidate.x < best_candidate->x ||
                 (candidate.x == best_candidate->x &&
                  candidate.y < best_candidate->y));
            if (!closer && !stable_tie_break) continue;
        }
        best_distance_squared = distance_squared;
        best_candidate = candidate;
    }
    return best_candidate;
}

std::optional<core::Vec2d> nearest_workbench_snap_target(
    core::Vec2d position,
    std::span<const core::Vec2d> candidates,
    double snap_distance,
    core::Vec2d minimum,
    core::Vec2d maximum) noexcept {
    if (!minimum.finite() || !maximum.finite() ||
        maximum.x < minimum.x || maximum.y < minimum.y) {
        return std::nullopt;
    }
    std::optional<core::Vec2d> best_candidate;
    double best_distance_squared = std::numeric_limits<double>::infinity();
    if (!position.finite() || !std::isfinite(snap_distance) ||
        snap_distance < 0.0 || snap_distance > 1.0e6) {
        return std::nullopt;
    }
    for (const core::Vec2d candidate : candidates) {
        if (!candidate.finite() || candidate.x < minimum.x || candidate.x > maximum.x ||
            candidate.y < minimum.y || candidate.y > maximum.y) {
            continue;
        }
        const double dx = candidate.x - position.x;
        const double dy = candidate.y - position.y;
        if (std::abs(dx) > snap_distance || std::abs(dy) > snap_distance) continue;
        const double distance_squared = dx * dx + dy * dy;
        if (!std::isfinite(distance_squared)) {
            continue;
        }
        if (best_candidate.has_value()) {
            const bool closer = distance_squared < best_distance_squared;
            const bool tied = distance_squared == best_distance_squared;
            const bool stable_tie_break = tied &&
                (candidate.x < best_candidate->x ||
                 (candidate.x == best_candidate->x &&
                  candidate.y < best_candidate->y));
            if (!closer && !stable_tie_break) continue;
        }
        best_distance_squared = distance_squared;
        best_candidate = candidate;
    }
    return best_candidate;
}

std::optional<core::Vec2d> workbench_instrument_snap_position(
    core::Vec2d position,
    core::Vec2d instrument_size,
    core::Vec2d work_surface_origin,
    core::Vec2d work_surface_extent,
    std::span<const WorkbenchSnapWindow> other_windows,
    double gap,
    double snap_distance,
    double margin) {
    if (!position.finite() || !instrument_size.finite() ||
        !work_surface_origin.finite() || !work_surface_extent.finite() ||
        instrument_size.x <= 0.0 || instrument_size.y <= 0.0 ||
        work_surface_extent.x < 0.0 || work_surface_extent.y < 0.0 ||
        !std::isfinite(gap) || gap < 0.0 || gap > 1.0e6 ||
        !std::isfinite(snap_distance) || snap_distance < 0.0 ||
        snap_distance > 1.0e6 || !std::isfinite(margin) ||
        margin < 0.0 || margin > 1.0e6 ||
        other_windows.size() > kWorkbenchInstrumentCount) {
        return std::nullopt;
    }

    const core::Vec2d minimum{
        work_surface_origin.x + margin,
        work_surface_origin.y + margin};
    const core::Vec2d maximum{
        std::max(minimum.x, work_surface_origin.x + work_surface_extent.x -
            instrument_size.x - margin),
        std::max(minimum.y, work_surface_origin.y + work_surface_extent.y -
            instrument_size.y - margin)};
    if (!minimum.finite() || !maximum.finite()) return std::nullopt;

    const core::Vec2d clamped = clamp_workbench_instrument_position(
        position, instrument_size, work_surface_origin, work_surface_extent, margin);
    std::vector<core::Vec2d> candidates;
    candidates.reserve(4U + other_windows.size() * 8U);
    candidates.push_back({minimum.x, clamped.y});
    candidates.push_back({maximum.x, clamped.y});
    candidates.push_back({clamped.x, minimum.y});
    candidates.push_back({clamped.x, maximum.y});

    for (const auto& other : other_windows) {
        if (!other.position.finite() || !other.size.finite() ||
            other.size.x <= 0.0 || other.size.y <= 0.0) {
            continue;
        }
        // Dock beside another instrument without forcing it to share the
        // other instrument's top edge. The free axis remains under the user's
        // pointer, which makes side docking reachable from any vertical offset.
        candidates.push_back({other.position.x + other.size.x + gap, clamped.y});
        candidates.push_back({other.position.x - instrument_size.x - gap, clamped.y});
        candidates.push_back({clamped.x, other.position.y + other.size.y + gap});
        candidates.push_back({clamped.x, other.position.y - instrument_size.y - gap});
        candidates.push_back({other.position.x, clamped.y});
        candidates.push_back({other.position.x + other.size.x - instrument_size.x, clamped.y});
        candidates.push_back({clamped.x, other.position.y});
        candidates.push_back({clamped.x, other.position.y + other.size.y - instrument_size.y});
    }

    return nearest_workbench_snap_target(
        clamped, candidates, snap_distance, minimum, maximum);
}

core::Vec2d fit_workbench_instrument_size(
    core::Vec2d requested_size,
    core::Vec2d work_surface_extent,
    double maximum_surface_fraction) noexcept {
    constexpr double minimum_width = 120.0;
    constexpr double minimum_height = kWorkbenchInstrumentMinimumHeight;
    if (!requested_size.finite() || !work_surface_extent.finite() ||
        !std::isfinite(maximum_surface_fraction) ||
        maximum_surface_fraction <= 0.0 || maximum_surface_fraction > 1.0) {
        return {minimum_width, minimum_height};
    }
    const double maximum_width = std::max(
        minimum_width, work_surface_extent.x * maximum_surface_fraction);
    const double maximum_height = std::max(
        minimum_height, work_surface_extent.y * maximum_surface_fraction);
    return {
        std::clamp(requested_size.x, minimum_width, maximum_width),
        std::clamp(requested_size.y, minimum_height, maximum_height)};
}

core::Vec2d clamp_workbench_instrument_position(
    core::Vec2d position,
    core::Vec2d instrument_size,
    core::Vec2d work_surface_origin,
    core::Vec2d work_surface_extent,
    double margin) noexcept {
    if (!position.finite() || !instrument_size.finite() ||
        !work_surface_origin.finite() || !work_surface_extent.finite() ||
        instrument_size.x < 0.0 || instrument_size.y < 0.0 ||
        work_surface_extent.x < 0.0 || work_surface_extent.y < 0.0 ||
        !std::isfinite(margin) || margin < 0.0 || margin > 1.0e6) {
        return work_surface_origin.finite() ? work_surface_origin : core::Vec2d{};
    }
    const core::Vec2d minimum{
        work_surface_origin.x + margin,
        work_surface_origin.y + margin};
    const core::Vec2d maximum{
        std::max(minimum.x, work_surface_origin.x + work_surface_extent.x -
            instrument_size.x - margin),
        std::max(minimum.y, work_surface_origin.y + work_surface_extent.y -
            instrument_size.y - margin)};
    if (!minimum.finite() || !maximum.finite()) return work_surface_origin;
    return {
        std::clamp(position.x, minimum.x, maximum.x),
        std::clamp(position.y, minimum.y, maximum.y)};
}

core::Vec2d workbench_placement_window_position(
    core::Vec2d pointer,
    core::Vec2d instrument_size,
    core::Vec2d pointer_grab_offset,
    core::Vec2d work_surface_origin,
    core::Vec2d work_surface_extent,
    double margin) noexcept {
    if (!pointer.finite() || !pointer_grab_offset.finite()) {
        return clamp_workbench_instrument_position(
            work_surface_origin, instrument_size, work_surface_origin,
            work_surface_extent, margin);
    }
    return clamp_workbench_instrument_position(
        {pointer.x - pointer_grab_offset.x,
         pointer.y - pointer_grab_offset.y},
        instrument_size, work_surface_origin, work_surface_extent, margin);
}

double workbench_instrument_drag_grip_width(double available_content_width) noexcept {
    constexpr double minimum_width = 1.0;
    return std::isfinite(available_content_width)
        ? std::max(minimum_width, available_content_width)
        : minimum_width;
}

bool workbench_instrument_drag_controls_stacked(
    double available_content_width) noexcept {
    return !std::isfinite(available_content_width) ||
        available_content_width < kWorkbenchInstrumentDragControlsStackBreakpoint;
}

const char* workbench_instrument_drag_grip_label(double grip_width) noexcept {
    return std::isfinite(grip_width) && grip_width >= 156.0
        ? "DRAG TO MOVE · SNAP"
        : "DRAG";
}

bool workbench_drag_grip_contains(
    core::Vec2d pointer,
    core::Vec2d minimum,
    core::Vec2d maximum) noexcept {
    return pointer.finite() && minimum.finite() && maximum.finite() &&
        maximum.x >= minimum.x && maximum.y >= minimum.y &&
        pointer.x >= minimum.x && pointer.x <= maximum.x &&
        pointer.y >= minimum.y && pointer.y <= maximum.y;
}

bool workbench_instrument_drag_region_contains(
    core::Vec2d pointer,
    core::Vec2d grip_minimum,
    core::Vec2d grip_maximum,
    core::Vec2d titlebar_minimum,
    core::Vec2d titlebar_maximum,
    bool titlebar_valid) noexcept {
    return workbench_drag_grip_contains(pointer, grip_minimum, grip_maximum) ||
        (titlebar_valid && workbench_drag_grip_contains(
            pointer, titlebar_minimum, titlebar_maximum));
}

std::optional<WorkbenchInstrument> topmost_workbench_drag_target(
    core::Vec2d pointer,
    std::span<const WorkbenchInstrumentDragTarget> targets) noexcept {
    if (!pointer.finite()) return std::nullopt;
    const WorkbenchInstrumentDragTarget* topmost_window = nullptr;
    for (const auto& target : targets) {
        if (!target.visible || !target.window_valid ||
            !workbench_drag_grip_contains(
                pointer, target.window_minimum, target.window_maximum)) {
            continue;
        }
        if (topmost_window == nullptr ||
            target.display_order > topmost_window->display_order) {
            topmost_window = &target;
        }
    }
    if (topmost_window != nullptr) {
        return workbench_instrument_drag_region_contains(
                pointer, topmost_window->grip_minimum,
                topmost_window->grip_maximum,
                topmost_window->titlebar_minimum,
                topmost_window->titlebar_maximum,
                topmost_window->titlebar_valid)
            ? std::optional<WorkbenchInstrument>{topmost_window->instrument}
            : std::nullopt;
    }
    const WorkbenchInstrumentDragTarget* topmost = nullptr;
    for (const auto& target : targets) {
        if (!target.visible ||
            !workbench_instrument_drag_region_contains(
                pointer, target.grip_minimum, target.grip_maximum,
                target.titlebar_minimum, target.titlebar_maximum,
                target.titlebar_valid)) {
            continue;
        }
        if (topmost == nullptr || target.display_order > topmost->display_order) {
            topmost = &target;
        }
    }
    return topmost == nullptr
        ? std::nullopt
        : std::optional<WorkbenchInstrument>{topmost->instrument};
}

std::optional<WorkbenchInstrument> topmost_workbench_drag_grip_target(
    core::Vec2d pointer,
    std::span<const WorkbenchInstrumentDragTarget> targets) noexcept {
    if (!pointer.finite()) return std::nullopt;
    const WorkbenchInstrumentDragTarget* topmost_window = nullptr;
    for (const auto& target : targets) {
        if (!target.visible || !target.window_valid ||
            !workbench_drag_grip_contains(
                pointer, target.window_minimum, target.window_maximum)) {
            continue;
        }
        if (topmost_window == nullptr ||
            target.display_order > topmost_window->display_order) {
            topmost_window = &target;
        }
    }
    if (topmost_window != nullptr) {
        return workbench_drag_grip_contains(
                pointer, topmost_window->grip_minimum,
                topmost_window->grip_maximum)
            ? std::optional<WorkbenchInstrument>{topmost_window->instrument}
            : std::nullopt;
    }
    const WorkbenchInstrumentDragTarget* topmost = nullptr;
    for (const auto& target : targets) {
        if (!target.visible ||
            !workbench_drag_grip_contains(
                pointer, target.grip_minimum, target.grip_maximum)) {
            continue;
        }
        if (topmost == nullptr || target.display_order > topmost->display_order) {
            topmost = &target;
        }
    }
    return topmost == nullptr
        ? std::nullopt
        : std::optional<WorkbenchInstrument>{topmost->instrument};
}

bool workbench_instrument_window_contains(
    core::Vec2d pointer,
    core::Vec2d window_minimum,
    core::Vec2d window_maximum,
    bool visible) noexcept {
    return visible && workbench_drag_grip_contains(
        pointer, window_minimum, window_maximum);
}

bool workbench_instrument_should_settle(
    bool pending_snap,
    bool titlebar_drag_moved,
    bool explicit_grip_drag_moved,
    bool mouse_released) noexcept {
    if (!pending_snap && !mouse_released) return false;
    return pending_snap || (mouse_released &&
        (titlebar_drag_moved || explicit_grip_drag_moved));
}

NativePointerEvent native_pointer_event_to_screen_space(
    NativePointerEvent client_event,
    std::int32_t screen_offset_x,
    std::int32_t screen_offset_y) noexcept {
    const auto translate = [](std::int32_t value, std::int32_t offset) {
        const std::int64_t translated = static_cast<std::int64_t>(value) + offset;
        return static_cast<std::int32_t>(std::clamp<std::int64_t>(
            translated,
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    };
    client_event.x = translate(client_event.x, screen_offset_x);
    client_event.y = translate(client_event.y, screen_offset_y);
    return client_event;
}

bool native_pointer_requests_pan(
    NativePointerButton button,
    bool pan_tool_active,
    bool space_down,
    bool alt_down) noexcept {
    if (button == NativePointerButton::middle ||
        button == NativePointerButton::secondary) {
        return true;
    }
    return button == NativePointerButton::primary &&
        (pan_tool_active || space_down || alt_down);
}

bool work_surface_pointer_requests_pan(
    NativePointerButton button,
    bool pan_tool_active,
    bool space_down,
    bool alt_down,
    bool empty_surface) noexcept {
    return native_pointer_requests_pan(button, pan_tool_active, space_down, alt_down) ||
        (button == NativePointerButton::primary && empty_surface);
}

bool work_surface_camera_pan_allowed(
    bool viewport_valid,
    bool camera_input_allowed,
    bool pointer_inside_viewport) noexcept {
    return viewport_valid && camera_input_allowed && pointer_inside_viewport;
}

bool work_surface_camera_hotkey_allowed(
    bool viewport_focused,
    bool camera_input_allowed,
    bool text_input_active,
    bool keyboard_captured,
    bool key_pressed) noexcept {
    return viewport_focused && camera_input_allowed && !text_input_active &&
        !keyboard_captured && key_pressed;
}

core::Vec2d work_surface_camera_keyboard_pan_delta(
    bool left_pressed,
    bool right_pressed,
    bool up_pressed,
    bool down_pressed,
    core::Vec2d viewport_extent) noexcept {
    if (!viewport_extent.finite() || viewport_extent.x <= 0.0 ||
        viewport_extent.y <= 0.0 || viewport_extent.x > 1.0e7 ||
        viewport_extent.y > 1.0e7) {
        return {};
    }
    const double horizontal = static_cast<double>(right_pressed) -
        static_cast<double>(left_pressed);
    const double vertical = static_cast<double>(down_pressed) -
        static_cast<double>(up_pressed);
    if (horizontal == 0.0 && vertical == 0.0) return {};
    constexpr double kPageFraction = 0.16;
    const double diagonal_scale = horizontal != 0.0 && vertical != 0.0
        ? 1.0 / std::sqrt(2.0) : 1.0;
    return {horizontal * viewport_extent.x * kPageFraction * diagonal_scale,
            vertical * viewport_extent.y * kPageFraction * diagonal_scale};
}

bool work_surface_pointer_is_unclaimed(
    bool hit_cache_matches_revision,
    bool pointer_hits_scene) noexcept {
    // A stale hit cache cannot safely claim the pointer for scene selection.
    // Keep background navigation available until current-frame hit evidence exists.
    return !hit_cache_matches_revision || !pointer_hits_scene;
}

core::Vec2d workbench_release_position(
    bool has_native_release,
    core::Vec2d native_release_position,
    core::Vec2d fallback_position) noexcept {
    if (has_native_release && native_release_position.finite()) {
        return native_release_position;
    }
    return fallback_position.finite() ? fallback_position : core::Vec2d{};
}

bool workbench_drag_threshold_crossed(
    core::Vec2d press_position,
    core::Vec2d current_position,
    double threshold) noexcept {
    if (!press_position.finite() || !current_position.finite() ||
        !std::isfinite(threshold) || threshold < 0.0 || threshold > 1.0e6) {
        return false;
    }
    return std::hypot(current_position.x - press_position.x,
                      current_position.y - press_position.y) >= threshold;
}

core::Vec2d workbench_dragged_position(
    core::Vec2d initial_window_position,
    core::Vec2d initial_pointer_position,
    core::Vec2d current_pointer_position) noexcept {
    if (!initial_window_position.finite() || !initial_pointer_position.finite() ||
        !current_pointer_position.finite()) {
        return initial_window_position;
    }
    const core::Vec2d result{
        initial_window_position.x + current_pointer_position.x - initial_pointer_position.x,
        initial_window_position.y + current_pointer_position.y - initial_pointer_position.y};
    return result.finite() ? result : initial_window_position;
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
    case Workspace::uv: return "uv";
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

bool workspace_supports_scene_object_translation(Workspace workspace) noexcept {
    return workspace == Workspace::model || workspace == Workspace::condition;
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

const char* workbench_pane_name(WorkbenchPane pane) noexcept {
    switch (pane) {
    case WorkbenchPane::navigation: return "pane_navigation";
    case WorkbenchPane::workspace_selector: return "pane_workspace_selector";
    case WorkbenchPane::agent_task_strip: return "pane_agent_task_strip";
    case WorkbenchPane::toolbar: return "pane_toolbar";
    case WorkbenchPane::tool_rack: return "pane_tool_rack";
    case WorkbenchPane::work_surface: return "pane_work_surface";
    case WorkbenchPane::operation_ledger: return "pane_operation_ledger";
    case WorkbenchPane::instrument_bay: return "pane_instrument_bay";
    case WorkbenchPane::ai_context: return "pane_ai_context";
    }
    return "pane_unknown";
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

core::Result<EngineeringPathProjection> project_engineering_path_xz(
    const application::EngineeringAuthoringView::Path& path,
    const application::EngineeringAuthoringView::Workcell* workcell) {
    EngineeringPathProjection projection;
    projection.source_sample_count = path.samples.size();
    if (path.samples.empty()) {
        return core::Result<EngineeringPathProjection>::success(std::move(projection));
    }

    projection.minimum_x = std::numeric_limits<double>::infinity();
    projection.maximum_x = -std::numeric_limits<double>::infinity();
    projection.minimum_z = std::numeric_limits<double>::infinity();
    projection.maximum_z = -std::numeric_limits<double>::infinity();
    const auto include_position = [&projection](core::Vec3d position) {
        if (!position.finite()) return false;
        projection.minimum_x = std::min(projection.minimum_x, position.x);
        projection.maximum_x = std::max(projection.maximum_x, position.x);
        projection.minimum_z = std::min(projection.minimum_z, position.z);
        projection.maximum_z = std::max(projection.maximum_z, position.z);
        return true;
    };
    for (const auto& sample : path.samples) {
        const auto& position = sample.tool_pose.translation;
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) || sample.status.empty()) {
            return core::Result<EngineeringPathProjection>::failure(validation(
                "engineering path projection contains an invalid sample"));
        }
        static_cast<void>(include_position(position));
    }
    if (workcell != nullptr) {
        for (const auto& entity : workcell->entities) {
            if (!entity.enabled) continue;
            if (entity.id == 0U || !entity.transform.finite() ||
                !include_position(entity.transform.translation)) {
                return core::Result<EngineeringPathProjection>::failure(validation(
                    "engineering workcell marker is invalid"));
            }
            projection.workcell_markers.push_back(EngineeringPathProjectionMarker{
                entity.id, entity.kind, entity.name, entity.transform.translation,
                entity.reference_frame});
        }
        for (const auto& region : workcell->keep_outs) {
            if (!region.enabled) continue;
            if (region.id == 0U || !region.transform.finite() ||
                !region.dimensions.finite() || region.dimensions.x <= 0.0 ||
                region.dimensions.y <= 0.0 || region.dimensions.z <= 0.0) {
                return core::Result<EngineeringPathProjection>::failure(validation(
                    "engineering workcell keep-out projection is invalid"));
            }
            EngineeringPathProjectionKeepOut projected_region;
            projected_region.id = region.id;
            projected_region.kind = region.kind;
            projected_region.purpose = region.purpose;
            projected_region.minimum_x = std::numeric_limits<double>::infinity();
            projected_region.maximum_x = -std::numeric_limits<double>::infinity();
            projected_region.minimum_z = std::numeric_limits<double>::infinity();
            projected_region.maximum_z = -std::numeric_limits<double>::infinity();
            for (std::uint32_t corner = 0U; corner < 8U; ++corner) {
                const core::Vec3d local{
                    (corner & 1U) == 0U ? -region.dimensions.x * 0.5
                                        : region.dimensions.x * 0.5,
                    (corner & 2U) == 0U ? -region.dimensions.y * 0.5
                                        : region.dimensions.y * 0.5,
                    (corner & 4U) == 0U ? -region.dimensions.z * 0.5
                                        : region.dimensions.z * 0.5};
                const auto world = region.transform.translation +
                    region.transform.rotation.rotate(
                        core::componentwise_multiply(region.transform.scale, local));
                if (!world.finite()) {
                    return core::Result<EngineeringPathProjection>::failure(validation(
                        "engineering workcell keep-out exceeds the projection range"));
                }
                projected_region.minimum_x = std::min(projected_region.minimum_x, world.x);
                projected_region.maximum_x = std::max(projected_region.maximum_x, world.x);
                projected_region.minimum_z = std::min(projected_region.minimum_z, world.z);
                projected_region.maximum_z = std::max(projected_region.maximum_z, world.z);
            }
            if (!include_position({projected_region.minimum_x, 0.0,
                    projected_region.minimum_z}) ||
                !include_position({projected_region.maximum_x, 0.0,
                    projected_region.maximum_z})) {
                return core::Result<EngineeringPathProjection>::failure(validation(
                    "engineering workcell keep-out exceeds the projection range"));
            }
            projection.keep_outs.push_back(std::move(projected_region));
        }
    }
    if (!std::isfinite(projection.maximum_x - projection.minimum_x) ||
        !std::isfinite(projection.maximum_z - projection.minimum_z)) {
        return core::Result<EngineeringPathProjection>::failure(validation(
            "engineering path coordinates exceed the projection range"));
    }

    const auto projected_count = std::min(
        path.samples.size(), kMaxEngineeringPathProjectionPoints);
    projection.points.reserve(projected_count);
    for (std::size_t index = 0U; index < projected_count; ++index) {
        const auto source_index = projected_count == path.samples.size()
            ? index
            : (index * (path.samples.size() - 1U)) / (projected_count - 1U);
        const auto& sample = path.samples[source_index];
        projection.points.push_back(EngineeringPathProjectionPoint{
            sample.sequence,
            sample.tool_pose.translation,
            sample.target_id,
            sample.status});
    }
    projection.omitted_sample_count = path.samples.size() - projected_count;
    return core::Result<EngineeringPathProjection>::success(std::move(projection));
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

ProjectedEdgePickResult resolve_projected_edge_pick(
    std::span<const ProjectedEdgePickCandidate> candidates,
    core::Vec2d screen_position,
    double hit_radius_pixels,
    double ambiguity_margin_pixels) {
    const auto finite_point = [](core::Vec2d point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    const double radius_squared = hit_radius_pixels * hit_radius_pixels;
    if (!finite_point(screen_position) || !std::isfinite(hit_radius_pixels) ||
        hit_radius_pixels <= 0.0 || !std::isfinite(radius_squared) ||
        !std::isfinite(ambiguity_margin_pixels) || ambiguity_margin_pixels < 0.0) {
        return {ProjectedEdgePickStatus::invalid_input, {}, {}, 0.0};
    }

    using EdgeKey = std::pair<std::uint64_t, std::uint64_t>;
    std::map<EdgeKey, std::pair<const ProjectedEdgePickCandidate*, double>> unique_edges;
    for (const auto& candidate : candidates) {
        if (candidate.object.value == 0U || candidate.edge.value == 0U ||
            !finite_point(candidate.first) || !finite_point(candidate.second)) {
            return {ProjectedEdgePickStatus::invalid_input, {}, {}, 0.0};
        }
        const double dx = candidate.second.x - candidate.first.x;
        const double dy = candidate.second.y - candidate.first.y;
        const double length_squared = dx * dx + dy * dy;
        if (!std::isfinite(length_squared)) {
            return {ProjectedEdgePickStatus::invalid_input, {}, {}, 0.0};
        }
        double projection = 0.0;
        if (length_squared > 1e-12) {
            projection = std::clamp(
                ((screen_position.x - candidate.first.x) * dx +
                 (screen_position.y - candidate.first.y) * dy) / length_squared,
                0.0, 1.0);
        }
        const double closest_x = candidate.first.x + dx * projection;
        const double closest_y = candidate.first.y + dy * projection;
        const double distance_x = screen_position.x - closest_x;
        const double distance_y = screen_position.y - closest_y;
        const double distance_squared = distance_x * distance_x + distance_y * distance_y;
        if (!std::isfinite(distance_squared)) {
            return {ProjectedEdgePickStatus::invalid_input, {}, {}, 0.0};
        }
        if (distance_squared >= radius_squared) continue;

        const EdgeKey key{candidate.object.value, candidate.edge.value};
        const auto [entry, inserted] = unique_edges.emplace(
            key, std::pair{&candidate, distance_squared});
        if (!inserted && distance_squared < entry->second.second) {
            entry->second = {&candidate, distance_squared};
        }
    }

    const ProjectedEdgePickCandidate* closest = nullptr;
    double best_distance_squared = radius_squared;
    double second_distance_squared = radius_squared;
    for (const auto& [key, candidate] : unique_edges) {
        static_cast<void>(key);
        if (candidate.second < best_distance_squared) {
            second_distance_squared = best_distance_squared;
            best_distance_squared = candidate.second;
            closest = candidate.first;
        } else if (candidate.second < second_distance_squared) {
            second_distance_squared = candidate.second;
        }
    }
    if (closest == nullptr) {
        return {ProjectedEdgePickStatus::miss, {}, {}, 0.0};
    }
    if (second_distance_squared < radius_squared &&
        std::sqrt(second_distance_squared) - std::sqrt(best_distance_squared) <=
            ambiguity_margin_pixels) {
        return {ProjectedEdgePickStatus::ambiguous, {}, {},
                std::sqrt(best_distance_squared)};
    }
    return {ProjectedEdgePickStatus::hit, closest->object, closest->edge,
            std::sqrt(best_distance_squared)};
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

core::Result<std::vector<std::vector<core::Vec3d>>> resolve_decal_projection_outline(
    const production::DecalPlacement& placement,
    const production::DecalProjection& projection,
    std::uint64_t current_mesh_asset,
    core::Revision current_mesh_revision,
    const geometry::CompiledMesh& source_mesh,
    const geometry::CompiledMesh& visible_mesh) {
    if (auto result = placement.validate(); !result) {
        return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(
            result.error().with_context("decal outline placement"));
    }
    if (placement.id != projection.placement_id ||
        placement.revision != projection.placement_revision ||
        placement.target_mesh_asset != current_mesh_asset ||
        placement.target_mesh_asset != projection.target_mesh_asset ||
        placement.target_mesh_revision != current_mesh_revision ||
        placement.target_mesh_revision != projection.target_mesh_revision ||
        placement.target_mesh_digest != projection.target_mesh_digest ||
        placement.material_id != projection.material_id ||
        source_mesh.source_revision != current_mesh_revision ||
        visible_mesh.source_revision != current_mesh_revision) {
        return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(stale(
            "decal outline source, placement, or projection identity changed"));
    }
    if (!source_mesh.valid() || !visible_mesh.valid()) {
        return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(validation(
            "decal outline source mesh is invalid"));
    }
    bool same_positions = source_mesh.positions.size() == visible_mesh.positions.size();
    for (std::size_t index = 0U; same_positions && index < source_mesh.positions.size(); ++index) {
        const core::Vec3d& source = source_mesh.positions[index];
        const core::Vec3d& visible = visible_mesh.positions[index];
        same_positions = source.x == visible.x && source.y == visible.y && source.z == visible.z;
    }
    if (!same_positions || source_mesh.indices != visible_mesh.indices ||
        source_mesh.vertex_ids != visible_mesh.vertex_ids ||
        source_mesh.triangle_faces != visible_mesh.triangle_faces) {
        return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(stale(
            "visible viewport geometry differs from the exact decal source mesh"));
    }
    const std::size_t triangle_count = source_mesh.indices.size() / 3U;
    if (triangle_count > kMaxDecalOutlineSourceTriangles ||
        projection.polygons.size() > kMaxDecalOutlinePolygons) {
        return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(unsupported(
            "decal outline exceeds the native presentation bound"));
    }

    std::map<geometry::FaceId, std::vector<std::size_t>> face_triangles;
    for (std::size_t triangle = 0U; triangle < triangle_count; ++triangle) {
        face_triangles[source_mesh.triangle_faces[triangle]].push_back(triangle);
    }

    std::vector<std::vector<core::Vec3d>> outlines;
    outlines.reserve(projection.polygons.size());
    std::set<std::pair<geometry::FaceId, std::uint32_t>> seen_source_triangles;
    constexpr double barycentric_tolerance = 1.0e-9;
    for (const production::DecalProjectedPolygon& polygon : projection.polygons) {
        if (!polygon.source_face || polygon.vertices.size() < 3U ||
            polygon.vertices.size() > kMaxDecalOutlineVerticesPerPolygon ||
            !seen_source_triangles.emplace(polygon.source_face, polygon.source_triangle).second) {
            return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(validation(
                "decal projection contains a malformed or duplicate source polygon"));
        }
        const auto face = face_triangles.find(polygon.source_face);
        if (face == face_triangles.end() || polygon.source_triangle >= face->second.size()) {
            return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(stale(
                "decal projection source face or triangle ordinal is absent"));
        }
        const std::size_t triangle = face->second[polygon.source_triangle];
        std::array<core::Vec3d, 3U> source_positions{};
        for (std::size_t corner = 0U; corner < source_positions.size(); ++corner) {
            const std::uint32_t index = source_mesh.indices[triangle * 3U + corner];
            source_positions[corner] = source_mesh.positions[index];
        }

        std::vector<core::Vec3d> outline;
        outline.reserve(polygon.vertices.size());
        for (const production::DecalProjectedVertex& vertex : polygon.vertices) {
            if (!vertex.uv.finite() || vertex.uv.x < 0.0 || vertex.uv.x > 1.0 ||
                vertex.uv.y < 0.0 || vertex.uv.y > 1.0) {
                return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(validation(
                    "decal projection contains an invalid UV coordinate"));
            }
            double total = 0.0;
            std::array<double, 3U> weights = vertex.source_barycentric;
            for (const double weight : weights) {
                if (!std::isfinite(weight) || weight < -barycentric_tolerance ||
                    weight > 1.0 + barycentric_tolerance) {
                    return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(validation(
                        "decal projection contains an invalid source barycentric coordinate"));
                }
                total += weight;
            }
            if (!std::isfinite(total) || std::abs(total - 1.0) > 1.0e-8) {
                return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(validation(
                    "decal projection barycentric coordinates do not sum to one"));
            }
            for (double& weight : weights) weight = std::clamp(weight, 0.0, 1.0) / total;
            const core::Vec3d point = source_positions[0U] * weights[0U] +
                source_positions[1U] * weights[1U] + source_positions[2U] * weights[2U];
            if (!point.finite()) {
                return core::Result<std::vector<std::vector<core::Vec3d>>>::failure(validation(
                    "decal projection resolved to a non-finite source position"));
            }
            outline.push_back(point);
        }
        outlines.push_back(std::move(outline));
    }
    return core::Result<std::vector<std::vector<core::Vec3d>>>::success(std::move(outlines));
}

core::Result<ProjectedSurfaceHitResolution> resolve_frontmost_viewport_surface(
    std::span<const SculptViewportTriangle> triangles,
    core::Vec2d screen_position) {
    if (!screen_position.finite()) {
        return core::Result<ProjectedSurfaceHitResolution>::failure(invalid(
            "viewport surface pick requires a finite screen point"));
    }

    std::optional<ProjectedSurfaceHitResolution> best;
    std::array<std::uint64_t, 5U> best_identity{};
    bool ambiguous_source_tie = false;
    for (const SculptViewportTriangle& triangle : triangles) {
        if (triangle.object.value == 0U || triangle.face.value == 0U ||
            triangle.vertices[0].value == 0U || triangle.vertices[1].value == 0U ||
            triangle.vertices[2].value == 0U ||
            triangle.vertices[0] == triangle.vertices[1] ||
            triangle.vertices[0] == triangle.vertices[2] ||
            triangle.vertices[1] == triangle.vertices[2]) {
            return core::Result<ProjectedSurfaceHitResolution>::failure(validation(
                "viewport surface triangle has invalid stable source identity"));
        }
        for (std::size_t index = 0U; index < triangle.screen_positions.size(); ++index) {
            if (!triangle.screen_positions[index].finite() ||
                !triangle.local_positions[index].finite() ||
                !std::isfinite(triangle.camera_depth[index])) {
                return core::Result<ProjectedSurfaceHitResolution>::failure(validation(
                    "viewport surface triangle contains non-finite projected source data"));
            }
        }

        const core::Vec2d& a = triangle.screen_positions[0U];
        const core::Vec2d& b = triangle.screen_positions[1U];
        const core::Vec2d& c = triangle.screen_positions[2U];
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

        std::array<double, 3U> weights{
            std::clamp(first, 0.0, 1.0),
            std::clamp(second, 0.0, 1.0),
            std::clamp(third, 0.0, 1.0)};
        const double total = weights[0U] + weights[1U] + weights[2U];
        if (!std::isfinite(total) || total <= 0.0) {
            return core::Result<ProjectedSurfaceHitResolution>::failure(validation(
                "viewport surface hit produced invalid barycentric coordinates"));
        }
        for (double& weight : weights) weight /= total;
        const double depth = triangle.camera_depth[0U] * weights[0U] +
            triangle.camera_depth[1U] * weights[1U] +
            triangle.camera_depth[2U] * weights[2U];
        if (!std::isfinite(depth)) {
            return core::Result<ProjectedSurfaceHitResolution>::failure(validation(
                "viewport surface hit produced non-finite camera depth"));
        }
        const double depth_tolerance = 1.0e-12 * std::max({
            1.0, std::abs(depth), best.has_value() ? std::abs(best->camera_depth) : 0.0});
        const bool nearer = !best.has_value() ||
            depth > best->camera_depth + depth_tolerance;
        if (nearer) {
            core::Vec3d position{};
            for (std::size_t index = 0U; index < weights.size(); ++index) {
                position = position + triangle.local_positions[index] * weights[index];
            }
            if (!position.finite()) {
                return core::Result<ProjectedSurfaceHitResolution>::failure(validation(
                    "viewport surface hit produced a non-finite source position"));
            }
            production::SculptSurfaceAnchor anchor{
                triangle.face, triangle.vertices, weights};
            if (auto result = anchor.validate(); !result) {
                return core::Result<ProjectedSurfaceHitResolution>::failure(result.error());
            }
            best = ProjectedSurfaceHitResolution{
                ProjectedSurfaceHitStatus::hit,
                triangle.object,
                production::SculptStrokeSample{
                    position, 1.0, std::move(anchor), std::nullopt},
                depth};
            best_identity = {
                triangle.object.value,
                triangle.face.value,
                triangle.vertices[0U].value,
                triangle.vertices[1U].value,
                triangle.vertices[2U].value};
            ambiguous_source_tie = false;
            continue;
        }

        if (std::abs(depth - best->camera_depth) > depth_tolerance) continue;
        if (triangle.object != best->object ||
            triangle.face.value != best_identity[1U]) {
            ambiguous_source_tie = true;
            continue;
        }
        const std::array<std::uint64_t, 5U> identity{
            triangle.object.value,
            triangle.face.value,
            triangle.vertices[0U].value,
            triangle.vertices[1U].value,
            triangle.vertices[2U].value};
        if (identity < best_identity) {
            core::Vec3d position{};
            for (std::size_t index = 0U; index < weights.size(); ++index) {
                position = position + triangle.local_positions[index] * weights[index];
            }
            if (!position.finite()) {
                return core::Result<ProjectedSurfaceHitResolution>::failure(validation(
                    "viewport surface hit produced a non-finite source position"));
            }
            production::SculptSurfaceAnchor anchor{
                triangle.face, triangle.vertices, weights};
            if (auto result = anchor.validate(); !result) {
                return core::Result<ProjectedSurfaceHitResolution>::failure(result.error());
            }
            best->sample = production::SculptStrokeSample{
                position, 1.0, std::move(anchor), std::nullopt};
            best_identity = identity;
        }
    }

    if (!best.has_value()) {
        return core::Result<ProjectedSurfaceHitResolution>::success(
            ProjectedSurfaceHitResolution{});
    }
    if (ambiguous_source_tie) {
        best->status = ProjectedSurfaceHitStatus::ambiguous;
        best->object = {};
        best->sample = {};
        return core::Result<ProjectedSurfaceHitResolution>::success(std::move(*best));
    }
    return core::Result<ProjectedSurfaceHitResolution>::success(std::move(*best));
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

core::Result<std::vector<core::Vec3d>> sample_knife_viewport_stroke(
    std::span<const SculptViewportTriangle> triangles,
    scene::ObjectId object,
    geometry::FaceId face,
    std::span<const core::Vec2d> screen_path,
    double endpoint_snap_radius_pixels) {
    if (!object || !face || screen_path.size() < 2U ||
        screen_path.size() > kMaxKnifeViewportPathPoints ||
        !std::isfinite(endpoint_snap_radius_pixels) ||
        endpoint_snap_radius_pixels <= 0.0 || endpoint_snap_radius_pixels > 64.0) {
        return core::Result<std::vector<core::Vec3d>>::failure(invalid(
            "knife viewport stroke identity, path, or endpoint radius is outside its bounds"));
    }
    for (const core::Vec2d point : screen_path) {
        if (!point.finite()) {
            return core::Result<std::vector<core::Vec3d>>::failure(invalid(
                "knife viewport stroke contains a non-finite screen point"));
        }
    }

    struct FaceVertexPoint {
        core::Vec2d screen;
        core::Vec3d local;
    };
    std::map<geometry::VertexId, FaceVertexPoint> face_vertices;
    std::vector<SculptViewportTriangle> face_triangles;
    std::map<std::pair<geometry::VertexId, geometry::VertexId>, std::size_t>
        triangle_edge_uses;
    for (const SculptViewportTriangle& triangle : triangles) {
        if (triangle.object != object || triangle.face != face) continue;
        if (triangle.face.value == 0U) {
            return core::Result<std::vector<core::Vec3d>>::failure(validation(
                "knife viewport triangle has an invalid source face identity"));
        }
        face_triangles.push_back(triangle);
        for (std::size_t corner = 0U; corner < triangle.vertices.size(); ++corner) {
            const geometry::VertexId vertex = triangle.vertices[corner];
            const FaceVertexPoint point{triangle.screen_positions[corner],
                triangle.local_positions[corner]};
            if (!vertex || !point.screen.finite() || !point.local.finite() ||
                !std::isfinite(triangle.camera_depth[corner])) {
                return core::Result<std::vector<core::Vec3d>>::failure(validation(
                    "knife viewport triangle contains invalid source vertex data"));
            }
            const auto [existing, inserted] = face_vertices.emplace(vertex, point);
            if (!inserted &&
                (existing->second.screen.x != point.screen.x ||
                 existing->second.screen.y != point.screen.y ||
                 existing->second.local.x != point.local.x ||
                 existing->second.local.y != point.local.y ||
                 existing->second.local.z != point.local.z)) {
                return core::Result<std::vector<core::Vec3d>>::failure(validation(
                    "knife viewport triangles disagree about a shared source vertex"));
            }
        }
        const std::array<std::pair<std::size_t, std::size_t>, 3U> triangle_edges{{
            {0U, 1U}, {1U, 2U}, {2U, 0U}}};
        for (const auto [first_index, second_index] : triangle_edges) {
            const geometry::VertexId first = triangle.vertices[first_index];
            const geometry::VertexId second = triangle.vertices[second_index];
            const auto edge = first < second
                ? std::pair{first, second} : std::pair{second, first};
            ++triangle_edge_uses[edge];
        }
    }
    if (face_triangles.empty() || face_vertices.size() < 3U) {
        return core::Result<std::vector<core::Vec3d>>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "knife viewport stroke does not name a visible source face"));
    }
    if (std::any_of(triangle_edge_uses.begin(), triangle_edge_uses.end(),
            [](const auto& item) { return item.second > 2U; })) {
        return core::Result<std::vector<core::Vec3d>>::failure(validation(
            "knife viewport triangulation has a non-manifold edge incidence"));
    }

    const double edge_snap_radius = std::min(endpoint_snap_radius_pixels, 4.0);
    const double snap_radius_squared = edge_snap_radius * edge_snap_radius;
    const double vertex_snap_radius = std::min(endpoint_snap_radius_pixels, 3.0);
    const double vertex_snap_radius_squared = vertex_snap_radius * vertex_snap_radius;
    struct SnappedEndpoint {
        core::Vec3d local;
    };
    const auto snap_endpoint = [&](core::Vec2d screen)
        -> std::optional<SnappedEndpoint> {
        std::optional<geometry::VertexId> best_vertex;
        double best_distance = vertex_snap_radius_squared;
        for (const auto& [vertex, point] : face_vertices) {
            const double dx = point.screen.x - screen.x;
            const double dy = point.screen.y - screen.y;
            const double distance = dx * dx + dy * dy;
            if (!std::isfinite(distance) || distance > vertex_snap_radius_squared) continue;
            if (!best_vertex.has_value() || distance < best_distance ||
                (distance == best_distance && vertex < *best_vertex)) {
                best_vertex = vertex;
                best_distance = distance;
            }
        }
        if (best_vertex.has_value()) {
            return SnappedEndpoint{face_vertices.at(*best_vertex).local};
        }

        std::optional<SnappedEndpoint> best_edge_point;
        std::optional<std::pair<geometry::VertexId, geometry::VertexId>> best_edge;
        double best_edge_distance = snap_radius_squared;
        for (const auto& [edge, use_count] : triangle_edge_uses) {
            if (use_count != 1U) continue;
            const FaceVertexPoint& first = face_vertices.at(edge.first);
            const FaceVertexPoint& second = face_vertices.at(edge.second);
            const double dx = second.screen.x - first.screen.x;
            const double dy = second.screen.y - first.screen.y;
            const double length_squared = dx * dx + dy * dy;
            if (!std::isfinite(length_squared) || length_squared <= 1e-24) continue;
            const double factor = std::clamp(
                ((screen.x - first.screen.x) * dx +
                 (screen.y - first.screen.y) * dy) / length_squared,
                0.0, 1.0);
            if (!std::isfinite(factor) || factor <= 1e-9 || factor >= 1.0 - 1e-9) continue;
            const double projected_x = first.screen.x + dx * factor;
            const double projected_y = first.screen.y + dy * factor;
            const double error_x = projected_x - screen.x;
            const double error_y = projected_y - screen.y;
            const double distance = error_x * error_x + error_y * error_y;
            if (!std::isfinite(distance) || distance > snap_radius_squared) continue;
            if (!best_edge_point.has_value() || distance < best_edge_distance ||
                (distance == best_edge_distance && edge < *best_edge)) {
                best_edge = edge;
                best_edge_distance = distance;
                best_edge_point = SnappedEndpoint{
                    first.local + (second.local - first.local) * factor};
            }
        }
        return best_edge_point;
    };

    const auto first_endpoint = snap_endpoint(screen_path.front());
    const auto last_endpoint = snap_endpoint(screen_path.back());
    if (!first_endpoint.has_value() || !last_endpoint.has_value()) {
        return core::Result<std::vector<core::Vec3d>>::failure(core::Diagnostic(
            core::ErrorCode::unsupported,
            "knife stroke endpoints must be close to source-face vertices or boundary edges"));
    }
    if ((first_endpoint->local - last_endpoint->local).length_squared() <= 1e-24) {
        return core::Result<std::vector<core::Vec3d>>::failure(invalid(
            "knife stroke endpoints must snap to distinct source-face boundary positions"));
    }

    std::vector<core::Vec3d> result;
    result.reserve(screen_path.size());
    result.push_back(first_endpoint->local);
    for (std::size_t index = 1U; index + 1U < screen_path.size(); ++index) {
        const auto sample = sample_sculpt_viewport_surface(
            face_triangles, object, screen_path[index]);
        if (!sample) {
            return core::Result<std::vector<core::Vec3d>>::failure(sample.error().with_context(
                "knife viewport stroke must remain on its selected source face"));
        }
        if ((sample.value().position - result.back()).length_squared() <= 1.0e-24) {
            continue;
        }
        result.push_back(sample.value().position);
    }
    const core::Vec3d last_position = last_endpoint->local;
    if ((last_position - result.back()).length_squared() <= 1.0e-24) {
        return core::Result<std::vector<core::Vec3d>>::failure(invalid(
            "knife stroke contains a zero-length final segment"));
    }
    result.push_back(last_position);
    if (result.size() > kMaxKnifeViewportPathPoints) {
        return core::Result<std::vector<core::Vec3d>>::failure(invalid(
            "knife viewport stroke exceeds its path-point limit"));
    }
    return core::Result<std::vector<core::Vec3d>>::success(std::move(result));
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

core::Result<editor::MaterialGraphAuthoringReview>
UiController::begin_material_graph_review(std::string identity) const {
    const auto snapshot = session_.snapshot();
    auto current = session_.material_graph_source(identity);
    std::optional<project::MaterialGraph> current_graph;
    if (current) {
        current_graph = std::move(current.value());
    } else if (current.error().code != core::ErrorCode::not_found) {
        return core::Result<editor::MaterialGraphAuthoringReview>::failure(current.error());
    }
    return editor::MaterialGraphAuthoringReview::begin(
        snapshot.project_revision, std::move(identity), std::move(current_graph));
}

core::Result<application::DispatchReceipt> UiController::commit_material_graph_review(
    editor::MaterialGraphAuthoringReview& review) {
    if (!review.candidate().has_value()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "material graph review has no proposal to commit"));
    }
    const auto snapshot = session_.snapshot();
    auto current = session_.material_graph_source(review.identity());
    std::optional<project::MaterialGraph> current_graph;
    if (current) {
        current_graph = std::move(current.value());
    } else if (current.error().code != core::ErrorCode::not_found) {
        return core::Result<application::DispatchReceipt>::failure(current.error());
    }
    if (auto result = review.validate_commit(snapshot.project_revision, current_graph); !result) {
        record_ui_problem(result.error().with_context("material graph review commit"));
        return core::Result<application::DispatchReceipt>::failure(result.error());
    }
    application::SetMaterialGraphAction action;
    action.expected_project_revision = review.base_project_revision();
    action.expected_graph_revision = review.expected_graph_revision();
    action.expected_graph_digest = review.expected_graph_digest();
    action.graph = *review.candidate();
    auto result = dispatch(application::ApplicationAction{std::move(action)});
    if (result) review.cancel();
    return result;
}

core::Result<production::DecalProjection> UiController::preview_decal_placement(
    const production::DecalPlacement& placement,
    core::Revision expected_project_revision) const {
    return session_.decal_placement_preview(placement, expected_project_revision);
}

core::Result<geometry::CompiledMesh> UiController::decal_placement_source_mesh(
    const production::DecalPlacement& placement,
    core::Revision expected_project_revision) const {
    return session_.decal_placement_source_mesh(placement, expected_project_revision);
}

core::Result<production::DecalPlacement> UiController::make_decal_placement(
    scene::ObjectId target_object,
    std::uint64_t material_id,
    core::Vec3d center,
    core::Vec3d normal,
    core::Vec3d tangent,
    double width,
    double height,
    double depth,
    double minimum_normal_dot,
    core::Revision expected_project_revision) const {
    return session_.make_decal_placement(
        target_object, material_id, center, normal, tangent, width, height, depth,
        minimum_normal_dot, expected_project_revision);
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
    if (agent_named_target_clarification_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "resolve or clear the exact-name target clarification before editing the plan"));
    }
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

core::Result<void> UiController::compile_agent_goal_sequence(std::string goal) {
    auto named_target_resolution = std::exchange(
        agent_named_target_resolution_, std::nullopt);
    if (agent_named_target_clarification_.has_value() &&
        !named_target_resolution.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "resolve or clear the pending exact-name target clarification first"));
    }
    if (named_target_resolution.has_value() &&
        named_target_resolution->pending.goal != goal) {
        return core::Result<void>::failure(stale(
            "the goal changed while an exact-name target choice was pending"));
    }
    if (agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "a loaded checkpoint plan cannot be replaced; resume or clear it first"));
    }
    if (agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "clear the current AgentTask before compiling a new objective"));
    }
    if (!agent_plan_draft_.empty()) {
        return core::Result<void>::failure(invalid_state(
            "clear the captured plan before compiling a goal sequence"));
    }
    if (std::any_of(agent_clarifications_draft_.begin(),
            agent_clarifications_draft_.end(), [](const auto& question) {
                return question.human_receipt.has_value();
            })) {
        return core::Result<void>::failure(invalid_state(
            "clear answered clarifications before compiling a different objective"));
    }

    const auto parsed_goal = agent::parse_agent_goal(goal);
    if (!parsed_goal) return core::Result<void>::failure(parsed_goal.error());

    const auto source_snapshot = session_.snapshot();
    if (named_target_resolution.has_value() &&
        (source_snapshot.project_generation !=
                named_target_resolution->pending.source_generation ||
            source_snapshot.project_revision !=
                named_target_resolution->pending.source_revision)) {
        return core::Result<void>::failure(stale(
            "the source project changed while an exact-name target choice was pending"));
    }
    if (agent_plan_base_revision_.has_value() &&
        *agent_plan_base_revision_ != source_snapshot.project_revision) {
        return core::Result<void>::failure(stale(
            "project changed after the current objective draft was captured"));
    }
    const auto authoring = session_.authoring_context();
    if (!authoring) return core::Result<void>::failure(authoring.error());
    if (authoring.value().project_revision != source_snapshot.project_revision) {
        return core::Result<void>::failure(stale(
            "project changed while the objective target was being captured"));
    }

    std::vector<agent::TargetRef> targets;
    std::vector<agent::SceneNamedObjectFact> named_target_candidates;
    std::optional<scene::ObjectId> selected_named_target;
    const bool object_selection =
        authoring.value().selection_mode == editor::SelectionMode::object;
    const bool named_target = parsed_goal.value().exact_object_name.has_value();
    const bool nearest_spatial_target = parsed_goal.value().nearest_object_scope.has_value();
    const bool condition_tag_target = parsed_goal.value().condition_tag_scope.has_value();
    if (named_target) {
        constexpr std::size_t kMaxNamedTargetQueryScope = 512U;
        if (source_snapshot.objects.empty() ||
            source_snapshot.objects.size() > kMaxNamedTargetQueryScope) {
            return core::Result<void>::failure(invalid_state(
                "exact-name target resolution requires a project with 1 to 512 objects"));
        }
        std::vector<scene::ObjectId> allowed_scope;
        allowed_scope.reserve(source_snapshot.objects.size());
        for (const auto& object : source_snapshot.objects) {
            allowed_scope.push_back(object.object.id);
        }
        std::sort(allowed_scope.begin(), allowed_scope.end());
        if (std::adjacent_find(allowed_scope.begin(), allowed_scope.end()) !=
            allowed_scope.end()) {
            return core::Result<void>::failure(validation(
                "source project contains duplicate object identities"));
        }
        agent::SceneQueryBudget query_budget;
        const auto query = agent::SceneQuery::execute(session_,
            agent::SceneQueryRequest{
                agent::ObjectsByStableNameQuery{
                    *parsed_goal.value().exact_object_name},
                512U},
            query_budget, allowed_scope, source_snapshot.project_revision);
        if (!query) {
            return core::Result<void>::failure(query.error().with_context(
                "resolving the explicit exact-name goal target"));
        }
        if (query.value().source_revision != source_snapshot.project_revision ||
            query.value().truncated || !query.value().diagnostics.empty()) {
            return core::Result<void>::failure(stale(
                "exact-name target query is stale or incomplete"));
        }
        if (query.value().rows.empty()) {
            return core::Result<void>::failure(invalid_state(
                "the exact-name goal target was not found in the current project"));
        }
        named_target_candidates.reserve(query.value().rows.size());
        for (const auto& row : query.value().rows) {
            const auto* fact = std::get_if<agent::SceneNamedObjectFact>(&row);
            if (fact == nullptr ||
                fact->stable_name != *parsed_goal.value().exact_object_name) {
                return core::Result<void>::failure(validation(
                    "exact-name target query returned an unexpected fact"));
            }
            named_target_candidates.push_back(*fact);
        }
        std::sort(named_target_candidates.begin(), named_target_candidates.end(),
            [](const auto& left, const auto& right) {
                return left.object < right.object;
            });
        std::vector<scene::ObjectId> named_target_objects;
        named_target_objects.reserve(named_target_candidates.size());
        for (const auto& candidate : named_target_candidates) {
            const auto object = std::find_if(source_snapshot.objects.begin(),
                source_snapshot.objects.end(), [&candidate](const auto& value) {
                    return value.object.id == candidate.object;
                });
            if (object == source_snapshot.objects.end() ||
                object->object.name != *parsed_goal.value().exact_object_name) {
                return core::Result<void>::failure(stale(
                    "an exact-name candidate changed after the source snapshot was captured"));
            }
            if (object->object.parent.has_value() || object->object.locked) {
                return core::Result<void>::failure(invalid_state(
                    "exact-name target choices must all be unlocked root objects"));
            }
            named_target_objects.push_back(candidate.object);
        }
        if (std::adjacent_find(named_target_objects.begin(),
                named_target_objects.end()) != named_target_objects.end()) {
            return core::Result<void>::failure(validation(
                "exact-name target query returned duplicate object identities"));
        }
        if (named_target_resolution.has_value()) {
            const auto& pending = named_target_resolution->pending;
            std::vector<scene::ObjectId> expected_objects;
            expected_objects.reserve(pending.candidates.size());
            for (const auto& candidate : pending.candidates) {
                expected_objects.push_back(candidate.object);
            }
            if (pending.exact_name != *parsed_goal.value().exact_object_name ||
                named_target_objects != expected_objects ||
                named_target_resolution->selected_candidate_id.empty() ||
                std::none_of(pending.candidates.begin(), pending.candidates.end(),
                    [&named_target_resolution](const auto& candidate) {
                        return candidate.id == named_target_resolution->selected_candidate_id &&
                            candidate.object == named_target_resolution->selected_object;
                    }) ||
                !std::binary_search(named_target_objects.begin(), named_target_objects.end(),
                    named_target_resolution->selected_object)) {
                return core::Result<void>::failure(stale(
                    "the exact-name candidate set changed before the human choice was applied"));
            }
            selected_named_target = named_target_resolution->selected_object;
        } else if (named_target_candidates.size() == 1U) {
            selected_named_target = named_target_candidates.front().object;
        } else {
            constexpr std::size_t kMaxNamedTargetChoices = 16U;
            if (named_target_candidates.size() > kMaxNamedTargetChoices) {
                return core::Result<void>::failure(invalid_state(
                    "the exact-name target has more than 16 matches; narrow the name before compiling"));
            }
            PendingAgentNamedTargetClarification pending;
            pending.goal = goal;
            pending.exact_name = *parsed_goal.value().exact_object_name;
            pending.prompt = "Which object named \"" + pending.exact_name +
                "\" should this goal target?";
            pending.rationale =
                "Multiple unlocked root objects share this exact name. Choose one explicitly; "
                "the other object identities remain read-only alternatives.";
            pending.source_revision = source_snapshot.project_revision;
            pending.source_generation = source_snapshot.project_generation;
            pending.candidates.reserve(named_target_candidates.size());
            for (std::size_t index = 0U; index < named_target_candidates.size(); ++index) {
                const auto& candidate = named_target_candidates[index];
                pending.candidates.push_back(AgentNamedTargetCandidate{
                    "target-choice-" + std::to_string(index + 1U),
                    pending.exact_name + " (object #" +
                        std::to_string(candidate.object.value) + ")",
                    candidate.object});
            }
            agent_goal_ = goal;
            agent_named_target_clarification_ = std::move(pending);
            return core::Result<void>::success();
        }
        const auto object = std::find_if(source_snapshot.objects.begin(),
            source_snapshot.objects.end(), [selected_named_target](const auto& value) {
                return value.object.id == *selected_named_target;
            });
        if (object == source_snapshot.objects.end() ||
            object->object.name != *parsed_goal.value().exact_object_name) {
            return core::Result<void>::failure(stale(
                "the exact-name target changed after the source snapshot was captured"));
        }
        if (object->object.parent.has_value() || object->object.locked) {
            return core::Result<void>::failure(invalid_state(
                "exact-name goal targets must be unlocked root objects"));
        }
        targets.push_back(agent::TargetRef{*selected_named_target, std::monostate{}});
    } else if (nearest_spatial_target) {
        constexpr std::size_t kMaxSpatialTargetQueryScope = 512U;
        if (source_snapshot.objects.empty() ||
            source_snapshot.objects.size() > kMaxSpatialTargetQueryScope) {
            return core::Result<void>::failure(invalid_state(
                "nearest-object target resolution requires a project with 1 to 512 objects"));
        }
        std::vector<scene::ObjectId> allowed_scope;
        allowed_scope.reserve(source_snapshot.objects.size());
        for (const auto& object : source_snapshot.objects) {
            allowed_scope.push_back(object.object.id);
        }
        std::sort(allowed_scope.begin(), allowed_scope.end());
        if (std::adjacent_find(allowed_scope.begin(), allowed_scope.end()) !=
            allowed_scope.end()) {
            return core::Result<void>::failure(validation(
                "source project contains duplicate object identities"));
        }

        agent::SceneQueryBudget query_budget;
        const auto& spatial_scope = *parsed_goal.value().nearest_object_scope;
        const auto anchor_query = agent::SceneQuery::execute(session_,
            agent::SceneQueryRequest{
                agent::ObjectsByStableNameQuery{spatial_scope.anchor_name},
                kMaxSpatialTargetQueryScope},
            query_budget, allowed_scope, source_snapshot.project_revision);
        if (!anchor_query) {
            return core::Result<void>::failure(anchor_query.error().with_context(
                "resolving the named anchor for nearest-object target resolution"));
        }
        if (anchor_query.value().source_revision != source_snapshot.project_revision ||
            anchor_query.value().truncated || !anchor_query.value().diagnostics.empty()) {
            return core::Result<void>::failure(stale(
                "nearest-object anchor query is stale or incomplete"));
        }
        if (anchor_query.value().rows.empty()) {
            return core::Result<void>::failure(invalid_state(
                "the nearest-object scope anchor was not found in the current project"));
        }
        if (anchor_query.value().rows.size() != 1U) {
            return core::Result<void>::failure(invalid_state(
                "nearest-object scope requires a uniquely named anchor"));
        }
        const auto* anchor_fact = std::get_if<agent::SceneNamedObjectFact>(
            &anchor_query.value().rows.front());
        if (anchor_fact == nullptr || anchor_fact->stable_name != spatial_scope.anchor_name) {
            return core::Result<void>::failure(validation(
                "nearest-object anchor query returned an unexpected fact"));
        }
        const auto anchor_object = std::find_if(source_snapshot.objects.begin(),
            source_snapshot.objects.end(), [anchor_fact](const auto& object) {
                return object.object.id == anchor_fact->object;
            });
        if (anchor_object == source_snapshot.objects.end() ||
            anchor_object->object.name != spatial_scope.anchor_name) {
            return core::Result<void>::failure(stale(
                "nearest-object anchor changed after the source snapshot was captured"));
        }
        const auto* anchor_instance = source_snapshot.viewport.scene.find(anchor_fact->object);
        if (anchor_instance == nullptr ||
            !anchor_instance->world_transform.translation.finite()) {
            return core::Result<void>::failure(stale(
                "nearest-object anchor has no current finite world origin"));
        }
        const auto spatial_query = agent::SceneQuery::execute(session_,
            agent::SceneQueryRequest{
                agent::ObjectsWithinDistanceQuery{
                    anchor_instance->world_transform.translation,
                    spatial_scope.radius_world_units},
                kMaxSpatialTargetQueryScope},
            query_budget, allowed_scope, source_snapshot.project_revision);
        if (!spatial_query) {
            return core::Result<void>::failure(spatial_query.error().with_context(
                "resolving the nearest object around the named anchor"));
        }
        if (spatial_query.value().source_revision != source_snapshot.project_revision ||
            spatial_query.value().truncated || !spatial_query.value().diagnostics.empty()) {
            return core::Result<void>::failure(stale(
                "nearest-object spatial query is stale or incomplete"));
        }
        constexpr double kDistanceTieTolerance = 1e-9;
        std::optional<agent::SceneSpatialObjectFact> nearest_candidate;
        bool nearest_is_ambiguous = false;
        for (const auto& row : spatial_query.value().rows) {
            const auto* fact = std::get_if<agent::SceneSpatialObjectFact>(&row);
            if (fact == nullptr || !fact->object || !fact->world_origin.finite() ||
                !std::isfinite(fact->distance) || fact->distance < 0.0 ||
                fact->distance > spatial_scope.radius_world_units) {
                return core::Result<void>::failure(validation(
                    "nearest-object spatial query returned an invalid fact"));
            }
            if (fact->object == anchor_fact->object) continue;
            if (!nearest_candidate.has_value() ||
                fact->distance < nearest_candidate->distance - kDistanceTieTolerance) {
                nearest_candidate = *fact;
                nearest_is_ambiguous = false;
            } else if (std::abs(fact->distance - nearest_candidate->distance) <=
                kDistanceTieTolerance) {
                nearest_is_ambiguous = true;
            }
        }
        if (!nearest_candidate.has_value()) {
            return core::Result<void>::failure(invalid_state(
                "no other object origin lies within the nearest-object target radius"));
        }
        if (nearest_is_ambiguous) {
            return core::Result<void>::failure(invalid_state(
                "nearest-object target is tied; use an exact name or select one object"));
        }
        const auto target_object = std::find_if(source_snapshot.objects.begin(),
            source_snapshot.objects.end(), [&nearest_candidate](const auto& object) {
                return object.object.id == nearest_candidate->object;
            });
        if (target_object == source_snapshot.objects.end()) {
            return core::Result<void>::failure(stale(
                "nearest-object target changed after the source snapshot was captured"));
        }
        if (target_object->object.parent.has_value() || target_object->object.locked) {
            return core::Result<void>::failure(invalid_state(
                "nearest-object target must be an unlocked root object; no farther object was substituted"));
        }
        targets.push_back(agent::TargetRef{nearest_candidate->object, std::monostate{}});
    } else if (condition_tag_target) {
        constexpr std::size_t kMaxConditionTagTargetQueryScope = 512U;
        if (source_snapshot.objects.empty() ||
            source_snapshot.objects.size() > kMaxConditionTagTargetQueryScope) {
            return core::Result<void>::failure(invalid_state(
                "CONDITION tag target resolution requires a project with 1 to 512 objects"));
        }
        std::vector<scene::ObjectId> allowed_scope;
        allowed_scope.reserve(source_snapshot.objects.size());
        for (const auto& object : source_snapshot.objects) {
            allowed_scope.push_back(object.object.id);
        }
        std::sort(allowed_scope.begin(), allowed_scope.end());
        if (std::adjacent_find(allowed_scope.begin(), allowed_scope.end()) !=
            allowed_scope.end()) {
            return core::Result<void>::failure(validation(
                "source project contains duplicate object identities"));
        }

        agent::SceneQueryBudget query_budget;
        const auto query = agent::SceneQuery::execute(session_,
            agent::SceneQueryRequest{
                agent::ConditionAssetsByTagQuery{
                    *parsed_goal.value().condition_tag_scope},
                kMaxConditionTagTargetQueryScope},
            query_budget, allowed_scope, source_snapshot.project_revision);
        if (!query) {
            return core::Result<void>::failure(query.error().with_context(
                "resolving the explicit CONDITION tag goal scope"));
        }
        if (query.value().source_revision != source_snapshot.project_revision ||
            query.value().truncated || !query.value().diagnostics.empty()) {
            return core::Result<void>::failure(stale(
                "CONDITION tag target query is stale, incomplete, or unavailable"));
        }
        if (query.value().rows.empty()) {
            return core::Result<void>::failure(invalid_state(
                "the CONDITION tag goal scope matched no current project objects"));
        }

        std::set<scene::ObjectId> condition_targets;
        std::set<std::uint64_t> seen_assets;
        for (const auto& row : query.value().rows) {
            const auto* fact = std::get_if<agent::ConditionAssetTagFact>(&row);
            if (fact == nullptr || fact->asset == 0U || !fact->authored_object ||
                fact->source_id == 0U || fact->source_revision.empty() ||
                !seen_assets.insert(fact->asset).second ||
                !std::binary_search(allowed_scope.begin(), allowed_scope.end(),
                    fact->authored_object)) {
                return core::Result<void>::failure(validation(
                    "CONDITION tag query returned a malformed or out-of-scope fact"));
            }
            if (!fact->source_current || !fact->source_reviewed) {
                return core::Result<void>::failure(stale(
                    "CONDITION tag scope requires current, reviewed source evidence"));
            }
            if (!fact->protected_features.empty()) {
                return core::Result<void>::failure(invalid_state(
                    "CONDITION tag target has protected features without a feature-level preservation constraint"));
            }
            condition_targets.insert(fact->authored_object);
        }
        constexpr std::size_t kMaxConditionTagTargets = 16U;
        if (condition_targets.empty() ||
            condition_targets.size() > kMaxConditionTagTargets) {
            return core::Result<void>::failure(invalid_state(
                "CONDITION tag scope must resolve to 1 to 16 unique objects"));
        }
        for (const auto object_id : condition_targets) {
            const auto object = std::find_if(source_snapshot.objects.begin(),
                source_snapshot.objects.end(), [object_id](const auto& value) {
                    return value.object.id == object_id;
                });
            if (object == source_snapshot.objects.end()) {
                return core::Result<void>::failure(stale(
                    "a CONDITION tag target no longer exists in the source project"));
            }
            if (object->object.parent.has_value() || object->object.locked) {
                return core::Result<void>::failure(invalid_state(
                    "CONDITION tag targets must be unlocked root objects; no alternate target was substituted"));
            }
            targets.push_back(agent::TargetRef{object_id, std::monostate{}});
        }
    } else if (object_selection && !authoring.value().objects.empty()) {
        constexpr std::size_t kMaxSelectedObjectTargets = 16U;
        if (authoring.value().objects.size() > kMaxSelectedObjectTargets) {
            return core::Result<void>::failure(invalid_state(
                "goal compilation supports at most 16 explicitly selected root objects"));
        }
        auto selected_objects = authoring.value().objects;
        std::sort(selected_objects.begin(), selected_objects.end());
        for (const auto object_id : selected_objects) {
            const auto object = std::find_if(source_snapshot.objects.begin(),
                source_snapshot.objects.end(), [object_id](const auto& value) {
                    return value.object.id == object_id;
                });
            if (object == source_snapshot.objects.end()) {
                return core::Result<void>::failure(stale(
                    "a selected goal target no longer exists in the source project"));
            }
            if (object->object.parent.has_value() || object->object.locked) {
                return core::Result<void>::failure(invalid_state(
                    "goal compilation requires unlocked root-object targets"));
            }
            targets.push_back(agent::TargetRef{object_id, std::monostate{}});
        }
    } else if (authoring.value().selection_mode == editor::SelectionMode::face &&
        authoring.value().component_object.has_value() &&
        authoring.value().faces.size() == 1U) {
        targets.push_back(agent::TargetRef{
            *authoring.value().component_object, authoring.value().faces.front()});
    } else if (authoring.value().selection_mode == editor::SelectionMode::vertex &&
        authoring.value().component_object.has_value() &&
        authoring.value().vertices.size() == 1U) {
        targets.push_back(agent::TargetRef{
            *authoring.value().component_object, authoring.value().vertices.front()});
    } else {
        return core::Result<void>::failure(invalid_state(
            "select one or more root objects, or exactly one face or vertex, before compiling the goal"));
    }
    for (const auto& target : targets) {
        if (auto valid = target.validate(); !valid) return valid;
    }

    const auto commands = split_agent_goal_sequence(parsed_goal.value().commands);
    if (!commands) return core::Result<void>::failure(commands.error());
    constexpr std::size_t kMaxCompiledSteps = 16U;
    if (targets.empty() || commands.value().size() > kMaxCompiledSteps / targets.size()) {
        return core::Result<void>::failure(invalid_state(
            "expanded goal DAG exceeds the 16-step work budget"));
    }
    auto constraints = compile_objective_constraints(goal, agent_hard_constraints_draft_);
    if (!constraints) return core::Result<void>::failure(constraints.error());

    const std::uint32_t compile_effects =
        ai::effect_mask(ai::CapabilityEffect::geometry) |
        ai::effect_mask(ai::CapabilityEffect::topology) |
        ai::effect_mask(ai::CapabilityEffect::transform) |
        ai::effect_mask(ai::CapabilityEffect::material) |
        ai::effect_mask(ai::CapabilityEffect::uv);
    std::vector<agent::AgentPlanStep> steps;
    steps.reserve(commands.value().size() * targets.size());
    for (std::size_t target_index = 0U; target_index < targets.size(); ++target_index) {
        auto target_authoring = authoring.value();
        if (named_target || condition_tag_target) {
            target_authoring.selection_mode = editor::SelectionMode::object;
            target_authoring.objects = {targets[target_index].object};
            target_authoring.vertices.clear();
            target_authoring.edges.clear();
            target_authoring.faces.clear();
            target_authoring.component_object.reset();
            target_authoring.component_mesh_revision.reset();
        } else if (object_selection) {
            target_authoring.objects = {targets[target_index].object};
        }
        if (auto valid = target_authoring.validate(); !valid) {
            return core::Result<void>::failure(valid.error().with_context(
                "validating selected goal target"));
        }
        const auto compile_context = make_ai_context(
            source_snapshot, target_authoring, compile_effects);
        std::optional<std::string> previous_step_id;
        for (std::size_t command_index = 0U;
             command_index < commands.value().size(); ++command_index) {
            const auto proposal = native_planner_.propose(
                commands.value()[command_index], compile_context);
            if (!proposal) {
                return core::Result<void>::failure(proposal.error().with_context(
                    "compiling target " + std::to_string(target_index + 1U) +
                    ", objective step " + std::to_string(command_index + 1U)));
            }
            if (proposal.value().base_revision != source_snapshot.project_revision ||
                proposal.value().context != target_authoring) {
                return core::Result<void>::failure(stale(
                    "source revision or selection changed while compiling the goal DAG"));
            }
            const std::string step_id = targets.size() == 1U
                ? "step-" + std::to_string(command_index + 1U)
                : "target-" + std::to_string(target_index + 1U) + "-step-" +
                    std::to_string(command_index + 1U);
            std::vector<std::string> dependencies;
            if (previous_step_id.has_value()) dependencies.push_back(*previous_step_id);
            steps.push_back(agent::AgentPlanStep{
                step_id, targets[target_index], commands.value()[command_index],
                std::move(dependencies)});
            previous_step_id = step_id;
        }
    }

    std::vector<scene::ObjectId> objective_scope;
    objective_scope.reserve(targets.size());
    for (const auto& target : targets) objective_scope.push_back(target.object);

    const std::uint64_t task_sequence = next_agent_task_id_ ==
        std::numeric_limits<std::uint64_t>::max() ? 1U : next_agent_task_id_;
    const std::string task_id = agent_plan_id_.empty()
        ? "agent-task-" + std::to_string(task_sequence) : agent_plan_id_;
    auto clarifications = agent_clarifications_draft_;
    std::string named_target_question_id;
    if (named_target_resolution.has_value()) {
        const auto& pending = named_target_resolution->pending;
        agent::ClarificationQuestion question;
        question.id = "question-" + std::to_string(clarifications.size() + 1U);
        question.prompt = pending.prompt;
        question.rationale = pending.rationale;
        question.impacts = {agent::ClarificationImpact::object_scope};
        question.affected_objects = objective_scope;
        question.downstream_plan_branches = steps.size();
        question.requires_explicit_answer = true;
        question.candidates.reserve(pending.candidates.size());
        for (const auto& candidate : pending.candidates) {
            question.candidates.push_back(agent::ClarificationCandidate{
                candidate.id, candidate.label, {}, candidate.object});
        }
        named_target_question_id = question.id;
        clarifications.push_back(std::move(question));
    }

    agent::AuthoringObjective objective{
        agent_objective_id_.empty() ? "agent-objective-draft" : agent_objective_id_,
        goal, source_snapshot.project_revision, std::move(objective_scope), {},
        std::move(constraints.value()), std::move(clarifications), agent_recipe_id_};
    agent::AgentPlan plan{
        named_target_resolution.has_value() ? task_id : "agent-plan-draft",
        std::move(objective), steps};
    if (named_target_resolution.has_value()) {
        if (auto answered = application::HumanApplicationAccess::answer_agent_clarification(
                plan, named_target_question_id,
                named_target_resolution->selected_candidate_id, std::nullopt); !answered) {
            return core::Result<void>::failure(answered.error().with_context(
                "recording the explicit exact-name target choice"));
        }
    }
    if (auto valid = plan.validate(); !valid) {
        return core::Result<void>::failure(valid.error().with_context(
            "validating the compiled objective plan"));
    }
    const auto final_authoring = session_.authoring_context();
    const auto final_snapshot = session_.snapshot();
    if (final_snapshot.project_revision != source_snapshot.project_revision ||
        final_snapshot.project_generation != source_snapshot.project_generation ||
        !final_authoring || final_authoring.value() != authoring.value()) {
        return core::Result<void>::failure(stale(
            "source revision or selection changed before the compiled plan was installed"));
    }

    agent_plan_base_revision_ = source_snapshot.project_revision;
    agent_plan_draft_ = std::move(steps);
    agent_clarifications_draft_ = std::move(plan.objective.clarifications);
    agent_goal_ = std::move(goal);
    if (agent_objective_id_.empty()) agent_objective_id_ = "agent-objective-draft";
    if (named_target_resolution.has_value()) agent_plan_id_ = task_id;
    agent_condition_inventory_.reset();
    return core::Result<void>::success();
}

core::Result<void> UiController::resolve_agent_named_goal_target(
    std::string_view candidate_id) {
    if (!agent_named_target_clarification_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "there is no pending exact-name target clarification"));
    }
    const auto pending = *agent_named_target_clarification_;
    const auto candidate = std::find_if(pending.candidates.begin(), pending.candidates.end(),
        [candidate_id](const auto& value) { return value.id == candidate_id; });
    if (candidate == pending.candidates.end()) {
        return core::Result<void>::failure(invalid(
            "exact-name target choice does not match a displayed candidate"));
    }
    const auto current = session_.snapshot();
    if (current.project_generation != pending.source_generation ||
        current.project_revision != pending.source_revision) {
        agent_named_target_clarification_.reset();
        agent_named_target_resolution_.reset();
        agent_goal_.clear();
        return core::Result<void>::failure(stale(
            "the source project changed; compile the exact-name goal again"));
    }
    if (agent_task_.has_value() || !agent_plan_draft_.empty() ||
        agent_checkpoint_pending_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "the exact-name target choice cannot replace an active plan or checkpoint"));
    }

    agent_named_target_clarification_.reset();
    agent_named_target_resolution_ = AgentNamedTargetResolution{
        pending, candidate->id, candidate->object};
    auto compiled = compile_agent_goal_sequence(pending.goal);
    if (!compiled) {
        agent_named_target_resolution_.reset();
        const auto after = session_.snapshot();
        if (after.project_generation == pending.source_generation &&
            after.project_revision == pending.source_revision) {
            agent_goal_ = pending.goal;
            agent_named_target_clarification_ = pending;
        } else {
            agent_goal_.clear();
        }
    }
    return compiled;
}

core::Result<application::DispatchReceipt> UiController::set_project_authoring_standards(
    project::ProjectAuthoringStandards standards) {
    const auto source = session_.snapshot();
    return dispatch_with_category(
        application::SetProjectAuthoringStandardsAction{
            source.project_generation, source.project_revision, std::move(standards)},
        OperationCategory::committed_command);
}

core::Result<void> UiController::set_agent_recipe(std::string recipe_id) {
    if (agent_named_target_clarification_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "resolve or clear the exact-name target clarification before changing the recipe"));
    }
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
    if (agent_named_target_clarification_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "resolve or clear the exact-name target clarification before changing constraints"));
    }
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
    agent::ClarificationImpact impact,
    std::vector<agent::ClarificationCandidate> candidates,
    std::string rationale) {
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
    if (!candidates.empty() &&
        (candidates.size() < 2U || candidates.size() > 16U)) {
        return core::Result<void>::failure(invalid(
            "candidate clarification requires 2 to 16 choices"));
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
    question.rationale = rationale.empty()
        ? "The operator requires an explicit answer before this plan may run."
        : std::move(rationale);
    question.impacts = {impact};
    question.affected_objects = std::move(scope);
    question.downstream_plan_branches = agent_plan_draft_.size();
    question.candidates = std::move(candidates);
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
    return record_agent_clarification_answer(
        std::move(goal), question_id, std::nullopt, std::move(answer));
}

core::Result<void> UiController::choose_agent_clarification_candidate(
    std::string goal,
    std::string_view question_id,
    std::string candidate_id) {
    return record_agent_clarification_answer(
        std::move(goal), question_id, std::move(candidate_id), std::nullopt);
}

core::Result<void> UiController::record_agent_clarification_answer(
    std::string goal,
    std::string_view question_id,
    std::optional<std::string> candidate_id,
    std::optional<std::string> freeform_answer) {
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
        plan, question_id, std::move(candidate_id), std::move(freeform_answer));
    if (!answered) return answered;
    agent_goal_ = plan.objective.goal;
    agent_clarifications_draft_ = std::move(plan.objective.clarifications);
    return core::Result<void>::success();
}

core::Result<agent::AgentTaskSnapshot> UiController::prepare_agent_plan(
    std::string goal,
    std::optional<agent::AgentWorkEnvelope> requested_envelope) {
    if (agent_named_target_clarification_.has_value()) {
        return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
            "resolve or clear the pending exact-name target clarification before preparing a task"));
    }
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
            agent::effect_mask(agent::Effect::transform)) != 0U &&
        std::any_of(agent_plan_draft_.begin(), agent_plan_draft_.end(),
            [](const auto& step) {
                return starts_with_agent_command_words(
                    step.intent, "translate", "object", "by");
            });
    if (permits_transform) {
        std::vector<scene::ObjectId> transform_roots;
        for (const auto& step : agent_plan_draft_) {
            if (starts_with_agent_command_words(
                    step.intent, "translate", "object", "by") &&
                std::holds_alternative<std::monostate>(step.target.component) &&
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
            "multi-command correspondence requires a single alias chain of unchanged transforms, topology-stable edits to one vertex, or face-topology edits on one object"));
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

    // The exemplar has completed the whole recorded chain, so it must match
    // the final step's post-command evidence (identical to the first step's
    // for single commands and signature-stable chains).
    const auto& final_step = agent_demonstration_trace_->steps.back();
    if (!final_step.result_object_structure_signature.has_value()) {
        return core::Result<std::size_t>::failure(unsupported(
            "this demonstration has no final post-command exemplar evidence"));
    }
    const auto exemplar_signature = session_.native_structure_signature(
        exemplar_object, std::monostate{});
    if (!exemplar_signature ||
        exemplar_signature.value() != *final_step.result_object_structure_signature) {
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
    const bool face_topology_chain =
        is_demonstration_face_topology_chain(*agent_demonstration_trace_);
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
      std::vector<application::NativeFaceChainResolution> chain_resolution;
      for (std::size_t index = 0U; index < agent_demonstration_trace_->steps.size(); ++index) {
        const auto& trace_step = agent_demonstration_trace_->steps[index];
        // Face chains bind only their first face; every later step reuses
        // that object binding and takes its face from the chain replay.
        const auto& binding_step = face_topology_chain
            ? agent_demonstration_trace_->steps.front() : trace_step;
        const auto binding = std::find_if(
            agent_demonstration_bindings_.begin(), agent_demonstration_bindings_.end(),
            [target_set_index, &binding_step](const auto& value) {
                return value.target_set_index == target_set_index &&
                    value.target_alias == binding_step.target_alias &&
                    value.component_alias == binding_step.component_alias;
            });
        if (binding == agent_demonstration_bindings_.end()) {
            return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                "demonstration trace target binding is missing"));
        }
        if (face_topology_chain && index == 0U) {
            const auto* first_face = std::get_if<geometry::FaceId>(&binding->target.component);
            if (first_face == nullptr) {
                return core::Result<agent::AgentTaskSnapshot>::failure(invalid(
                    "face-topology chain binding must select a face"));
            }
            std::vector<application::NativeFaceChainStep> chain;
            chain.reserve(agent_demonstration_trace_->steps.size());
            for (std::size_t chain_index = 0U;
                 chain_index < agent_demonstration_trace_->steps.size(); ++chain_index) {
                const auto& recorded = agent_demonstration_trace_->steps[chain_index];
                chain.push_back(application::NativeFaceChainStep{
                    recorded.tool_id,
                    recorded.parameters.normalized_distance,
                    *recorded.structure_signature,
                    *recorded.result_object_structure_signature,
                    chain_index > 0U && recorded.component_alias ==
                        agent_demonstration_trace_->steps[chain_index - 1U].component_alias});
            }
            auto resolved = session_.resolve_native_face_chain(
                binding->target.object, *first_face, chain);
            if (!resolved) {
                return core::Result<agent::AgentTaskSnapshot>::failure(
                    resolved.error().with_context("demonstration face-topology chain"));
            }
            chain_resolution = std::move(resolved.value());
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
            if (!inserted && (!(sequential_vertex_trace || face_topology_chain) ||
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
        agent::TargetRef step_target = binding->target;
        core::Result<std::string> intent = core::Result<std::string>::failure(
            invalid("demonstration step intent was not resolved"));
        if (face_topology_chain) {
            if (index >= chain_resolution.size()) {
                return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                    "face-topology chain resolution is incomplete"));
            }
            const auto& resolved = chain_resolution[index];
            step_target.component = resolved.face;
            if (trace_step.tool_id == "mesh.poke-face") {
                intent = core::Result<std::string>::success("poke face");
            } else {
                if (!resolved.distance.has_value()) {
                    return core::Result<agent::AgentTaskSnapshot>::failure(invalid_state(
                        "face-topology chain step has no resolved distance"));
                }
                const auto formatted = demonstration_number_text(*resolved.distance);
                if (!formatted) {
                    return core::Result<agent::AgentTaskSnapshot>::failure(formatted.error());
                }
                intent = core::Result<std::string>::success(
                    (trace_step.tool_id == "mesh.inset-face" ? "inset face " : "extrude face ") +
                    formatted.value());
            }
        } else {
            intent = demonstration_step_intent(
                trace_step, binding->target, snapshot, vertex_base_position);
        }
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
            step_target,
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
    agent_checkpoint_directly_conflicting_steps_.clear();
    agent_checkpoint_invalidated_steps_.clear();
    agent_checkpoint_pending_.emplace(std::move(checkpoint.value()));
    return core::Result<void>::success();
}

core::Result<void> UiController::rebase_agent_checkpoint() {
    if (!agent_checkpoint_pending_.has_value() || agent_task_.has_value()) {
        return core::Result<void>::failure(invalid_state(
            "load one saved checkpoint before requesting a rebase"));
    }
    agent_checkpoint_directly_conflicting_steps_.clear();
    agent_checkpoint_invalidated_steps_.clear();
    const auto analysis = agent::AgentTask::analyze_checkpoint_rebase(
        session_, *agent_checkpoint_pending_);
    if (!analysis) {
        record_ui_problem(analysis.error().with_context("agent task rebase analysis"));
        return core::Result<void>::failure(analysis.error());
    }
    agent_checkpoint_directly_conflicting_steps_ =
        analysis.value().directly_conflicting_step_ids;
    agent_checkpoint_invalidated_steps_ = analysis.value().invalidated_step_ids;
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
    agent_checkpoint_directly_conflicting_steps_.clear();
    agent_checkpoint_invalidated_steps_.clear();
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
    agent_named_target_clarification_.reset();
    agent_named_target_resolution_.reset();
    agent_plan_draft_.clear();
    agent_hard_constraints_draft_.clear();
    agent_clarifications_draft_.clear();
    agent_checkpoint_pending_.reset();
    agent_checkpoint_rebased_ = false;
    agent_checkpoint_directly_conflicting_steps_.clear();
    agent_checkpoint_invalidated_steps_.clear();
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
        record.importer_version = format == MeshImportFormat::obj ? 2U : 1U;
        record.options.import_hierarchy = false;
        record.options.import_materials = false;
        record.options.import_normals = false;
        record.artifact_identity = "pending.mesh";
        record.artifact_digest = application::canonical_mesh_geometry_digest(mesh);
        record.domain_digests.push_back(assets::SourceDomainDigest{
            assets::SourceAssetDomain::geometry, record.artifact_digest});
        auto material_digest = application::canonical_mesh_material_region_digest(mesh);
        if (!material_digest) return fail(material_digest.error());
        if (material_digest.value().has_value()) {
            record.domain_digests.push_back(assets::SourceDomainDigest{
                assets::SourceAssetDomain::material,
                *material_digest.value()});
        }
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

core::Result<application::DispatchReceipt> UiController::import_generated_botanical_gltf(
    std::span<const std::uint8_t> gltf_bytes,
    const botanical::Source& source,
    const runtime_api::JobReceipt& job,
    std::string_view expected_organization_scope,
    std::string object_name) {
    const auto fail = [this](core::Diagnostic diagnostic) {
        auto surfaced = diagnostic.with_context("private botanical result promotion");
        record_ui_problem(surfaced);
        return core::Result<application::DispatchReceipt>::failure(std::move(surfaced));
    };
    if (auto valid = source.validate(); !valid) return fail(valid.error());
    if (expected_organization_scope.empty() ||
        job.organization_scope != expected_organization_scope) {
        return fail(core::Diagnostic(core::ErrorCode::validation_failed,
            "completed job receipt does not match the configured organization scope"));
    }
    const std::string source_snapshot = source.serialize();
    const auto* source_data = reinterpret_cast<const std::uint8_t*>(source_snapshot.data());
    const auto source_digest = assets::sha256(std::span<const std::uint8_t>(
        source_data, source_snapshot.size()));
    const auto valid_runtime_id = [](std::string_view value) {
        return !value.empty() && value.size() <= 128U &&
            value.find("..") == std::string_view::npos &&
            std::all_of(value.begin(), value.end(), [](unsigned char byte) {
                return std::isalnum(byte) != 0 || byte == '.' || byte == '_' || byte == '-';
            });
    };
    if (job.status != "succeeded" || job.source_identity != source.identity ||
        job.source_revision != source.revision ||
        !valid_runtime_id(job.job_id) || !valid_runtime_id(job.source_artifact_id) ||
        !valid_runtime_id(job.output_artifact_id) ||
        job.source_digest != source_digest || job.output_digest.is_zero() ||
        (job.output_kind != runtime_api::kBotanicalGltfOutputKind &&
         job.output_kind != runtime_api::kBotanicalGlbOutputKind)) {
        return fail(invalid(
            "completed job receipt does not bind the exact source and approved botanical interchange output"));
    }
    const auto current_authoring = session_.condition_authoring();
    if (!current_authoring) return fail(current_authoring.error().with_context(
        "current CONDITION source revision"));
    if (current_authoring.value().project_revision != source.revision) {
        return fail(core::Diagnostic(core::ErrorCode::stale_data,
            "CONDITION authoring changed after generation; submit the current revision again"));
    }
    const bool is_glb = job.output_kind == runtime_api::kBotanicalGlbOutputKind;
    const auto imported = is_glb
        ? io::import_generated_botanical_glb(
            gltf_bytes, source.identity, source.revision, job.output_digest)
        : io::import_generated_botanical_gltf(
            gltf_bytes, source.identity, source.revision, job.output_digest);
    if (!imported) return fail(imported.error());

    const auto application_snapshot = session_.snapshot();
    if (!application_snapshot.project_path.has_value()) {
        return fail(core::Diagnostic(core::ErrorCode::invalid_state,
            "save the project before promoting a source-linked generated mesh"));
    }
    const auto staged_source = persist_generated_botanical_source_blob(
        *application_snapshot.project_path, gltf_bytes);
    if (!staged_source) return fail(staged_source.error());

    const auto job_identity_digest = assets::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(job.job_id.data()), job.job_id.size()));
    const std::string job_identity_hash = job_identity_digest.hex();
    assets::SourceImportRecord record;
    record.identity = "botanical.generated." + job_identity_hash.substr(0U, 32U);
    record.relative_source_path = staged_source.value().relative_path;
    record.source_generation = core::Revision{1U};
    record.kind = assets::SourceAssetKind::mesh;
    record.source_digest = staged_source.value().digest;
    record.importer_identity = is_glb
        ? "carto.import.generated-glb" : "carto.import.generated-gltf";
    record.importer_version = 1U;
    record.options.import_hierarchy = false;
    record.options.import_materials = false;
    record.options.import_normals = false;
    record.artifact_identity = "pending.mesh";
    record.artifact_digest = application::canonical_mesh_geometry_digest(imported.value().mesh);
    record.domain_digests.push_back(assets::SourceDomainDigest{
        assets::SourceAssetDomain::geometry, record.artifact_digest});
    record.botanical_generation = assets::BotanicalGenerationReceipt{
        source_snapshot, job_identity_digest, job.source_digest, job.output_digest};

    const auto active = std::find_if(
        application_snapshot.source_assets.begin(), application_snapshot.source_assets.end(),
        [&record](const application::SourceAssetView& item) {
            return item.identity == record.identity;
        });
    if (active != application_snapshot.source_assets.end()) {
        record.artifact_identity = active->artifact_identity;
        record.source_generation = active->source_generation;
    }
    if (object_name.empty()) object_name = "Generated Botanical Mesh";
    if (auto valid = record.validate(); !valid) return fail(valid.error());

    auto admitted = dispatch(application::CreateMeshObjectAction{
        std::move(object_name), std::move(imported.value().mesh), std::move(record)});
    if (!admitted) return core::Result<application::DispatchReceipt>::failure(
        admitted.error());
    return admitted;
}

core::Result<application::DispatchReceipt> UiController::import_material_texture(
    const std::filesystem::path& path) {
    const auto fail = [this](core::Diagnostic diagnostic) {
        auto surfaced = diagnostic.with_context("native material texture import");
        record_ui_problem(surfaced);
        return core::Result<application::DispatchReceipt>::failure(std::move(surfaced));
    };
    if (path.empty() || path.filename().empty()) {
        return fail(invalid("material texture import path must name a file"));
    }
    const auto application_snapshot = session_.snapshot();
    if (!application_snapshot.project_path.has_value()) {
        return fail(invalid_state(
            "save the project before importing a managed material texture source"));
    }

    const assets::TextureSourceLimits limits;
    const auto encoded = read_bounded_file(
        path, static_cast<std::size_t>(limits.max_source_bytes), "material texture source");
    if (!encoded) return fail(encoded.error());
    const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.value().data());
    const std::span<const std::uint8_t> source(data, encoded.value().size());
    const auto inspected = assets::inspect_texture_source(source, {}, limits);
    if (!inspected) return fail(inspected.error());
    if (inspected.value().stage != assets::TextureSourceStage::container_inspected) {
        return fail(validation(
            "texture inspection returned an unsupported source stage"));
    }
    if (std::any_of(application_snapshot.texture_sources.begin(),
            application_snapshot.texture_sources.end(), [&inspected](const auto& source_record) {
                return source_record.content_digest == inspected.value().source.digest;
            })) {
        return fail(invalid(
            "this content-addressed texture source is already in the project"));
    }
    const auto locator = persist_texture_source_blob(
        *application_snapshot.project_path, encoded.value(), inspected.value(), limits);
    if (!locator) return fail(locator.error());

    auto imported = dispatch(application::ImportMaterialTextureSourceAction{
        application_snapshot.project_revision,
        inspected.value().source.digest,
        inspected.value().format,
        inspected.value().width,
        inspected.value().height});
    if (!imported) return fail(imported.error());
    return imported;
}

core::Result<application::DispatchReceipt> UiController::import_reference_image(
    const std::filesystem::path& path) {
    const auto fail = [this](core::Diagnostic diagnostic) {
        auto surfaced = diagnostic.with_context("native reference-image import");
        record_ui_problem(surfaced);
        return core::Result<application::DispatchReceipt>::failure(std::move(surfaced));
    };
    if (path.empty() || path.filename().empty()) {
        return fail(invalid("reference-image import path must name a file"));
    }
    const auto initial = session_.snapshot();
    if (!initial.project_path.has_value()) {
        return fail(invalid_state(
            "save the project before importing a managed reference image"));
    }

    const assets::TextureSourceLimits limits;
    const auto encoded = read_bounded_file(
        path, static_cast<std::size_t>(limits.max_source_bytes), "reference image source");
    if (!encoded) return fail(encoded.error());
    const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.value().data());
    const std::span<const std::uint8_t> source(data, encoded.value().size());
    const auto inspected = assets::inspect_texture_source(source, {}, limits);
    if (!inspected) return fail(inspected.error());
    if (inspected.value().stage != assets::TextureSourceStage::container_inspected) {
        return fail(validation(
            "reference-image inspection returned an unsupported source stage"));
    }

    std::error_code error;
    const auto requested_root = initial.project_path->parent_path().empty()
        ? std::filesystem::current_path(error)
        : initial.project_path->parent_path();
    if (error) return fail(io_error("unable to resolve the saved project directory"));
    const auto project_root = std::filesystem::weakly_canonical(requested_root, error);
    if (error || project_root.empty()) {
        return fail(io_error("unable to resolve the saved project directory"));
    }
    assets::BlobStore blob_store(
        project_root / ".carto-assets" / "source-blobs",
        assets::BlobStoreLimits{limits.max_source_bytes, 256U});
    const auto expected_blob = blob_store.path_for(inspected.value().source.digest);
    const auto relative_blob = expected_blob.lexically_relative(project_root);
    if (relative_blob.empty() || relative_blob.is_absolute() ||
        std::any_of(relative_blob.begin(), relative_blob.end(),
            [](const auto& component) { return component == ".."; })) {
        return fail(validation(
            "reference-image blob has no safe project-relative locator"));
    }
    const std::string relative_locator = relative_blob.generic_string();
    const std::string plane_identity =
        "reference.image." + inspected.value().source.digest.hex();
    const auto& current_catalog = initial.presentation_catalog;
    if (current_catalog.reference_planes().contains(plane_identity)) {
        return fail(invalid("this reference-image source already has a plane"));
    }

    const auto matching_source = std::find_if(
        initial.source_assets.begin(), initial.source_assets.end(),
        [&inspected, &relative_locator](const application::SourceAssetView& item) {
            return item.kind == assets::SourceAssetKind::image &&
                item.relative_source_path == relative_locator &&
                item.source_digest == inspected.value().source.digest;
        });
    const auto conflicting_path = std::find_if(
        initial.source_assets.begin(), initial.source_assets.end(),
        [&relative_locator](const application::SourceAssetView& item) {
            return item.relative_source_path == relative_locator;
        });
    const auto identity = "reference.image.source." + inspected.value().source.digest.hex();
    const auto identity_collision = std::find_if(
        initial.source_assets.begin(), initial.source_assets.end(),
        [&identity](const application::SourceAssetView& item) {
            return item.identity == identity;
        });
    if (matching_source == initial.source_assets.end() &&
        (conflicting_path != initial.source_assets.end() ||
         identity_collision != initial.source_assets.end())) {
        return fail(validation(
            "reference-image identity or content path conflicts with a registered source"));
    }

    core::Revision source_generation{1U};
    std::optional<assets::SourceReimportPlan> source_plan;
    if (matching_source != initial.source_assets.end()) {
        source_generation = matching_source->source_generation;
    } else {
        assets::SourceImportRecord record;
        record.identity = identity;
        record.relative_source_path = relative_locator;
        record.kind = assets::SourceAssetKind::image;
        record.source_digest = inspected.value().source.digest;
        record.importer_identity = "carto.import.image";
        record.importer_version = 1U;
        record.options.import_hierarchy = false;
        record.options.import_materials = false;
        record.options.import_normals = false;
        record.artifact_identity = "image." + plane_identity;
        record.artifact_digest = inspected.value().source.digest;
        auto plan = session_.preview_source_asset_addition(std::move(record));
        if (!plan) return fail(plan.error());
        const auto entry = std::find_if(plan.value().entries.begin(), plan.value().entries.end(),
            [&identity](const assets::SourceReimportPlanEntry& item) {
                return item.identity == identity;
            });
        if (entry == plan.value().entries.end() ||
            entry->change != assets::SourceReimportChange::added ||
            !entry->incoming.has_value()) {
            return fail(validation(
                "reference-image source plan did not contain the expected addition"));
        }
        source_generation = entry->incoming->source_generation;
        source_plan = std::move(plan.value());
    }

    project::ReferenceImagePlane plane;
    plane.identity = plane_identity;
    plane.source_asset_identity = matching_source != initial.source_assets.end()
        ? matching_source->identity : identity;
    plane.source_locator = relative_locator;
    plane.source_digest = inspected.value().source.digest;
    plane.source_generation = source_generation;
    plane.image_width_pixels = inspected.value().width;
    plane.image_height_pixels = inspected.value().height;
    plane.view = project::ReferencePlaneView::perspective;
    plane.width_meters = 1.0;
    plane.opacity = 1.0;
    plane.visible = true;
    plane.locked = false;
    if (auto valid = plane.validate(); !valid) return fail(valid.error());
    auto candidate_catalog = current_catalog;
    if (auto inserted = candidate_catalog.insert_reference_plane(plane); !inserted) {
        return fail(inserted.error());
    }
    if (auto valid = candidate_catalog.validate(); !valid) return fail(valid.error());

    const auto persisted = persist_texture_source_blob(
        *initial.project_path, encoded.value(), inspected.value(), limits);
    if (!persisted) return fail(persisted.error());
    if (persisted.value() != relative_locator) {
        return fail(stale(
            "reference-image locator changed while its bytes were being staged"));
    }

    if (source_plan.has_value()) {
        const auto registered = apply_source_reimport_plan(std::move(*source_plan));
        if (!registered) return fail(registered.error());
    }
    const auto before_catalog_commit = session_.snapshot();
    const auto committed = dispatch(application::SetPresentationCatalogAction{
        before_catalog_commit.project_generation,
        before_catalog_commit.project_revision,
        std::move(candidate_catalog)});
    if (!committed) {
        return fail(committed.error().with_context(
            "source bytes or registration may remain without a reference plane"));
    }
    return committed;
}

core::Result<application::DispatchReceipt> UiController::update_reference_image_plane(
    std::uint64_t expected_project_generation,
    core::Revision expected_project_revision,
    project::ReferenceImagePlane plane) {
    const auto fail = [this](core::Diagnostic diagnostic) {
        auto surfaced = diagnostic.with_context("reference-image plane edit");
        record_ui_problem(surfaced);
        return core::Result<application::DispatchReceipt>::failure(std::move(surfaced));
    };
    const auto before = session_.snapshot();
    if (before.project_generation != expected_project_generation ||
        before.project_revision != expected_project_revision) {
        return fail(stale(
            "reference-image edit snapshot no longer matches the project generation and revision"));
    }
    if (!before.project_path.has_value()) {
        return fail(invalid_state(
            "reference-image plane edits require a saved project"));
    }
    const auto current = before.presentation_catalog.reference_planes().find(plane.identity);
    if (current == before.presentation_catalog.reference_planes().end()) {
        return fail(stale("reference-image plane no longer exists in the project"));
    }
    const auto& original = current->second;
    if (plane.source_asset_identity != original.source_asset_identity ||
        plane.source_locator != original.source_locator ||
        plane.source_digest != original.source_digest ||
        plane.source_generation != original.source_generation ||
        plane.image_width_pixels != original.image_width_pixels ||
        plane.image_height_pixels != original.image_height_pixels) {
        return fail(validation(
            "reference-image plane edits cannot replace or rebind its source"));
    }

    auto normalized_candidate = plane;
    normalized_candidate.revision = original.revision;
    project::PresentationCatalog original_record;
    project::PresentationCatalog candidate_record;
    if (auto inserted = original_record.insert_reference_plane(original); !inserted) {
        return fail(inserted.error());
    }
    if (auto inserted = candidate_record.insert_reference_plane(normalized_candidate);
        !inserted) {
        return fail(inserted.error());
    }
    if (original_record.serialize() == candidate_record.serialize()) {
        return fail(invalid("reference-image plane edit contains no changes"));
    }
    if (original.revision.exhausted()) {
        return fail(invalid_state("reference-image plane revision space is exhausted"));
    }
    plane.revision = original.revision.next();
    if (auto valid = plane.validate(); !valid) return fail(valid.error());

    project::PresentationCatalog candidate;
    for (const auto& [object, camera] : before.presentation_catalog.cameras()) {
        static_cast<void>(object);
        if (auto inserted = candidate.insert_camera(camera); !inserted) {
            return fail(inserted.error());
        }
    }
    for (const auto& [object, light] : before.presentation_catalog.lights()) {
        static_cast<void>(object);
        if (auto inserted = candidate.insert_light(light); !inserted) {
            return fail(inserted.error());
        }
    }
    for (const auto& [identity, existing] :
            before.presentation_catalog.reference_planes()) {
        const auto& replacement = identity == plane.identity ? plane : existing;
        if (auto inserted = candidate.insert_reference_plane(replacement); !inserted) {
            return fail(inserted.error());
        }
    }
    if (auto valid = candidate.validate(); !valid) return fail(valid.error());
    return dispatch(application::SetPresentationCatalogAction{
        before.project_generation,
        before.project_revision,
        std::move(candidate)});
}

core::Result<application::DispatchReceipt>
UiController::update_reference_image_plane_views(
    std::uint64_t expected_project_generation,
    core::Revision expected_project_revision,
    std::vector<ReferenceImagePlaneViewEdit> edits) {
    const auto fail = [this](core::Diagnostic diagnostic) {
        auto surfaced = diagnostic.with_context("reference-image view-set edit");
        record_ui_problem(surfaced);
        return core::Result<application::DispatchReceipt>::failure(std::move(surfaced));
    };
    const auto before = session_.snapshot();
    constexpr std::size_t kMaxViewEdits = 4'096U;
    if (before.project_generation != expected_project_generation ||
        before.project_revision != expected_project_revision) {
        return fail(stale(
            "reference-image view edit snapshot no longer matches the project generation and revision"));
    }
    if (!before.project_path.has_value()) {
        return fail(invalid_state(
            "reference-image view-set edits require a saved project"));
    }
    if (edits.empty() || edits.size() > kMaxViewEdits) {
        return fail(validation(
            "reference-image view-set edit count is empty or exceeds its bound"));
    }

    std::set<std::string> seen;
    std::map<std::string, project::ReferenceImagePlane> replacements;
    for (auto& edit : edits) {
        if (!seen.insert(edit.identity).second) {
            return fail(validation(
                "reference-image view-set edit duplicates a plane identity"));
        }
        const auto current = before.presentation_catalog.reference_planes().find(
            edit.identity);
        if (current == before.presentation_catalog.reference_planes().end()) {
            return fail(stale("reference-image view edit names a missing plane"));
        }
        const auto& original = current->second;
        if (original.locked) {
            return fail(core::Diagnostic(
                core::ErrorCode::unsupported,
                "locked reference-image view placement cannot be changed"));
        }
        auto candidate = original;
        candidate.view = edit.view;
        candidate.view_set_identity = std::move(edit.view_set_identity);
        candidate.pinned_camera = edit.pinned_camera;
        switch (candidate.view) {
        case project::ReferencePlaneView::front:
            candidate.normal = {0.0, 0.0, 1.0};
            candidate.up = {0.0, 1.0, 0.0};
            break;
        case project::ReferencePlaneView::side:
            candidate.normal = {1.0, 0.0, 0.0};
            candidate.up = {0.0, 1.0, 0.0};
            break;
        case project::ReferencePlaneView::top:
            candidate.normal = {0.0, -1.0, 0.0};
            candidate.up = {0.0, 0.0, 1.0};
            break;
        case project::ReferencePlaneView::perspective:
        case project::ReferencePlaneView::camera_pinned:
            break;
        }
        const auto same_vector = [](core::Vec3d first, core::Vec3d second) {
            return first.x == second.x && first.y == second.y && first.z == second.z;
        };
        if (candidate.view == original.view &&
            candidate.view_set_identity == original.view_set_identity &&
            candidate.pinned_camera == original.pinned_camera &&
            same_vector(candidate.normal, original.normal) &&
            same_vector(candidate.up, original.up)) {
            continue;
        }
        if (original.revision.exhausted()) {
            return fail(invalid_state(
                "reference-image plane revision space is exhausted"));
        }
        candidate.revision = original.revision.next();
        if (auto valid = candidate.validate(); !valid) {
            return fail(valid.error());
        }
        replacements.emplace(candidate.identity, std::move(candidate));
    }
    if (replacements.empty()) {
        return fail(invalid("reference-image view-set edit contains no changes"));
    }

    project::PresentationCatalog candidate;
    for (const auto& [object, camera] : before.presentation_catalog.cameras()) {
        static_cast<void>(object);
        if (auto inserted = candidate.insert_camera(camera); !inserted) {
            return fail(inserted.error());
        }
    }
    for (const auto& [object, light] : before.presentation_catalog.lights()) {
        static_cast<void>(object);
        if (auto inserted = candidate.insert_light(light); !inserted) {
            return fail(inserted.error());
        }
    }
    for (const auto& [identity, existing] :
            before.presentation_catalog.reference_planes()) {
        const auto replacement = replacements.find(identity);
        const auto& plane = replacement == replacements.end()
            ? existing : replacement->second;
        if (auto inserted = candidate.insert_reference_plane(plane); !inserted) {
            return fail(inserted.error());
        }
    }
    if (auto valid = candidate.validate(); !valid) return fail(valid.error());
    return dispatch(application::SetPresentationCatalogAction{
        before.project_generation,
        before.project_revision,
        std::move(candidate)});
}

core::Result<assets::DecodedRgba8Texture> UiController::decode_reference_image_tga(
    std::string_view identity) {
    auto decoded = session_.decode_reference_image_tga(identity);
    if (!decoded) {
        auto surfaced = decoded.error().with_context("native reference-image preview");
        record_ui_problem(surfaced);
        return core::Result<assets::DecodedRgba8Texture>::failure(std::move(surfaced));
    }
    return decoded;
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

core::Result<engineering::ImportProbe> UiController::probe_engineering_step(
    const std::filesystem::path& path) const {
    auto prefix = read_file_prefix(path, engineering::kMaxStepHeaderProbeBytes,
        "STEP source");
    if (!prefix) return core::Result<engineering::ImportProbe>::failure(prefix.error());
    engineering::StepAp242Provider provider;
    return provider.probe(prefix.value());
}

core::Result<application::DispatchReceipt> UiController::import_engineering_bom_csv(
    const std::filesystem::path& path) {
    auto bytes = read_bounded_file(path, engineering::kMaxBomCsvBytes,
        "engineering BOM CSV");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    const auto snapshot = session_.snapshot();
    if (!snapshot.engineering.present) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "import the engineering authoring document before importing its BOM"));
    }
    return dispatch_with_category(
        application::ImportEngineeringBomCsvAction{
            snapshot.project_revision,
            snapshot.engineering.authored_revision,
            path.filename().generic_string(),
            std::move(bytes).value()},
        OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::import_engineering_requirement_csv(
    const std::filesystem::path& path) {
    auto bytes = read_bounded_file(path, engineering::kMaxRequirementCsvBytes,
        "engineering requirement CSV");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    const auto snapshot = session_.snapshot();
    if (!snapshot.engineering.present) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "import the engineering authoring document before importing requirements"));
    }
    return dispatch_with_category(
        application::ImportEngineeringRequirementCsvAction{
            snapshot.project_revision,
            snapshot.engineering.authored_revision,
            path.filename().generic_string(),
            std::move(bytes).value()},
        OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::import_engineering_path_result(
    const std::filesystem::path& path) {
    auto bytes = read_bounded_file(path, engineering::VantaPathResult::kMaxSerializedBytes,
        "VANTA path result");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    const auto snapshot = session_.snapshot();
    if (!snapshot.engineering.present) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "import the engineering authoring document before importing a VANTA path result"));
    }
    return dispatch_with_category(
        application::ImportEngineeringPathResultAction{
            snapshot.project_revision,
            snapshot.engineering.authored_revision,
            std::move(bytes).value()},
        OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::attach_engineering_evidence(
    const std::filesystem::path& path,
    engineering::SourceKind kind) {
    if (kind != engineering::SourceKind::drawing &&
        kind != engineering::SourceKind::specification) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "opaque engineering evidence supports drawing or specification sources only"));
    }
    auto bytes = read_bounded_file(path,
        engineering::kMaxEngineeringEvidenceBytes, "engineering evidence");
    if (!bytes) return core::Result<application::DispatchReceipt>::failure(bytes.error());
    const auto snapshot = session_.snapshot();
    if (!snapshot.engineering.present) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "import engineering authoring before attaching source evidence"));
    }
    return dispatch_with_category(
        application::AttachEngineeringEvidenceAction{
            snapshot.project_revision,
            snapshot.engineering.authored_revision,
            kind,
            path.filename().generic_string(),
            std::move(bytes).value()},
        OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::create_engineering_characteristic(
    const application::CreateEngineeringCharacteristicAction& action) {
    return dispatch_with_category(action, OperationCategory::committed_command);
}

core::Result<application::DispatchReceipt> UiController::create_engineering_target(
    const application::CreateEngineeringTargetAction& action) {
    return dispatch_with_category(action, OperationCategory::committed_command);
}

core::Result<application::DispatchReceipt> UiController::update_engineering_workcell(
    const application::UpdateEngineeringWorkcellAction& action) {
    return dispatch_with_category(action, OperationCategory::committed_command);
}

core::Result<application::DispatchReceipt> UiController::create_engineering_keep_out(
    const application::CreateEngineeringKeepOutAction& action) {
    return dispatch_with_category(action, OperationCategory::committed_command);
}

core::Result<application::DispatchReceipt> UiController::create_engineering_workcell_frame(
    const application::CreateEngineeringWorkcellFrameAction& action) {
    return dispatch_with_category(action, OperationCategory::committed_command);
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

core::Result<engineering::PackageAdmissionResult> UiController::admit_engineering_package() const {
    const auto package = build_engineering_package();
    if (!package) {
        return core::Result<engineering::PackageAdmissionResult>::failure(package.error());
    }
    return engineering::admit_for_consumer(package.value());
}

core::Result<void> UiController::publish_engineering_package(
    const std::filesystem::path& path) const {
    if (path.empty() || path.filename().empty()) {
        return core::Result<void>::failure(invalid(
            "engineering publication path must name a file"));
    }
    const auto package = build_engineering_package();
    if (!package) return core::Result<void>::failure(package.error());
    const auto admission = engineering::admit_for_consumer(package.value());
    if (!admission) return core::Result<void>::failure(admission.error().with_context(
        "engineering publication admission"));
    if (admission.value().status != engineering::PackageAdmissionStatus::admit) {
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

core::Result<application::DispatchReceipt> UiController::add_condition_interaction_anchor(
    condition::InteractionAnchor anchor,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(application::AddConditionInteractionAnchorAction{
        expected_project_revision,
        expected_authoring_revision,
        std::move(anchor)}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_prefab(
    condition::PrefabRecord prefab,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(application::AddConditionPrefabAction{
        expected_project_revision,
        expected_authoring_revision,
        std::move(prefab)}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::instantiate_condition_prefab(
    condition::Id prefab_id,
    std::string name,
    core::Vec3d world_position,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(application::InstantiateConditionPrefabAction{
        expected_project_revision,
        expected_authoring_revision,
        prefab_id,
        std::move(name),
        world_position}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_collection(
    condition::CollectionRecord collection,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(application::AddConditionCollectionAction{
        expected_project_revision,
        expected_authoring_revision,
        std::move(collection)}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::add_condition_region_kit(
    condition::RegionKitManifest region_kit,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(application::AddConditionRegionKitAction{
        expected_project_revision,
        expected_authoring_revision,
        std::move(region_kit)}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::replace_condition_authored_record(
    application::ConditionAuthoredRecord record,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(
        application::ReplaceConditionAuthoredRecordAction{
            expected_project_revision, expected_authoring_revision,
            std::move(record)},
        OperationCategory::provider);
}

core::Result<application::DispatchReceipt> UiController::delete_condition_authored_record(
    application::ConditionAuthoredRecordKind kind,
    condition::Id id,
    core::Revision expected_project_revision,
    core::Revision expected_authoring_revision) {
    return dispatch_with_category(
        application::DeleteConditionAuthoredRecordAction{
            expected_project_revision, expected_authoring_revision, kind, id},
        OperationCategory::provider);
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
    const condition::Id branch_id = branch.id;
    if (auto appended = append_condition_record(
            authoring.value().state_branches, std::move(branch), "state branch"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    const auto materialized = condition::compare_state_branches(
        authoring.value(), 0U, branch_id);
    if (!materialized) {
        return core::Result<application::DispatchReceipt>::failure(
            materialized.error().with_context(
                "CONDITION state branch does not match its source or parent before-values"));
    }
    return dispatch_with_category(application::ImportConditionAuthoringAction{
        authoring.value().serialize()}, OperationCategory::provider);
}

core::Result<application::DispatchReceipt>
UiController::resolve_condition_state_branch_merge(
    const condition::StateComparisonResult& comparison,
    const std::vector<condition::StateMergeChoice>& choices,
    std::string name,
    std::string applicability) {
    auto authoring = session_.condition_authoring();
    if (!authoring) {
        return core::Result<application::DispatchReceipt>::failure(
            authoring.error().with_context("CONDITION state branch merge"));
    }
    if (authoring.value().project_revision.exhausted()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION authoring revision space is exhausted"));
    }
    condition::Id maximum_branch_id = 0U;
    for (const auto& branch : authoring.value().state_branches) {
        maximum_branch_id = std::max(maximum_branch_id, branch.id);
    }
    if (maximum_branch_id == std::numeric_limits<condition::Id>::max()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION state branch identity space is exhausted"));
    }
    const condition::Id merged_branch_id = maximum_branch_id + 1U;
    auto merged = condition::resolve_state_branch_merge(authoring.value(), comparison,
        choices, merged_branch_id, std::move(name), std::move(applicability));
    if (!merged) {
        return core::Result<application::DispatchReceipt>::failure(
            merged.error().with_context("CONDITION state branch merge"));
    }
    if (auto appended = append_condition_record(
            authoring.value().state_branches, std::move(merged).value(),
            "state branch merge"); !appended) {
        return core::Result<application::DispatchReceipt>::failure(appended.error());
    }
    authoring.value().project_revision = authoring.value().project_revision.next();
    if (auto valid_document = authoring.value().validate(); !valid_document) {
        return core::Result<application::DispatchReceipt>::failure(valid_document.error());
    }
    const auto materialized = condition::compare_state_branches(authoring.value(),
        comparison.common_ancestor_branch_id, merged_branch_id);
    if (!materialized) {
        return core::Result<application::DispatchReceipt>::failure(
            materialized.error().with_context(
                "resolved state branch does not match its source or parent before-values"));
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

core::Result<application::DispatchReceipt>
UiController::commit_condition_vegetation_preview(
    const condition::VegetationPreviewResult& preview,
    scene::ObjectId botanical_source_object) {
    if (!botanical_source_object || preview.application_revision.value() == 0U ||
        preview.recipe_id == 0U || preview.preview_digest.is_zero()) {
        return core::Result<application::DispatchReceipt>::failure(invalid(
            "CONDITION vegetation commit requires a valid exact preview and source object"));
    }
    const auto current = session_.snapshot();
    if (current.project_revision != preview.application_revision) {
        return core::Result<application::DispatchReceipt>::failure(stale(
            "CONDITION vegetation preview belongs to an older project revision"));
    }
    if (!preview.plant_source.has_value()) {
        return core::Result<application::DispatchReceipt>::failure(invalid_state(
            "CONDITION vegetation commit requires an explicitly bound plant source"));
    }
    return dispatch(application::CreatePlantScatterBatchAction{
        preview.application_revision,
        preview.recipe_id,
        preview.preview_digest,
        botanical_source_object});
}

core::Result<ConditionKinematicPosePreview>
UiController::preview_condition_kinematic_hinge(
    const application::ApplicationSnapshot& snapshot,
    std::uint64_t assembly_id,
    std::uint64_t joint_id,
    double normalized_travel) const {
    if (snapshot.project_revision != session_.snapshot().project_revision) {
        return core::Result<ConditionKinematicPosePreview>::failure(stale(
            "hinge pose preview snapshot no longer matches the current project"));
    }
    const auto projected = std::find_if(
        snapshot.condition.kinematic_assemblies.begin(),
        snapshot.condition.kinematic_assemblies.end(),
        [assembly_id](const auto& item) { return item.id == assembly_id; });
    if (projected == snapshot.condition.kinematic_assemblies.end()) {
        return core::Result<ConditionKinematicPosePreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "hinge pose preview assembly is not present in this snapshot"));
    }
    if (!projected->part_bindings_current) {
        return core::Result<ConditionKinematicPosePreview>::failure(stale(
            "hinge pose preview is blocked by a missing or stale mesh binding"));
    }
    const auto authoring = session_.condition_authoring();
    if (!authoring) {
        return core::Result<ConditionKinematicPosePreview>::failure(
            authoring.error().with_context("CONDITION hinge pose preview"));
    }
    const auto source = std::find_if(
        authoring.value().kinematic_assemblies.begin(),
        authoring.value().kinematic_assemblies.end(),
        [assembly_id](const auto& item) { return item.id == assembly_id; });
    if (source == authoring.value().kinematic_assemblies.end() ||
        source->source_revision != projected->source_revision ||
        source->asset_id != projected->asset_id) {
        return core::Result<ConditionKinematicPosePreview>::failure(stale(
            "hinge pose preview source changed after the application snapshot"));
    }
    if (auto valid = source->validate(); !valid) {
        return core::Result<ConditionKinematicPosePreview>::failure(
            valid.error().with_context("CONDITION hinge pose preview source"));
    }
    const auto graph = kinematic::KinematicGraph::deserialize(source->graph_bytes);
    if (!graph) {
        return core::Result<ConditionKinematicPosePreview>::failure(
            graph.error().with_context("CONDITION hinge pose preview graph"));
    }
    const auto limits = kinematic::JointLimitSet::deserialize(source->joint_limit_bytes);
    if (!limits) {
        return core::Result<ConditionKinematicPosePreview>::failure(
            limits.error().with_context("CONDITION hinge pose preview limits"));
    }
    const auto local_pose = kinematic::preview_hinge_pose(
        graph.value(), limits.value(), kinematic::JointId{joint_id}, normalized_travel);
    if (!local_pose) {
        return core::Result<ConditionKinematicPosePreview>::failure(local_pose.error());
    }

    const auto owner = std::find_if(
        authoring.value().assets.begin(), authoring.value().assets.end(),
        [source](const auto& item) { return item.id == source->asset_id; });
    if (owner == authoring.value().assets.end() || owner->authored_object_id == 0U) {
        return core::Result<ConditionKinematicPosePreview>::failure(stale(
            "hinge pose preview source has no current authored root object"));
    }
    const auto root_object = std::find_if(
        snapshot.objects.begin(), snapshot.objects.end(), [&owner](const auto& item) {
            return item.object.id.value == owner->authored_object_id;
        });
    if (root_object == snapshot.objects.end() ||
        !root_object->world_transform.finite()) {
        return core::Result<ConditionKinematicPosePreview>::failure(stale(
            "hinge pose preview root transform is unavailable or invalid"));
    }
    const auto& root = root_object->world_transform;
    const double scale_tolerance = 1.0e-8 *
        std::max({1.0, std::abs(root.scale.x), std::abs(root.scale.y),
            std::abs(root.scale.z)});
    if (root.scale.x <= 0.0 || root.scale.y <= 0.0 || root.scale.z <= 0.0 ||
        std::abs(root.scale.x - root.scale.y) > scale_tolerance ||
        std::abs(root.scale.x - root.scale.z) > scale_tolerance) {
        return core::Result<ConditionKinematicPosePreview>::failure(unsupported(
            "hinge pose preview requires a positive uniform root scale"));
    }
    const core::Vec3d pivot = root.translation + root.rotation.rotate(
        core::componentwise_multiply(root.scale, local_pose.value().pivot_in_root));
    const core::Vec3d axis = root.rotation.rotate(local_pose.value().axis_in_root).normalized();
    if (!pivot.finite() || !axis.finite()) {
        return core::Result<ConditionKinematicPosePreview>::failure(validation(
            "hinge pose preview produced a non-finite world pivot or axis"));
    }
    const double half_angle = local_pose.value().angle_radians * 0.5;
    const double sine = std::sin(half_angle);
    const core::Quaternion rotation_delta{
        axis.x * sine, axis.y * sine, axis.z * sine, std::cos(half_angle)};

    ConditionKinematicPosePreview preview;
    preview.assembly_id = assembly_id;
    preview.joint_id = joint_id;
    preview.project_revision = snapshot.project_revision;
    preview.source_revision = source->source_revision;
    preview.angle_radians = local_pose.value().angle_radians;
    for (const auto& binding : source->part_bindings) {
        const auto part = std::find_if(
            snapshot.objects.begin(), snapshot.objects.end(), [&binding](const auto& item) {
                return item.object.id.value == binding.scene_object_id;
            });
        if (part == snapshot.objects.end() ||
            !part->object.mesh_asset.has_value() ||
            *part->object.mesh_asset != binding.mesh_asset_id ||
            !part->mesh_revision.has_value() ||
            *part->mesh_revision != binding.mesh_revision ||
            !part->world_transform.finite()) {
            return core::Result<ConditionKinematicPosePreview>::failure(stale(
                "hinge pose preview is blocked by a stale articulated mesh binding"));
        }
        if (binding.joint_id != joint_id) continue;
        core::Transform transformed = part->world_transform;
        transformed.translation = pivot + rotation_delta.rotate(
            part->world_transform.translation - pivot);
        transformed.rotation = (rotation_delta * part->world_transform.rotation).normalized();
        if (!transformed.finite()) {
            return core::Result<ConditionKinematicPosePreview>::failure(validation(
                "hinge pose preview produced an invalid part transform"));
        }
        preview.part_poses.push_back(ConditionKinematicPartPose{
            binding.scene_object_id, transformed});
    }
    if (preview.part_poses.empty()) {
        return core::Result<ConditionKinematicPosePreview>::failure(core::Diagnostic(
            core::ErrorCode::not_found,
            "hinge pose preview joint has no bound scene mesh parts"));
    }
    return core::Result<ConditionKinematicPosePreview>::success(std::move(preview));
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
    if (std::any_of(snapshot.condition.kinematic_assemblies.begin(),
            snapshot.condition.kinematic_assemblies.end(), [](const auto& assembly) {
                return assembly.part_binding_count == 0U ||
                    !assembly.part_bindings_current;
            })) {
        return core::Result<condition::Package>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION publication is blocked by missing or stale articulated mesh bindings"));
    }
    if (std::any_of(snapshot.condition.prefab_instances.begin(),
            snapshot.condition.prefab_instances.end(), [](const auto& instance) {
                return !instance.current;
            })) {
        return core::Result<condition::Package>::failure(core::Diagnostic(
            core::ErrorCode::stale_data,
            "CONDITION publication is blocked by stale prefab instance source or scene bindings"));
    }
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

core::Result<editor::PointSightlineResult> UiController::evaluate_point_sightline(
    const editor::PointSightlineQuery& query,
    const editor::PointSightlineOptions& options) const {
    const auto current = session_.snapshot();
    if (current.viewport.error.has_value()) {
        return core::Result<editor::PointSightlineResult>::failure(
            current.viewport.error->with_context("native authoring sightline query"));
    }
    if (query.source_revision != current.project_revision ||
        query.source_revision != current.viewport.project_revision) {
        return core::Result<editor::PointSightlineResult>::failure(stale(
            "native authoring sightline query does not match the current viewport snapshot"));
    }
    return editor::evaluate_point_sightline(current.viewport.scene, query, options);
}

core::Result<editor::RouteVisibilityResult> UiController::evaluate_route_visibility(
    const editor::RouteVisibilityQuery& query,
    const editor::RouteVisibilityOptions& options) const {
    const auto current = session_.snapshot();
    if (current.viewport.error.has_value()) {
        return core::Result<editor::RouteVisibilityResult>::failure(
            current.viewport.error->with_context(
                "native authoring route-visibility query requires a current viewport"));
    }
    if (query.source_revision != current.project_revision ||
        query.source_revision != current.viewport.project_revision) {
        return core::Result<editor::RouteVisibilityResult>::failure(stale(
            "native authoring route-visibility query does not match the current viewport snapshot"));
    }
    return editor::evaluate_route_visibility(current.viewport.scene, query, options);
}

core::Result<editor::CurveRouteVisibilityResult>
UiController::evaluate_curve_route_visibility(
    std::string_view curve_identity,
    core::Revision expected_project_revision,
    core::Vec3d target,
    std::uint32_t samples_per_segment,
    std::optional<scene::ObjectId> observer_object,
    std::optional<scene::ObjectId> target_object,
    const editor::RouteVisibilityOptions& options) const {
    const auto initial = session_.snapshot();
    if (initial.project_revision != expected_project_revision ||
        initial.viewport.project_revision != expected_project_revision) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(stale(
            "curve route request does not match the expected project snapshot"));
    }
    if (initial.viewport.error.has_value()) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(
            initial.viewport.error->with_context(
                "native curve-route visibility requires a current viewport"));
    }
    auto curve = session_.curve_path_source(curve_identity);
    if (!curve) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(
            curve.error().with_context("native curve-route visibility"));
    }

    const auto current = session_.snapshot();
    if (current.project_revision != expected_project_revision ||
        current.viewport.project_revision != expected_project_revision ||
        current.project_generation != initial.project_generation) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(stale(
            "project changed while the curve-route source was being captured"));
    }
    if (current.viewport.error.has_value()) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(
            current.viewport.error->with_context(
                "native curve-route visibility requires a current viewport"));
    }
    const auto source_view = std::find_if(
        current.curve_paths.begin(), current.curve_paths.end(),
        [curve_identity](const auto& candidate) {
            return candidate.identity == curve_identity;
        });
    if (source_view == current.curve_paths.end() ||
        source_view->source_revision != curve.value().source_revision) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(stale(
            "curve-route source identity or revision is no longer current"));
    }
    auto query = editor::build_curve_route_visibility_query(
        curve.value(), target, expected_project_revision, samples_per_segment,
        observer_object, target_object, options);
    if (!query) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(query.error());
    }
    if (query.value().curve_digest != source_view->digest) {
        return core::Result<editor::CurveRouteVisibilityResult>::failure(stale(
            "curve-route source digest differs from the captured project receipt"));
    }
    return editor::evaluate_curve_route_visibility(
        current.viewport.scene, curve.value(), query.value(),
        current.project_revision, options);
}

core::Result<editor::LandmarkViewshedResult> UiController::evaluate_landmark_viewshed(
    const editor::LandmarkViewshedQuery& query,
    const editor::LandmarkViewshedOptions& options) const {
    const auto current = session_.snapshot();
    if (current.viewport.error.has_value()) {
        return core::Result<editor::LandmarkViewshedResult>::failure(
            current.viewport.error->with_context(
                "native authoring viewshed query requires a current viewport"));
    }
    if (query.source_revision != current.project_revision ||
        query.source_revision != current.viewport.project_revision) {
        return core::Result<editor::LandmarkViewshedResult>::failure(stale(
            "native authoring viewshed query does not match the current viewport snapshot"));
    }
    return editor::evaluate_landmark_viewshed(current.viewport.scene, query, options);
}

core::Result<editor::AuthoringObservationResult>
UiController::evaluate_authoring_observation(
    const editor::AuthoringObservationQuery& query,
    const editor::AuthoringObservationOptions& options) const {
    const auto current = session_.snapshot();
    if (current.viewport.error.has_value()) {
        return core::Result<editor::AuthoringObservationResult>::failure(
            current.viewport.error->with_context(
                "native authoring observation requires a current viewport"));
    }
    if (query.source_revision != current.project_revision ||
        query.source_revision != current.viewport.project_revision) {
        return core::Result<editor::AuthoringObservationResult>::failure(stale(
            "native authoring observation does not match the current viewport snapshot"));
    }
    return editor::evaluate_authoring_observation(
        current.viewport.scene, query, options);
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
    if (agent_named_target_clarification_.has_value()) {
        const auto& pending = *agent_named_target_clarification_;
        AgentNamedTargetClarificationView view;
        view.prompt = pending.prompt;
        view.rationale = pending.rationale;
        view.source_revision = pending.source_revision;
        view.source_generation = pending.source_generation;
        view.stale = result.project_generation != pending.source_generation ||
            result.project_revision != pending.source_revision;
        view.candidates.reserve(pending.candidates.size());
        for (const auto& candidate : pending.candidates) {
            view.candidates.push_back({candidate.id, candidate.label});
        }
        result.agent_named_target_clarification = std::move(view);
    }
    result.agent_repair_ready = agent_repair_mode_;
    result.agent_checkpoint_loaded = agent_checkpoint_pending_.has_value();
    result.agent_checkpoint_rebased = agent_checkpoint_rebased_;
    if (agent_checkpoint_pending_.has_value()) {
        result.agent_checkpoint_task_id = agent_checkpoint_pending_->plan.id;
        result.agent_checkpoint_base_revision = agent_checkpoint_pending_->source_revision;
        result.agent_checkpoint_rebase_revision = agent_plan_base_revision_.value_or(
            agent_checkpoint_pending_->source_revision);
    }
    result.agent_checkpoint_directly_conflicting_steps =
        agent_checkpoint_directly_conflicting_steps_;
    result.agent_checkpoint_invalidated_steps = agent_checkpoint_invalidated_steps_;
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
